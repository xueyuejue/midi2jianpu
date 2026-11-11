// html_writer.cpp - 简谱 -> 自包含 HTML 渲染
//
// 排版坐标约定（一个音符是一列）：
//
//     ·  ·      <- 上八度点（.oct 固定 12px 高，无点时留空占位）
//     5  -  -   <- 数字与延音线同行（.core 用 flex row）
//     ·         <- 下八度点（.oct.low，紧贴数字下方，不会被歌词压住）
//     风        <- 歌词
//
// 早先 .note 用 flex-direction:column 却把 `-` 直接作为子元素，导致延音线
// 掉到数字下一行、整份谱子看起来像一堆方块；低八度点用 absolute 定位在
// .note 底部，有歌词时会压在字上。这两处都是纯排版问题，改用分层结构解决。
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/version.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace m2j {

static std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

// 时值 -> CSS class + 延音线条数 + 减时线条数
struct DurStyle {
    const char* cls;
    bool dotted;
    int extLines;   // 延音线数量：二分 1 条、附点二分 2 条、全音符 3 条
    int beams;      // 减时线数量：八分 1 条、十六分 2 条、三十二分 3 条
};

static DurStyle durStyleOf(double beats) {
    static const struct { double beats; DurStyle st; } table[] = {
        {0.125, {"thirtysecond", false, 0, 3}},
        {0.25,  {"sixteenth",    false, 0, 2}},
        {0.375, {"sixteenth",    true,  0, 2}},
        {0.5,   {"eighth",       false, 0, 1}},
        {0.75,  {"eighth",       true,  0, 1}},
        {1.0,   {"quarter",      false, 0, 0}},
        {1.5,   {"quarter",      true,  0, 0}},
        {2.0,   {"half",         false, 1, 0}},
        // 3 拍 / 6 拍在简谱里靠延音线条数表达（5 - - / 5 - - - - -），
        // 与文本谱 DUR_TABLE 的 "--" / 延长线写法一致，不画附点。
        // dotted 的语义是「需要在数字右侧补一个点」，只留给 0.375 / 0.75 / 1.5 拍。
        {3.0,   {"half",         false, 2, 0}},
        {4.0,   {"whole",        false, 3, 0}},
        {6.0,   {"whole",        false, 5, 0}},
    };
    double best = 1e9;
    DurStyle out{"quarter", false, 0, 0};
    for (auto& t : table) {
        double d = std::abs(beats - t.beats);
        if (d < best) { best = d; out = t.st; }
    }
    return out;
}

// 三连音的时值不能按普通时值表取近似值。
// 三连音八分是 1/3 拍，而时值表里最近的档位是 0.375（附点十六分），
// 于是一个「三连八分」被画成附点十六分——两条减时线外加一个附点，
// 懂简谱的人一眼就看出错。三连音的真实档位只有 1/3、1/6、1/12 拍。
static DurStyle tripletStyleOf(double beats) {
    double r = beats * 3.0;      // 1/3 拍 -> 1.0，1/6 拍 -> 0.5，1/12 拍 -> 0.25
    if (r < 0.34) return {"thirtysecond", false, 0, 3};
    if (r < 0.75) return {"sixteenth",    false, 0, 2};
    return {"eighth", false, 0, 1};
}

// 休止符的「可精确显示」档位：与 durStyleOf() 表一一对应，拍数 * 16 = units。
// 间隙不是这些档位时 durStyleOf() 只能取最近档 —— 1.75 拍画成 1.5 拍、
// 1.25 拍画成 1 拍，读者数出来的拍数凭空少 0.25。文本谱 v1.59.0 的
// exactRest 与 MusicXML v1.58.0 的 splitRestDuration() 都已精确拆段，
// HTML 是三端里唯一还按最近档近似的一端。
struct RestStep {
    long units;      // 以 1/16 拍为单位
    double beats;
};
static const RestStep kExactRestSteps[] = {
    {96, 6.0}, {64, 4.0}, {48, 3.0}, {32, 2.0}, {24, 1.5},
    {16, 1.0}, {12, 0.75}, {8, 0.5}, {6, 0.375}, {4, 0.25}, {2, 0.125},
};

// 把间隙按整数单位贪心拆成若干可精确显示的休止符。
// 返回 false 表示拆不干净（余量不足 1/8 拍、或 gap 不是 1/16 拍的整数倍），
// 调用方保留旧的最近档单列兜底 —— 与另外两端的兜底口径一致。
// 单段也返回 false：精确档素材走旧路径，逐字节不变。
static bool splitRestBeats(long gapTicks, int division, std::vector<double>& out) {
    out.clear();
    if (gapTicks <= 0 || division <= 0) return false;
    const long long scaled = 1LL * gapTicks * 16;
    if (scaled % division != 0) return false;
    long long rem = scaled / division;
    while (rem > 0) {
        bool picked = false;
        for (const auto& st : kExactRestSteps) {
            if (st.units <= rem) {
                out.push_back(st.beats);
                rem -= st.units;
                picked = true;
                break;
            }
        }
        if (!picked) { out.clear(); return false; }
    }
    return out.size() > 1;
}

// 生成一个音符的 HTML 列。
//
// 关键点：延音线不参与宽度计算（绝对定位挂在 .core 右侧），所以 .note 的
// 实际宽度由数字决定，八度点和歌词都对准数字中心——这是简谱的硬性要求，
// 否则 `1 - - -` 的低八度点会飘到整个音符的正中，偏离数字好远。
static std::string noteColumn(const std::string& numText, int highDots, int lowDots,
                              double beats, const std::string& lyric,
                              const std::string& extraClass = "",
                              const DurStyle* overrideStyle = nullptr) {
    DurStyle ds = overrideStyle ? *overrideStyle : durStyleOf(beats);

    std::ostringstream os;
    // 附点档位必须在数字右侧补一个点，否则画出来就是基础时值：
    // 0.75 拍看成 0.5 拍、1.5 拍看成 1 拍，读者数出来的拍数凭空少 1/3。
    // CSS 的 .dotted .num::after 早就写好了，这里把它接上。
    os << "<span class='note " << ds.cls << (ds.dotted ? " dotted" : "")
       << extraClass << "'";
    // --n 是延音线条数，用来给 .note 预留右侧空间
    if (ds.extLines > 0) os << " style='--n:" << ds.extLines << "'";
    os << ">";

    // 上八度点：固定高度占位，保证各行音符顶部对齐
    os << "<span class='oct'>";
    for (int i = 0; i < highDots; i++) os << "<i></i>";
    os << "</span>";

    // 数字（延音线绝对定位在其右侧，不撑宽）
    os << "<span class='core'><span class='num'>" << htmlEscape(numText) << "</span>";
    if (ds.extLines > 0) {
        os << "<span class='exts'>";
        for (int i = 0; i < ds.extLines; i++) os << "<span class='ext'>-</span>";
        os << "</span>";
    }
    os << "</span>";

    // 下八度点：紧跟数字下方、歌词之前
    os << "<span class='oct low'>";
    for (int i = 0; i < lowDots; i++) os << "<i></i>";
    os << "</span>";

    // 歌词：始终占位，保证同一行音符底部对齐
    os << "<span class='lyric'>" << htmlEscape(lyric) << "</span>";
    os << "</span>";
    return os.str();
}

// 把和弦标记挂到一个音符列的顶部。
//
// 不能另起一列：和弦标记若占宽度，同一拍的音符间距会被它撑开，
// 谱面看起来就是参差不齐的。这里塞进 .note 内部绝对定位，浮在数字上方，
// 与标记行里的和弦（小节起点那种）落在同一高度，视觉上是一条线。
static std::string withChord(const std::string& col, const std::string& chord) {
    if (chord.empty()) return col;
    size_t p = col.find('>');
    if (p == std::string::npos) return col;
    return col.substr(0, p + 1) +
           "<span class='chordx'>" + htmlEscape(chord) + "</span>" +
           col.substr(p + 1);
}

// 断音 / 重音 / 滑音记号同样浮在音符列里、不占宽度。
// 放在右上角而不是正中：上八度点占的是正中那 12px，两者会叠在一起。
static std::string withArticulation(const std::string& col, const std::string& art) {
    if (art.empty()) return col;
    size_t p = col.find('>');
    if (p == std::string::npos) return col;
    return col.substr(0, p + 1) +
           "<span class='artic'>" + htmlEscape(art) + "</span>" +
           col.substr(p + 1);
}

// 小节中途换力度档：同样浮在音符列顶部、不占宽度。
//
// 不能塞进标记行——标记行是一小节一条，写进去等于把「第 2 拍变弱」说成
// 「整小节都弱」，比不标更误导。挂到出事那个音头上才和听到的一致。
// 与和弦撞到同一列时加 .pair 往左让半个身位，两个标记各占半边。
static std::string withDyn(const std::string& col, const std::string& dyn) {
    if (dyn.empty()) return col;
    size_t p = col.find('>');
    if (p == std::string::npos) return col;
    const bool paired = col.find("chordx") != std::string::npos;
    return col.substr(0, p + 1) +
           "<span class='dynx" + std::string(paired ? " pair" : "") + "'>" +
           htmlEscape(dyn) + "</span>" + col.substr(p + 1);
}

// 生成一段间隙的休止符列：优先按精确档位之和拆列，拆不干净才退回旧单列。
// 拆列时和弦 / 力度标记只挂第一列 —— 标记标的是音头位置，挂到每一列
// 会把「第 2 拍变弱」重复标成好几次。
static std::string restColumns(long gapTicks, int division, double restBeats,
                               const std::string& chord, const std::string& dyn) {
    std::vector<double> parts;
    if (!splitRestBeats(gapTicks, division, parts))
        return withDyn(withChord(noteColumn("0", 0, 0, restBeats, ""), chord), dyn);
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        std::string col = noteColumn("0", 0, 0, parts[i], "");
        if (i == 0) col = withDyn(withChord(col, chord), dyn);
        out += col;
    }
    return out;
}

// 生成和弦列：同一时刻发声的多个音竖向叠写，共用一个时值线。
//
// 简谱里和弦就是上下对齐的几行数字。若不叠排而顺序输出，一个三音和弦会被
// 画成三个连续音符（4/4 小节看着像有 12 拍），完全误导。
static std::string chordColumn(const std::vector<std::pair<std::string, std::pair<int, int>>>& tones,
                               double beats, const std::string& lyric) {
    DurStyle ds = durStyleOf(beats);

    std::ostringstream os;
    os << "<span class='note chordcol " << ds.cls << "'";
    if (ds.extLines > 0) os << " style='--n:" << ds.extLines << "'";
    os << "><span class='core'><span class='stack'>";

    for (size_t ti = 0; ti < tones.size(); ti++) {
        const auto& tn = tones[ti];
        // 附点只画在最下面那个音右侧（与减时线只画最低音同口径）：
        // .dotted 挂在单个 tone 上，CSS 的 .dotted .num::after 就只命中它；
        // 若挂在整列上，叠写的每个音都会各画一个点，看起来像一串附点。
        const bool lastTone = (ti + 1 == tones.size());
        os << "<span class='tone" << (lastTone && ds.dotted ? " dotted" : "") << "'>";
        os << "<span class='oct'>";
        for (int i = 0; i < tn.second.first; i++) os << "<i></i>";
        os << "</span>";
        os << "<span class='num'>" << htmlEscape(tn.first) << "</span>";
        os << "<span class='oct low'>";
        for (int i = 0; i < tn.second.second; i++) os << "<i></i>";
        os << "</span>";
        os << "</span>";
    }

    os << "</span>";
    if (ds.extLines > 0) {
        os << "<span class='exts'>";
        for (int i = 0; i < ds.extLines; i++) os << "<span class='ext'>-</span>";
        os << "</span>";
    }
    os << "</span>";
    os << "<span class='lyric'>" << htmlEscape(lyric) << "</span>";
    os << "</span>";
    return os.str();
}

// 把属性注入音符列的 <span class='note ...'> 开标签。
//
// 试听播放器要高亮「此刻正在响的那个音」，就得让每列知道自己的起点 tick。
// 属性不能挂在包装层上（withChord / withDyn / withArticulation 都会在外面
// 再套一层 span），所以这里只找第一个 class='note 的开标签——那些包装层的
// class 都不是 note，不会误命中。
static std::string withTick(const std::string& col, long tick) {
    const std::string key = "<span class='note";
    size_t p = col.find(key);
    if (p == std::string::npos) return col;
    size_t gt = col.find('>', p);
    if (gt == std::string::npos) return col;
    return col.substr(0, gt) + " data-t='" + std::to_string(tick) + "'" + col.substr(gt);
}

// 试听播放器：工具条 + Web Audio 合成 + 当前音符高亮 + 点击定位起播。
//
// 为什么内联而不是外链 JS：导出的 HTML 要能离线双击打开、直接打印，也不能
// 依赖 CDN。旋律用三角波 + 指数包络合成，够听清音高与节奏；不做音色还原，
// 那是 MIDI 合成器的活。播放数据是 [起点tick, 时长tick, 音高, 轨号, 力度]
// 的扁平数组，配一张 tempo 分段表在浏览器里换算成秒（变速曲目才对得上）。
static std::string playerBlock(const std::string& playJson) {
    return "<div id='playerbar'>"
           "<button id='pp' type='button'>▶ 试听</button>"
           "<label class='spd'>速度 <select id='spd'>"
           "<option value='0.5'>0.5×</option>"
           "<option value='0.75'>0.75×</option>"
           "<option value='1' selected>1×</option>"
           "<option value='1.25'>1.25×</option>"
           "<option value='1.5'>1.5×</option>"
           "</select></label>"
           "<span id='pstat'></span>"
           "<span class='hint'>点谱面上任一音符即从该处起播</span>"
           "</div>"
           "<script>\n"
           "var M2J_PLAY = " + playJson + ";\n"
           R"JS((function () {
  var btn = document.getElementById('pp');
  var sel = document.getElementById('spd');
  var stat = document.getElementById('pstat');
  var P = M2J_PLAY;
  if (!btn || !P || !P.notes || !P.notes.length) return;
  var div = P.division > 0 ? P.division : 480;
  var tempo = (P.tempo && P.tempo.length) ? P.tempo : [[0, 500000]];

  function secAt(tick) {
    var s = 0, prev = 0, us = tempo[0][1] > 0 ? tempo[0][1] : 500000;
    for (var i = 0; i < tempo.length; i++) {
      var seg = tempo[i];
      if (seg[0] > tick) break;
      s += (seg[0] - prev) / div * us / 1e6;
      prev = seg[0];
      if (seg[1] > 0) us = seg[1];
    }
    return s + (tick - prev) / div * us / 1e6;
  }

  var lastEnd = 0;
  for (var i = 0; i < P.notes.length; i++) {
    var n = P.notes[i];
    if (n[0] + n[1] > lastEnd) lastEnd = n[0] + n[1];
  }

  var ctx = null, master = null, nodes = [], timers = [], raf = 0;
  var evts = [], cursor = 0, active = [], t0 = 0, speed = 1, playing = false;

  function mmss(sec) {
    var m = Math.floor(sec / 60), s = Math.round(sec - m * 60);
    if (s === 60) { m += 1; s = 0; }
    return m + ':' + (s < 10 ? '0' : '') + s;
  }
  if (stat) stat.textContent = P.notes.length + ' 个音 · 约 ' + mmss(secAt(lastEnd));

  function clearMarks() {
    for (var i = 0; i < active.length; i++) {
      for (var k = 0; k < active[i].els.length; k++)
        active[i].els[k].classList.remove('playing');
    }
    active = [];
  }

  function hush() {
    for (var i = 0; i < nodes.length; i++) {
      try { nodes[i].stop(0); } catch (e) {}
    }
    nodes = [];
    for (var j = 0; j < timers.length; j++) clearTimeout(timers[j]);
    timers = [];
    if (raf) { cancelAnimationFrame(raf); raf = 0; }
  }

  function stop() {
    playing = false;
    hush();
    clearMarks();
    cursor = 0;
    if (btn) btn.textContent = '\u25b6 试听';
  }

  function mark(ev) {
    for (var k = 0; k < ev.els.length; k++) ev.els[k].classList.add('playing');
    active.push(ev);
    // 只在这一列已经滚出视口时才滚回来，否则播放时页面会一直上下跳
    if (ev.els.length) {
      var el = ev.els[0], r = el.getBoundingClientRect();
      if (r.top < 60 || r.bottom > (window.innerHeight - 40)) {
        try { el.scrollIntoView({ block: 'center', behavior: 'smooth' }); }
        catch (e) { el.scrollIntoView(); }
      }
    }
  }

  // 力度 -> 峰值增益。note-on velocity 在解析层早就有了（演奏法判定要用它），
  // 以前只拿它判重音、没送进播放层，于是重音和弱起听起来一样响。线性映射
  // 让强弱能听出来；缺第 5 位（老数据）按 96 处理，等于原来的固定音量。
  function peak(vel) {
    var v = (vel > 0 && vel <= 127) ? vel : 96;
    return 0.04 + 0.26 * v / 127;
  }

  function tone(at, dur, pitch, vel) {
    var o = ctx.createOscillator(), g = ctx.createGain();
    o.type = 'triangle';
    o.frequency.value = 440 * Math.pow(2, (pitch - 69) / 12);
    var end = at + Math.max(dur, 0.06);
    g.gain.setValueAtTime(0.0001, at);
    g.gain.exponentialRampToValueAtTime(peak(vel), at + 0.012);
    g.gain.exponentialRampToValueAtTime(0.0001, end);
    o.connect(g);
    g.connect(master);
    o.start(at);
    o.stop(end + 0.02);
    nodes.push(o);
  }

  function tick() {
    if (!playing) return;
    var now = ctx.currentTime - t0;
    while (cursor < evts.length && evts[cursor].start <= now) mark(evts[cursor++]);
    for (var i = active.length - 1; i >= 0; i--) {
      if (active[i].end <= now) {
        for (var k = 0; k < active[i].els.length; k++)
          active[i].els[k].classList.remove('playing');
        active.splice(i, 1);
      }
    }
    if (now > evts.length && evts.length && now > evts[evts.length - 1].end + 0.25) {
      stop();
      return;
    }
    raf = requestAnimationFrame(tick);
  }

  function start(from) {
    stop();
    if (!ctx) {
      var AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) { if (stat) stat.textContent = '此浏览器不支持 Web Audio，无法试听'; return; }
      ctx = new AC();
      master = ctx.createGain();
      master.gain.value = 0.6;
      master.connect(ctx.destination);
    }
    if (ctx.state === 'suspended') ctx.resume();
    speed = parseFloat(sel && sel.value) || 1;
    // from = 起播 tick：0 表示从曲子开头（保留弱起前的空白），点谱面时传该音
    // 的起点，于是「从这一小节开始听」不用真的等前面几十秒。
    from = from > 0 ? from : 0;
    var s0 = secAt(from);
    // 每个音一个事件；同一 tick 的多个音（和弦 / 多轨）共用同一批 DOM 列
    evts = [];
    for (var i = 0; i < P.notes.length; i++) {
      var n = P.notes[i];
      if (n[0] < from) continue;
      var s = (secAt(n[0]) - s0) / speed;
      var e = (secAt(n[0] + Math.max(n[1], 1)) - s0) / speed;
      var els = document.querySelectorAll("[data-t='" + n[0] + "']");
      evts.push({ start: s, end: e, tick: n[0], pitch: n[2],
                  vel: n.length > 4 ? n[4] : 96, els: els });
    }
    if (!evts.length) return;        // 点在最后一个音之后：没有可播的内容
    evts.sort(function (a, b) { return a.start - b.start || a.pitch - b.pitch; });
    cursor = 0;
    active = [];
    t0 = ctx.currentTime + 0.12;
    for (var j = 0; j < evts.length; j++) {
      var ev = evts[j];
      (function (ev) {
        var at = t0 + ev.start, dur = ev.end - ev.start;
        timers.push(setTimeout(function () { tone(at, dur, ev.pitch, ev.vel); },
                               Math.max(0, (at - ctx.currentTime) * 1000)));
      })(ev);
    }
    playing = true;
    if (btn) btn.textContent = '\u25a0 停止';
    raf = requestAnimationFrame(tick);
  }

  // 点击谱面任一音符从该处起播。用事件委托而不是逐个绑定：这段脚本插在
  // <body> 最前面，执行时下面的音符列还没解析出来，逐个绑定会一个都绑不上。
  document.addEventListener('click', function (ev) {
    var el = ev.target;
    while (el && el.getAttribute && !el.getAttribute('data-t')) {
      if (!el.parentNode) { el = null; break; }
      el = el.parentNode;
    }
    if (!el || !el.getAttribute) return;
    var tk = parseInt(el.getAttribute('data-t'), 10);
    if (!isNaN(tk)) start(tk);
  });

  btn.addEventListener('click', function () { if (playing) stop(); else start(0); });
  if (sel) sel.addEventListener('change', function () { if (playing) start(0); });
  window.addEventListener('beforeunload', hush);
})();
)JS"
           "</script>";
}

std::string renderHtml(const MidiData& data, const KeyEstimate& key,
                       const std::string& title,
                       const std::vector<LyricLine>* lyricLines,
                       bool player) {
    std::string displayTitle = title.empty() ? "简谱" : title;
    const std::string lyricLabel = lyricSourceLabel(data, lyricLines);

    std::ostringstream body;
    // 小节网格（含弱起）。三个输出模块共用同一套换算，见 notation.hpp 的 BarGrid
    const BarGrid grid = barGridOf(data);
    const long barLen = grid.barLen;
    // 调性也和网格一样是「按位置查表」：中途转调后同一个 MIDI 音高的唱名
    // 要随所在小节重算，页眉与谱面标记都由它给出。
    const KeyMap keys(data, key);
    // 力度 / 渐强渐弱 / 断音 / 滑音 / 段落：三个输出层共用同一份判定结果。
    const ExpressionMap expr(data);

    // 和弦检测。按「和声窗」而不是整小节（4/4 半小节、6/8 附点四分），
    // 所以索引键是准确的 tick，同一小节里可以有多个和弦。
    auto chords = detectChords(data, key);
    std::map<long, std::string> chordAt;
    for (const auto& c : chords) chordAt[c.startTick] = c.name;

    // 速度标记：折叠后的记号（阶跃 / 渐快 / 渐慢），再把「记号落在第几小节」
    // 预先算成一张表。同一个小节里有多个记号时保留第一个（后面的会在
    // 标记行里挤成一团，而且实用上也不会有一小节里变两次速的谱子）。
    const std::vector<TempoMark> tempoMarks = tempoMarksOf(data);
    std::map<long, size_t> tempoAtBar;
    for (size_t i = 0; i < tempoMarks.size(); i++) {
        const long b = grid.barIndex(tempoMarks[i].tick);
        if (tempoAtBar.find(b) == tempoAtBar.end()) tempoAtBar[b] = i;
    }

    // 歌词分配：HTML 的连音线弧线和 MusicXML 的 syllabic/slur 共用同一份结果。
    // 早先这里自己拆字、自己算「第几个音领第几个字」，导出 MusicXML 时整份
    // 歌词就丢了；现在统一走 notation.cpp 的 assignLyrics。
    LyricAssignment lyrics;
    auto lyricForNote = [&](long tick) -> std::string {
        auto lit = lyrics.at.find(tick);
        return lit == lyrics.at.end() ? std::string() : lit->second.text;
    };

    // 弯音修正（只统计同通道）
    auto bendOffset = [&](long start, long end, int channel) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.channel == channel && pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * pb.range;
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks, n.channel);
        if (std::abs(off) >= 0.5) n.pitch += (int)std::round(off);
        byTrack[n.track].push_back(n);
    }

    // 旋律轨：和弦标记与歌词都只给它。判定见 notation.cpp 的 pickMelodyTrack——
    // 有歌词时「LRC 行首能否对齐」是最强证据，比"音符最多"可靠得多
    // （音符最多的那条轨在真实工程里通常是鼓或柱式和弦伴奏）。
    int mainTrack = pickMelodyTrack(data, lyricLines);

    // ---- 歌词对齐 ----
    // 一行 LRC 常常跨两小节、音多于字，要按行内音数均匀铺开；MIDI 自带的
    // 0x05 歌词则每个音节都带精确 tick，不能抹平。两种来源由 resolveLyrics
    // 统一成同一份 LyricAssignment（显式 --lyrics 优先，否则自动用内置歌词），
    // 下面画连音线、写歌词字都只读这份结果。
    if (mainTrack >= 0) lyrics = resolveLyrics(data, lyricLines, mainTrack);

    // 同轨复调：一条轨常常同时装着两个声部（钢琴左右手写在同一轨、伴奏垫在
    // 人声轨下面）。简谱一行写不下两个声部，所以按声部拆成独立谱行；
    // 单声部轨（绝大多数文件）与旧版输出逐字节一致。
    struct PartView {
        int track = 0;
        int voice = 0;
        int voiceCount = 1;
        int channel = -1;
        std::vector<NoteEvent> notes;
    };
    std::vector<PartView> parts;
    for (auto& [t, ns] : byTrack) {
        const std::vector<int> vo = splitVoices(
            ns, 3, [&](const NoteEvent& n) { return expr.isGraceNote(n.startTicks, n.channel); });
        int voiceCount = 0;
        for (int v : vo) voiceCount = std::max(voiceCount, v + 1);
        if (voiceCount <= 1) {
            const int ch = dominantChannel(ns);
            parts.push_back({t, 0, 1, ch, std::move(ns)});
            continue;
        }
        for (int v = 0; v < voiceCount; v++) {
            PartView pv;
            pv.track = t;
            pv.voice = v;
            pv.voiceCount = voiceCount;
            for (size_t i = 0; i < ns.size(); i++)
                if (vo[i] == v) pv.notes.push_back(ns[i]);
            pv.channel = dominantChannel(pv.notes);
            if (!pv.notes.empty()) parts.push_back(std::move(pv));
        }
    }

    // ---- 试听播放器的数据 ----
    // 直接从 parts 取音符：它就是谱面上真正画出来的那一份（已按声部拆开、
    // 已做同音合并前的基础集），播放与谱面不会各说各话。
    // 只给 --player 生成；不带开关时 playJson 保持空串，输出逐字节不变。
    std::string playJson;
    if (player) {
        std::ostringstream pj;
        pj << "{\"division\":" << (data.division > 0 ? data.division : 480)
           << ",\"tempo\":[";
        bool firstSeg = true;
        for (const auto& tp : data.tempoChanges) {
            if (!(tp.bpm > 0)) continue;
            if (!firstSeg) pj << ",";
            pj << "[" << tp.tick << ","
               << (long)std::llround(60000000.0 / tp.bpm) << "]";
            firstSeg = false;
        }
        if (firstSeg) pj << "[0,500000]";     // 没有 set_tempo 就是 120 BPM
        pj << "],\"notes\":[";
        bool firstNote = true;
        for (const auto& pv : parts) {
            for (const auto& n : pv.notes) {
                if (n.durationTicks <= 0) continue;
                int vel = n.velocity;
                if (vel < 1) vel = 1;
                if (vel > 127) vel = 127;
                if (!firstNote) pj << ",";
                pj << "[" << n.startTicks << "," << n.durationTicks << ","
                   << n.pitch << "," << pv.track << "," << vel << "]";
                firstNote = false;
            }
        }
        pj << "]}";
        playJson = pj.str();
    }

    for (size_t pi = 0; pi < parts.size(); pi++) {
        const int t = parts[pi].track;
        const auto& ns = parts[pi].notes;
        const int partChannel = parts[pi].channel;
        const bool isMain = (t == mainTrack && parts[pi].voice == 0);
        // 速度标记属于整份谱，只在第一行谱上印一次。每轨都印一遍是噪音，
        // 而且同一页上会出现好几个一模一样的 ♩=90。
        const bool showTempo = (pi == 0);

        // ---- 同音合并：连续同音且首尾相接时并成一个长音 ----
        // 不合并的话同音重复会被画成 `5 - 5 -`，而简谱规范应写成 `5 - - -`。
        //
        // 但这里必须排除多声部：C 和弦的 5 和下一个 G 和弦的 5 音高相同、
        // 时间也相接，合并后 G 和弦就少了一个音。只在「该时刻只有一个音」时
        // 才允许合并。
        //
        // 也不能跨小节线合并：真实演奏里同音相邻几乎都是两次发音而不是延音，
        // 合并会把小节线上的音吞掉（第 15 小节开头的 6 变成 `-`）。
        auto triplets0 = detectTriplets(ns, data.division);
        std::map<long, int> onsetCount;
        for (const auto& n : ns) onsetCount[n.startTicks]++;
        auto isSingle = [&](long tick) {
            auto cit = onsetCount.find(tick);
            return cit == onsetCount.end() || cit->second <= 1;
        };

        std::vector<NoteEvent> merged;
        std::vector<bool> trip;
        for (size_t i = 0; i < ns.size(); i++) {
            if (i > 0 && !triplets0[i] && !triplets0[i - 1] && !merged.empty() &&
                ns[i].pitch == merged.back().pitch &&
                ns[i].startTicks == merged.back().startTicks + merged.back().durationTicks &&
                isSingle(ns[i].startTicks) && isSingle(merged.back().startTicks) &&
                grid.barIndex(ns[i].startTicks) == grid.barIndex(merged.back().startTicks)) {
                merged.back().durationTicks += ns[i].durationTicks;
                merged.back().durationSeconds += ns[i].durationSeconds;
                continue;
            }
            merged.push_back(ns[i]);
            trip.push_back(triplets0[i]);
        }

        // 只有旋律轨、且真的存在一字多音时才抬高和弦带：其余谱面的行距
        // 保持原样，不因为一个新功能把整页撑高。
        const bool hasSlur = (isMain && !lyrics.melismas.empty());
        body << "<div class='track'><h3>";
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) body << htmlEscape(it->second);
        else body << ((t == mainTrack) ? "旋律轨" : "伴奏轨 " + std::to_string(t));
        // 复调轨按声部拆成多行：标题要标清楚这是第几声部，否则同一轨名
        // 连着出现三四次，读者会以为是导出错了。
        if (parts[pi].voiceCount > 1)
            body << " <span class='voiceTag'>声部 " << (parts[pi].voice + 1) << "</span>";
        body << " <small>(" << merged.size() << " 音符)</small></h3><div class='staff"
             << (hasSlur ? " hasslur" : "") << "'><div class='row'>";

        // 每行放多少小节：按 16 拍折算（4/4 即 4 小节一行），与简谱歌本的习惯一致。
        // 原来靠 flex 自动换行，每行 3~4 小节不等，整页行宽参差。
        double beatsPerBar = (double)barLen / data.division;
        int measuresPerRow = 1;
        if (beatsPerBar > 0) {
            int n = (int)std::lround(16.0 / beatsPerBar);
            measuresPerRow = n < 1 ? 1 : (n > 8 ? 8 : n);
        }

        // ---- 按小节切分 ----
        // 简谱的 `-` 是「延长一拍」，不能跨过小节线：一个 6 拍的长音若整个画在
        // 一个小节里，该小节就凭空多出 2 拍，小节线位置随之全部错位。
        // 切分后首段照常写数字，后续段（cont）只画 `-`。
        struct HNote { NoteEvent n; bool cont; bool trip; };
        std::vector<HNote> seq;
        for (size_t i = 0; i < merged.size(); i++) {
            const auto& n = merged[i];
            long s = n.startTicks;
            long e = s + std::max<long>(n.durationTicks, 1);
            bool cont = false;
            while (s < e) {
                long barEnd = grid.barEnd(grid.barIndex(s));
                long segEnd = std::min(e, barEnd);
                if (segEnd <= s) segEnd = e;   // 防御，避免死循环
                NoteEvent seg = n;
                seg.startTicks = s;
                seg.durationTicks = segEnd - s;
                seq.push_back({seg, cont, cont ? false : (i < trip.size() ? trip[i] : false)});
                s = segEnd;
                cont = true;
            }
        }

        // 按时间顺序重排分段：上面是按「音符」逐一切分小节的，于是同一音符跨
        // 小节会被拆成多个分段，不同音符的分段便按「音符」交错出现
        // （note1_bar0, note1_bar1, note2_bar0, note2_bar1 …）。而下面的渲染循环
        // 假设小节号单调推进，一旦遇到后面的音符把 bar 带回到前一小节
        // （bar 1 → bar 0），补齐空小节的循环就会把已经画过的小节再画一遍——
        // 钢琴轨于是冒出十几二十个小节，而文本谱 / MusicXML 只有 5 小节，三端
        // 结构就此分叉。这里按 startTicks 稳定排序，让分段以「小节」为主序、
        // 同小节内保留原有相对顺序，渲染循环便不会再回退重画。
        std::stable_sort(seq.begin(), seq.end(),
                         [](const HNote& a, const HNote& b) {
                             return a.n.startTicks < b.n.startTicks;
                         });

        // ---- 同拍分组：同一 startTicks 的音符属于同一个和弦 ----
        std::vector<std::vector<const HNote*>> groups;
        for (const auto& s : seq) {
            if (!groups.empty()) {
                const HNote* prev = groups.back().back();
                if (s.cont == prev->cont && s.n.startTicks == prev->n.startTicks) {
                    groups.back().push_back(&s);
                    continue;
                }
            }
            groups.push_back({&s});
        }

        int curBar = -1;
        long prevEnd = 0;
        std::string lastDyn;
        std::string lastChord;
        size_t lastTempoIdx = (size_t)-1;
        bool inTriplet = false;

        // 和弦游标：按 tick 顺序推进。取「不晚于 tick 的最后一个还没标过的
        // 和弦」，于是小节内换和弦、以及和弦落在两个音之间（旋律正拖长音）
        // 这两种情况都不会漏标。
        auto chordIt = chordAt.begin();
        auto chordMarkFor = [&](long tick) -> std::string {
            if (!isMain) return std::string();
            std::string label;
            while (chordIt != chordAt.end() && chordIt->first <= tick) {
                if (chordIt->second != lastChord) label = chordIt->second;
                ++chordIt;
            }
            if (!label.empty()) lastChord = label;
            return label;
        };

        // 小节中途换力度档：与文本谱 / MusicXML 共用 dynamicStepsInside 的判定
        // （落在小节内、且不属于任何渐变段），先整份收出来，再按 tick 游标挂到
        // 出事那个音头上。旧实现只在 openBar 里取一次 dynamicAt，中途换档在
        // HTML 里同样整条丢失：第 2 拍的 ff→p 既不显示，后面还会把 p 顶到下一
        // 小节，谱面与听到的力度差三拍。渐变段内部的档位不在这里印。
        std::vector<ExpressionMark> midDyn;
        {
            long maxTick = 0;
            for (const auto& n : merged)
                maxTick = std::max(maxTick, n.startTicks + std::max<long>(n.durationTicks, 1));
            const int lastBar = (int)grid.barIndex(maxTick);
            for (int b = 0; b <= lastBar; b++)
                for (const auto& m : expr.dynamicStepsInside(grid.barStart(b),
                                                             grid.barEnd(b), partChannel))
                    midDyn.push_back(m);
        }
        size_t midDynIdx = 0;
        auto dynMarkFor = [&](long tick) -> std::string {
            std::string label;
            while (midDynIdx < midDyn.size() && midDyn[midDynIdx].tick <= tick) {
                if (midDyn[midDynIdx].dynamic != lastDyn) label = midDyn[midDynIdx].dynamic;
                ++midDynIdx;
            }
            if (!label.empty()) lastDyn = label;
            return label;
        };

        // 开一个小节：上面一行放标记（小节号 / 和弦 / 力度），下面才是音符。
        //
        // 早先把这三样直接塞进音符的 flex 流里，结果它们全挤在音符基线上：
        // 「Cmaj7 ff 1̇」看起来像一个词的三个字母，和弦与力度根本分不开。
        // 分成上下两层后，标记有固定的 17px 高度，各小节音符行也随之顶部对齐。
        auto openBar = [&](int bar, long firstTick, int channel) {
            long barStart = grid.barStart(bar);
            if (prevEnd < barStart) prevEnd = barStart;
            body << "<span class='measure'><span class='marks'>";

            // 小节号：长谱子靠它定位，从第 2 小节起显示。
            // 弱起小节不编号，第一个完整小节才是 1（见 BarGrid::measureNumber）
            if (bar > 0) body << "<span class='barno'>" << grid.measureNumber(bar) << "</span>";

            // 中途变拍号：把新拍号标在变化那一小节的标记行。
            // 页眉的摘要说明「什么时候变」，这里说明「从哪一栏开始按新拍子数」。
            if (showTempo && grid.meterChangedAt(bar))
                body << "<span class='meter'>" << grid.sigForBar(bar).toString() << "</span>";

            // 中途转调：标在换调那一小节。页眉写完整链条，这里指出
            // 「从哪一栏起按新调唱」，与拍号 / 速度标记同在标记行。
            if (keys.keyChangedAtBar(bar, grid))
                body << "<span class='keychg'>转 "
                     << "1=" << keyName(keys.keyAtBar(bar, grid)) << "</span>";

            // 中途变速：标在速度变化落在的那一小节。单一速度已经写在页眉，
            // 谱面上再印一遍纯属噪音，所以只在有两个以上记号时显示。
            //
            // 旧实现是「每个小节采样一次当前速度」，遇到渐慢（MIDI 里几十个
            // 连续 set_tempo）会每一小节都冒出一个 ♩=116 / ♩=112 / ♩=108，
            // 满屏无意义数字。现在改走 tempoMarksOf 的折叠结果：阶跃标
            // `♩=90`，渐变标一次 `rit. 120→60`（只标起点，不再逐小节采样）。
            if (showTempo && tempoMarks.size() > 1) {
                auto tm = tempoAtBar.find(bar);
                if (tm != tempoAtBar.end() && tm->second != lastTempoIdx) {
                    const TempoMark& m = tempoMarks[tm->second];
                    if (m.kind == TempoMarkKind::Ritardando) {
                        body << "<span class='tempo ramp'>rit. " << m.bpm
                             << "→" << m.toBpm << "</span>";
                    } else if (m.kind == TempoMarkKind::Accelerando) {
                        body << "<span class='tempo ramp'>accel. " << m.bpm
                             << "→" << m.toBpm << "</span>";
                    } else {
                        body << "<span class='tempo'>♩=" << m.bpm << "</span>";
                    }
                    lastTempoIdx = tm->second;
                }
            }


            // 力度记号按通道取、变化时才写。分级表由 ExpressionMap 提供，
            // 与文本谱、MusicXML 完全一致（旧版这里自己写了一套阈值）。
            const std::string dyn = expr.dynamicAt(firstTick, channel);
            if (!dyn.empty() && lastDyn != dyn) {
                body << "<span class='dyn'>" << dyn << "</span>";
                lastDyn = dyn;
            }
            // 段落名是整份谱的全局标记，只印第一行谱；渐强渐弱是通道级
            // 演奏表情，每一行谱都按自己的主导通道各印一次。旧版两者共用
            // showTempo（= 第一行谱），第二行起的 cresc./dim. 全被吞掉。
            for (const auto& m : expr.inRange(grid.barStart(bar), grid.barEnd(bar), channel)) {
                if (showTempo && !m.section.empty())
                    body << "<span class='section'>" << htmlEscape(m.section) << "</span>";
                if (m.wedge > 0) body << "<span class='wedge'>cresc.</span>";
                else if (m.wedge < 0) body << "<span class='wedge'>dim.</span>";
            }
            body << "</span><span class='notes'>";   // 标记行结束，音符行开始
        };

        // ---- 一字多音的连音线（slur）----
        // 一个字拖着好几个音唱时，简谱要在这些音上方画一条弧线。做法是
        // 把「同组且连续输出」的音符列攒进 slurBuf，组结束（换字、换小节、
        // 休止）时一次性包进 <span class='sgrp'>，用 ::before 画一条跨全组的
        // 弧线——和减时线组 bgrp 是同一个思路。
        //
        // 不能给每个音符各画一段弧：那样得到的是一串波浪而不是一条连音线；
        // 也不能用 JS 量坐标：HTML 要能离线打开、直接打印。
        std::vector<std::string> slurBuf;
        int curSlur = 0;
        auto flushSlur = [&]() {
            if (slurBuf.empty()) return;
            // 单个音画不出弧线（跨小节被切断时就剩一列），直接吐出去
            if (slurBuf.size() >= 2) {
                body << "<span class='sgrp'>";
                for (const auto& h : slurBuf) body << h;
                body << "</span>";
            } else {
                body << slurBuf.front();
            }
            slurBuf.clear();
        };
        auto emitWithGroup = [&](const std::string& colHtml, int g) {
            if (g != curSlur) { flushSlur(); curSlur = g; }
            if (g > 0) slurBuf.push_back(colHtml);
            else body << colHtml;
        };
        // forceBreak 给休止符用：歌词拖过休止时不把「0」也圈进连音线
        auto emit = [&](const std::string& colHtml, long tick, bool forceBreak = false) {
            emitWithGroup(colHtml, (hasSlur && !forceBreak) ? lyrics.groupAt(tick) : 0);
        };

        // ---- 减时线连通（beam）----
        // 简谱的减时线按「拍」连通：一拍里的两个八分音符共用一条横线，
        // 不是各画一小段。早先每个 .num 各带一条 border-bottom，被音符间距
        // 切开，八分音符看起来像一串断线，三连音更是三个孤立的短线。
        //
        // 做法：把能连成一条线的音符先攒进 beamBuf，遇到断点（换小节、休止、
        // 长音、三连音组结束）才一次性包进 <span class='bgrp'>。
        std::vector<std::string> beamBuf;
        int beamLevel = 0;
        int beamGroup = 0;      // 减时线组所属的一字多音组，避免一条线跨两个字
        long beamPrevEnd = -1;
        long beamBeat = -1;
        int beamBar = -1;
        bool beamTrip = false;

        auto flushBeam = [&]() {
            if (beamBuf.empty()) return;
            std::ostringstream g;
            g << "<span class='bgrp";
            if (beamLevel > 1) g << " b" << beamLevel;
            g << "'>";
            for (const auto& h : beamBuf) g << h;
            g << "</span>";
            emitWithGroup(g.str(), beamGroup);
            beamBuf.clear();
            beamLevel = 0;
            beamGroup = 0;
            beamPrevEnd = -1;
        };

        bool firstNote = true;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const auto& grp = groups[gi];
            const NoteEvent& n = grp[0]->n;
            const bool cont = grp[0]->cont;
            int bar = (int)grid.barIndex(n.startTicks);

            // 倚音（装饰音）：整组跳过，只在它后面的主音上用小字上标呈现。
            // 否则倚音会自己占一格，把主音挤到下一拍，整行节奏错位。
            if (expr.isGraceNote(n.startTicks, n.channel)) {
                prevEnd = std::max(prevEnd, n.startTicks + n.durationTicks);
                firstNote = false;
                continue;
            }

            if (bar != curBar) {
                flushBeam();                                  // 小节线截断减时线
                flushSlur();                                  // 小节线同样截断连音线
                curSlur = 0;                                  // 跨小节的组各画各的一段弧

                // 跨小节收口：上一小节末尾若还有空拍（上一个音中途结束、
                // 下一音落在下一小节正拍），先把这段休止补进上一小节。否则
                // 后面 openBar 把 prevEnd 抬到新小节起点后，这段间隙会被整段
                // 吞掉——文本谱 / MusicXML 都按 emitRestUntil / splitRestDuration
                // 把这段尾休止补出来，HTML 若丢则三端不一致（parts/和弦素材
                // 里很常见：一句落在第 1 拍、下一句从下一小节正拍起）。
                if (curBar >= 0) {
                    long prevBarEnd = grid.barEnd(curBar);
                    long tailGap = prevBarEnd - prevEnd;
                    if (tailGap >= restGateTicks(data.division)) {
                        double tailBeats = (double)tailGap / data.division;
                        emit(restColumns(tailGap, data.division, tailBeats,
                                         chordMarkFor(prevEnd), dynMarkFor(prevEnd)),
                             prevEnd, true);
                        prevEnd = prevBarEnd;                 // 收口，避免新小节再补一次
                    }
                }

                if (curBar >= 0) body << "</span></span>";   // 收尾上一个小节的音符行与小节

                // 补齐开头与中间完全空的小节。
                // 原来这里直接跳过，谱面第一小节标号是「5」、中途还会跳号，
                // 和 MusicXML（那边是补齐的）对不上，也不像一份能照着唱的谱子。
                int from = (curBar >= 0) ? curBar + 1 : 0;
                for (int b = from; b < bar; b++) {
                    if (b > 0 && b % measuresPerRow == 0) body << "</div><div class='row'>";
                    body << "<span class='measure'><span class='marks'>";
                    if (b > 0) body << "<span class='barno'>" << grid.measureNumber(b) << "</span>";
                    if (showTempo && grid.meterChangedAt(b))
                        body << "<span class='meter'>" << grid.sigForBar(b).toString() << "</span>";
                    if (keys.keyChangedAtBar(b, grid))
                        body << "<span class='keychg'>转 "
                             << "1=" << keyName(keys.keyAtBar(b, grid)) << "</span>";
                    // 段落标记也可能落在纯休止的前奏小节上；渐变按本行主导通道
                    // 取，与正常小节的 openBar 保持同一口径（不能用 n.channel，
                    // 它可能是同轨混进来的伴奏通道）。
                    for (const auto& m : expr.inRange(grid.barStart(b), grid.barEnd(b), partChannel)) {
                        if (showTempo && !m.section.empty())
                            body << "<span class='section'>" << htmlEscape(m.section) << "</span>";
                        if (m.wedge > 0) body << "<span class='wedge'>cresc.</span>";
                        else if (m.wedge < 0) body << "<span class='wedge'>dim.</span>";
                    }
                    // 旋律还没进来、伴奏已经在走的前奏小节：和弦照标，
                    // 否则开头的 4 小节就是四块空白，看不出和声进行。
                    // 休止时值按本小节的实际长度算——弱起小节比正常小节短
                    double restBeats = (double)grid.barSpan(b) / data.division;
                    body << "</span><span class='notes'>"
                         << withDyn(withChord(noteColumn("0", 0, 0, restBeats, ""),
                                              chordMarkFor(grid.barStart(b))),
                                    dynMarkFor(grid.barEnd(b) - 1))
                         << "</span></span>";
                    prevEnd = std::max(prevEnd, grid.barEnd(b));
                }

                // 每 measuresPerRow 个小节换一行
                if (bar > 0 && bar % measuresPerRow == 0) body << "</div><div class='row'>";

                curBar = bar;
                openBar(bar, n.startTicks, partChannel);
            } else if (inTriplet && !grp[0]->trip) {
                inTriplet = false;
            }

            // 三连音组的标记
            if (grp[0]->trip && !inTriplet) {
                flushBeam();
                flushSlur();          // 「3」是独立标记，不能混进连音线组
                curSlur = 0;
                body << "<span class='trip'>3</span>";
                inTriplet = true;
            }

            // 间隙补休止符。原来这里多了一个 `prevEnd > 0` 条件，
            // 导致乐曲开头的休止符（第一个音不在 tick 0）永远不显示。
            long gap = n.startTicks - prevEnd;
            if (gap >= restGateTicks(data.division)) {
                flushBeam();          // 休止符打断减时线
                double restBeats = (double)gap / data.division;
                emit(restColumns(gap, data.division, restBeats,
                                 chordMarkFor(prevEnd), dynMarkFor(prevEnd)),
                     prevEnd, true);
            }

            // 整组的时值取最长那个音
            long durTicks = 0;
            for (const auto* h : grp) durTicks = std::max(durTicks, h->n.durationTicks);
            double beats = (double)durTicks / data.division;

            if (cont) {
                // 延续段：一拍一个 `-`
                flushBeam();          // 长音打断减时线
                int cnt = (int)std::llround(beats);
                if (cnt < 1) cnt = 1;
                for (int k = 0; k < cnt; k++) {
                    long tk = n.startTicks + (long)k * data.division;
                    emit(withDyn(withChord(noteColumn("-", 0, 0, 1.0, ""), chordMarkFor(tk)),
                                 dynMarkFor(tk)), tk);
                }
            } else if (grp.size() == 1) {
                std::string jp = pitchToJianpu(n.pitch, keys.at(n.startTicks));
                int highDots = 0, lowDots = 0;
                std::string num;
                for (char c : jp) {
                    if (c == '\'') highDots++;
                    else if (c == ',') lowDots++;
                    else num += c;
                }
                // 倚音（装饰音）：先快速滑/弹一个音再落到主音，谱面上用小字上标
                // 写在主音数字前面，如 <sup>3</sup>5，不参与节奏计数。
                const NoteExpression ne = expr.noteAt(n.startTicks, n.channel);
                std::string graceHtml;
                if (ne.hasGrace())
                    graceHtml = "<span class='grace'>" +
                                htmlEscape(pitchToJianpu(ne.gracePitch, keys.at(n.startTicks))) +
                                "</span>";
                // 不要再排除第一个音：休止符是单独插入的列，本来就不参与歌词分配，
                // 多一个 !firstNote 只会把一句词的第一个字吞掉（"风霜染尽客行袍"
                // 变成"风霜染尽客行"+下一小节的"袍"）。
                std::string syl = (isMain && !lyrics.empty() && n.pitch != 0)
                                  ? lyricForNote(n.startTicks) : std::string();
                // 三连音走单独的时值档位，避免 1/3 拍被当成附点十六分
                DurStyle ds = grp[0]->trip ? tripletStyleOf(beats) : durStyleOf(beats);
                std::string col = graceHtml + withArticulation(
                    withDyn(withChord(noteColumn(num, highDots, lowDots, beats, syl, "", &ds),
                                      chordMarkFor(n.startTicks)),
                            dynMarkFor(n.startTicks)),
                    ne.symbol());
                // 试听播放器按这一列自己的起点 tick 找它（高亮用），
                // 延续段（"-"）与休止不需要，它们不是音头。
                if (player) col = withTick(col, n.startTicks);
                const int g = hasSlur ? lyrics.groupAt(n.startTicks) : 0;

                // 能否接上上一条减时线：同小节、首尾相接、减时线条数相同，
                // 且同属一个梁组（三连音组例外，它本来就跨组）。
                // 还要同属一个一字多音组：一条减时线跨两个字会把连音线切断。
                // 梁组边长由 TimeSignature::beamUnitTicks() 给：单拍子是一拍，
                // 复拍子（6/8 / 9/8 / 12/8）是附点拍。按「分母分之一」算的话
                // 6/8 的每个八分都自成一拍，六个音全部落在不同的拍上，
                // 一条减时线都画不出来。
                long beatLen = grid.sigForBar(bar).beamUnitTicks(data.division);
                long curBeat = beatLen > 0
                    ? (n.startTicks - grid.barStart(bar)) / beatLen : 0;
                long endTick = n.startTicks + std::max<long>(n.durationTicks, 1);
                bool join = ds.beams > 0 && ds.beams == beamLevel && !beamBuf.empty() &&
                            bar == beamBar && n.startTicks == beamPrevEnd &&
                            grp[0]->trip == beamTrip && g == beamGroup &&
                            (beamTrip || curBeat == beamBeat);

                if (ds.beams > 0) {
                    if (!join) {
                        flushBeam();
                        beamLevel = ds.beams;
                        beamGroup = g;
                        beamBar = bar;
                        beamBeat = curBeat;
                        beamTrip = grp[0]->trip;
                    }
                    beamBuf.push_back(col);
                    beamPrevEnd = endTick;
                } else {
                    flushBeam();
                    emitWithGroup(col, g);
                }
            } else {
                flushBeam();          // 和弦自成一列，不与单音连线
                // 多声部：竖向叠写。按音高降序，高音在上、低音在下，
                // 与五线谱的视觉方向一致（反过来看会很不习惯）。
                std::vector<NoteEvent> sorted;
                for (const auto* h : grp) sorted.push_back(h->n);
                std::sort(sorted.begin(), sorted.end(),
                          [](const NoteEvent& a, const NoteEvent& b) { return a.pitch > b.pitch; });

                // 倚音（装饰音）：和弦前面的快速装饰音同样用小字上标呈现。
                const NoteExpression ne = expr.noteAt(n.startTicks, n.channel);
                std::string graceHtml;
                if (ne.hasGrace())
                    graceHtml = "<span class='grace'>" +
                                htmlEscape(pitchToJianpu(ne.gracePitch, keys.at(n.startTicks))) +
                                "</span>";

                std::vector<std::pair<std::string, std::pair<int, int>>> tones;
                for (const auto& t : sorted) {
                    std::string jp = pitchToJianpu(t.pitch, keys.at(t.startTicks));
                    int hi = 0, lo = 0;
                    std::string num;
                    for (char c : jp) {
                        if (c == '\'') hi++;
                        else if (c == ',') lo++;
                        else num += c;
                    }
                    tones.push_back({num, {hi, lo}});
                }
                std::string ccol = graceHtml + withArticulation(
                         withDyn(withChord(chordColumn(tones, beats, ""),
                                           chordMarkFor(n.startTicks)),
                                 dynMarkFor(n.startTicks)),
                         ne.symbol());
                if (player) ccol = withTick(ccol, n.startTicks);
                emit(ccol, n.startTicks);
            }

            for (const auto* h : grp) prevEnd = std::max(prevEnd, h->n.startTicks + h->n.durationTicks);
            firstNote = false;
        }

        flushBeam();   // 收尾：把最后一条减时线画完再闭合小节
        flushSlur();   // 连音线同理，否则最后一组会整段消失
        body << "</span></span><span class='final-bar'></span></div></div></div>";
    }

    std::ostringstream html;
    html << "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
         << "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         << "<title>" << htmlEscape(displayTitle) << "</title>"
         << R"(<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='0.9em' font-size='90'>1</text></svg>"><style>
*{box-sizing:border-box}
body{font-family:"Noto Serif SC","Source Han Serif SC","SimSun",serif;max-width:960px;margin:36px auto;background:#fff;color:#222;padding:0 20px}
h1{font-size:26px;text-align:center;color:#b03030;margin:0 0 6px;font-weight:700}
.meta{color:#888;margin-bottom:26px;font-size:13px;text-align:center}

.track{padding:16px 0;margin-bottom:6px;border-bottom:1px dashed #eee}
.track:last-of-type{border-bottom:none}
.track h3{margin:0 0 10px;font-size:14px;color:#666;text-align:center;font-weight:600}
.track h3 small{color:#aaa;font-weight:normal;font-size:12px}

/* 谱面：每行固定若干小节，行内横排。原来用 flex 自动换行，行宽参差不齐。 */
.staff{display:block;font-size:26px;padding:4px 0}
.row{display:flex;align-items:flex-start;padding-top:10px}

/* 一个小节 = 上层标记行 + 下层音符行。
   早先小节号/和弦/力度和音符挤在同一条 flex 基线上，"Cmaj7 ff 1̇" 看起来
   像一个词的三个字母，和弦与力度根本分不开。分层后标记行高度固定 17px，
   各小节的音符行也随之顶部对齐。 */
.measure{display:flex;flex-direction:column;border-left:1.5px solid #bbb;
         padding:0 0 0 9px;margin:0 16px 0 0}
.marks{display:flex;align-items:flex-end;gap:8px;height:20px;padding-bottom:3px;
       line-height:1;white-space:nowrap}
.barno{font-size:10px;color:#c9c9c9;line-height:1;align-self:flex-start}
.dyn{font-size:14px;font-style:italic;color:#999;line-height:1}
/* 段落名（MIDI 0x06 marker）与渐强渐弱：和力度同在标记行，用颜色区分 */
.section{font-size:11px;font-weight:700;color:#2f7a5a;line-height:1;
         border:1px solid #cbe3d6;border-radius:3px;padding:1px 4px;background:#f3faf6}
.wedge{font-size:12px;font-style:italic;color:#a36a2a;line-height:1}
/* 断音/重音/滑音浮在音符列右上角：不占宽度，也不压住上八度点 */
.artic{position:absolute;right:-8px;top:-3px;font-size:14px;line-height:1;
       font-weight:700;color:#a33;white-space:nowrap;pointer-events:none}
.tempo{font-size:11px;font-weight:600;color:#4a6fa5;line-height:1;
       border:1px solid #cdd8e8;border-radius:3px;padding:1px 4px;background:#f4f7fc}
/* 渐快 / 渐慢：与阶跃变速同为速度记号，但用棕色区分——它不是一条
   「从这个速度开始」的指令，而是一整段的走向，读者扫谱时该分得开 */
.tempo.ramp{color:#8a5a2b;border-color:#e4d3bb;background:#fdf8f0}
.meter{font-size:11px;font-weight:700;color:#8a5a2b;line-height:1;
       border:1px solid #e4d3bb;border-radius:3px;padding:1px 4px;background:#fdf8f0}
/* 倚音（装饰音）：小字上标，写在主音数字前面，不参与节奏计数 */
.grace{font-size:11px;color:#a36a2a;vertical-align:super;margin-right:1px}
/* 转调标记与拍号/速度同为「谱面事件」，但用紫红色区分，扫一眼就知道
   这里不是节奏变了、而是整段唱名换了。 */
.keychg{font-size:11px;font-weight:700;color:#7a3f7d;line-height:1;
        border:1px solid #e0cbe1;border-radius:3px;padding:1px 4px;background:#fbf5fb}
/* 复调轨按声部拆行时的小标签：同一条轨会连着出现好几行，靠它区分 */
.voiceTag{font-size:12px;font-weight:400;color:#6b7a8d;margin-left:2px}

/* 和弦标记：浮在它所管的那个音符上方，不占宽度。
   位置必须跟音符走——标记行是从左往右排的，一旦前面多了一个速度标记，
   和弦就被顶右移，和第 3 拍的和弦叠在一起（`C` 与 `Am` 会糊成一团）。
   .notes 留 14px 上边距当和弦带，浮标底边贴住音符列上沿：
   上面是小节号/力度/速度那一行，下面是音符，三者互不侵入。
   也不能改成普通同行元素：那会把这一拍的音符间距撑开，谱面就参差不齐。 */
.chordx{position:absolute;bottom:calc(100% + 4px);left:50%;transform:translateX(-50%);
        font-size:13px;font-weight:600;font-style:normal;color:#b03030;
        line-height:1;white-space:nowrap;pointer-events:none}
.trip{font-size:12px;color:#666;align-self:flex-start;margin-right:2px;line-height:1.6}
.notes{display:flex;align-items:flex-start;padding-top:18px}

/* 一字多音的连音线：每个字拖着唱的那几个音共一条弧线。
   谱面上方从下到上分三层：数字 / 连音线 / 和弦字母。存在连音线时把
   .notes 的上留白从 18px 抬到 36px，和弦浮标相应抬高，弧线落在中间
   那 12px 里；三者各占一层，弧线不会压到高八度点，也不会和和弦粘连。
   只给有连音线的那一轨加 hasslur，其余谱面行距保持原样。 */
.hasslur .notes{padding-top:36px}
.hasslur .chordx{bottom:calc(100% + 22px)}
.sgrp{position:relative;display:inline-flex;align-items:flex-start}
.sgrp::before{content:"";position:absolute;left:3px;right:3px;top:-13px;height:9px;
              border-top:1.5px solid #555;border-radius:50% 50% 0 0 / 100% 100% 0 0;
              pointer-events:none}

/* ---- 音符列 ----
   .note 的宽度只由数字决定：延音线用绝对定位挂在 .core 右侧，并通过 --n
   给右侧预留 padding。这样 .oct（八度点）和 .lyric（歌词）都会精确对齐数字。 */
.note{display:inline-flex;flex-direction:column;align-items:center;position:relative;
      margin:0 6px;vertical-align:top;padding-right:calc(var(--n,0) * 1.15em)}
.oct{display:flex;flex-direction:column;align-items:center;gap:2px;height:12px;flex:none}
.oct.low{margin-top:2px}
.oct i{display:block;width:5px;height:5px;border-radius:50%;background:#222}
.core{position:relative;display:inline-block;line-height:1.15}
.num{font-weight:600;font-size:26px;line-height:1.15;display:inline-block}
.exts{position:absolute;left:100%;top:50%;transform:translateY(-42%);
      display:flex;white-space:nowrap}
.ext{font-size:26px;line-height:1;margin-left:7px;color:#222}
.lyric{font-size:14px;color:#666;margin-top:3px;min-height:18px;line-height:1.2;white-space:nowrap}

/* 和弦：同一时刻的多个音竖向叠写，共用一个时值线。
   行高比单音列高，所以整行会被撑高——这是简谱表示和弦的固有代价。 */
.chordcol .core{display:inline-block}
/* 和弦的延音线对齐整个和弦的底部（最低音那一行），而不是垂直居中：
   居中的话看起来像只有中间那个音在延长。 */
.chordcol .exts{top:auto;bottom:2px;transform:none}
.stack{display:flex;flex-direction:column;align-items:center}
.tone{display:flex;flex-direction:column;align-items:center;margin:-7px 0}
.tone .oct{height:8px;gap:1px}
.tone .oct i{width:4px;height:4px}
.tone .num{font-size:24px;line-height:1.05}

/* 减时线：八分 1 条、十六分 2 条、三十二分 3 条 */
.eighth .num,.sixteenth .num,.thirtysecond .num{border-bottom:2px solid #222;padding-bottom:1px}
.sixteenth .num{box-shadow:0 4px 0 #222}
.thirtysecond .num{box-shadow:0 4px 0 #222,0 8px 0 #222}

/* 连成一条的减时线：一拍内的同组音符共用一条横线，而不是各自一小段。
   .bgrp 里取消逐音符的短线，改用一条跨全组的绝对定位线；十六分/三十二分
   再向下每 4px 叠一条平行线。line 的纵向位置 = 八度点区(12px) + 数字行高，
   与 .num 的 border-bottom 原本所在的位置对齐。 */
.bgrp{position:relative;display:inline-flex;align-items:flex-start}
.bgrp>.note{margin:0 4px}
.bgrp .num{border-bottom:none;box-shadow:none}
.bgrp::after{content:"";position:absolute;left:1px;right:1px;
             top:calc(12px + 1.15em - 1px);height:2px;background:#222}
.bgrp.b2::after{box-shadow:0 4px 0 #222}
.bgrp.b3::after{box-shadow:0 4px 0 #222,0 8px 0 #222}

/* 和弦的减时线只画在最下面那个音上：叠写的每个音各画一条，看起来像十六分音符 */
.chordcol .tone .num{border-bottom:none;box-shadow:none}
.chordcol.eighth .tone:last-child .num,
.chordcol.sixteenth .tone:last-child .num,
.chordcol.thirtysecond .tone:last-child .num{border-bottom:2px solid #222;padding-bottom:1px}
.chordcol.sixteenth .tone:last-child .num{box-shadow:0 4px 0 #222}
.chordcol.thirtysecond .tone:last-child .num{box-shadow:0 4px 0 #222,0 8px 0 #222}
.dotted .num::after{content:"";display:inline-block;width:5px;height:5px;border-radius:50%;
                    background:#222;margin-left:4px;vertical-align:middle}

.final-bar{display:inline-block;width:2px;height:34px;background:#333;margin-left:10px;
           align-self:flex-start;margin-top:26px;position:relative}
.final-bar::after{content:"";position:absolute;left:5px;top:0;width:4px;height:34px;background:#333}
.footer{color:#bbb;font-size:12px;text-align:center;margin:24px 0 10px}

@media print{
  body{max-width:none;margin:0;padding:0}
  .track{page-break-inside:auto;border-bottom:none}
  .measure{page-break-inside:avoid}
  .barno{color:#ddd}
}
@media (max-width:600px){
  .staff{font-size:20px}
  .num,.ext{font-size:20px}
  .lyric{font-size:12px}
}
</style>)";
    // 小节中途的力度浮标只在真的出现过时才带样式：没有这种记号的谱子
    // 输出必须逐字节不变（既有素材的零回归靠这条守住，不能无条件塞 CSS）。
    if (body.str().find("dynx") != std::string::npos)
        html << "<style>"
             << ".dynx{position:absolute;bottom:calc(100% + 4px);left:50%;"
                "transform:translateX(-50%);font-size:12px;font-weight:600;"
                "font-style:italic;color:#2f6f4f;line-height:1;"
                "white-space:nowrap;pointer-events:none}"
             << ".dynx.pair{left:0;transform:translateX(-115%)}"
             << ".hasslur .dynx{bottom:calc(100% + 22px)}"
             << "</style>";
    // 试听播放器的样式也只在开关打开时才写：不带 --player 的页面（以及所有
    // 既有素材）必须逐字节不变，这条是零回归的底线。
    if (player) {
        html << "<style>"
             << "#playerbar{position:sticky;top:0;z-index:9;display:flex;gap:14px;"
                "align-items:center;justify-content:center;flex-wrap:wrap;"
                "padding:8px 12px;margin:0 0 14px;background:#fff;"
                "border-bottom:1px solid #eee;font-size:13px;color:#555}"
             << "#playerbar button{font:inherit;font-weight:600;color:#b03030;"
                "background:#fff;border:1px solid #e3c9c9;border-radius:16px;"
                "padding:5px 16px;cursor:pointer}"
             << "#playerbar button:hover{background:#fdf3f3}"
             << "#playerbar .spd select{font:inherit;padding:2px 4px;margin-left:4px}"
             << "#playerbar .hint{color:#bbb;font-size:12px}"
             << "#pstat{color:#aaa}"
             << ".note[data-t]{cursor:pointer}"
             << ".note.playing{background:rgba(176,48,48,.09);border-radius:5px}"
             << ".note.playing .num{color:#b03030}"
             << "@media print{#playerbar{display:none}}"
             << "</style>";
    }
    html << R"(</head><body>
)";
    // 播放器必须紧跟在 <body> 之后：position:sticky 只有在元素位于文档流前部
    // 时才「粘」得住，放到页尾就变成一个只能滚到底才看得见的按钮。
    if (player) html << playerBlock(playJson);
    html << R"(<h1>)" << htmlEscape(displayTitle) << R"(</h1>
<div class="meta">)"
         << htmlEscape(keys.summary(grid))
         << " ｜ " << meterSummary(data)
         << " ｜ " << (data.keySig.valid ? "MIDI调号"
                       : (key.scaleLabel.empty() ? "自动猜测" : "五声宫音校正"))
         << (tempoSummary(data).empty() ? "" : " ｜ " + tempoSummary(data))
         << (partSummary(data).empty() ? "" : " ｜ " + partSummary(data))
         << (pickupLabel(data).empty() ? "" : " ｜ " + pickupLabel(data))
         << (truncationLabel(data).empty() ? "" : " ｜ " + truncationLabel(data))
         << (timeBaseLabel(data).empty() ? "" : " ｜ " + timeBaseLabel(data))
         << (formatLabel(data).empty() ? "" : " ｜ " + formatLabel(data))
         << (controllerLabel(data).empty() ? "" : " ｜ " + controllerLabel(data))
         << (bendRangeLabel(data).empty() ? "" : " ｜ " + bendRangeLabel(data))
         << (runningStatusLabel(data).empty() ? "" : " ｜ " + runningStatusLabel(data))
         << (portLabel(data).empty() ? "" : " ｜ " + portLabel(data))
         << (sysExLabel(data).empty() ? "" : " ｜ " + sysExLabel(data))
         << (bankLabel(data).empty() ? "" : " ｜ " + bankLabel(data))
         << (trackLabel(data).empty() ? "" : " ｜ " + trackLabel(data))
         << (data.drumNotesSkipped > 0 ? " ｜ 已跳过鼓轨" : "")
         << (lyricLabel.empty() ? "" : " ｜ " + lyricLabel)
         << (expr.summary().empty() ? "" : " ｜ " + expr.summary())
         << "</div>" << body.str()
         << "<footer class='footer'>由 midi2jianpu v" M2J_VERSION " 生成</footer>"
         << "</body></html>";
    return html.str();
}

} // namespace m2j

// musicxml_writer.cpp - MIDI 数据 -> MusicXML 3.1
//
// 输出目标是「MuseScore 打开就能用」，因此比纯文本输出严格得多：
//   - duration 必须是整数，且 divisions 取自源文件，不做二次换算
//   - 同时发声的音符要标 <chord/>，否则会被当成旋律音顺次排开
//   - 跨小节的音符必须切开，段间用 <tie> 连接，否则小节时长会溢出
//   - 三连音的 <type> 要写名义时值（乘 3/2），不能写实际时长
//   - 每个小节的总时长必须等于拍号规定值，不足要补休止符
//   - 休止符间隙拆成标准时值之和，<duration> 与 <type>/<dot/> 必须严格一致
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/notation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <vector>

namespace m2j {

// 歌词 / 轨名 / 乐器名是用户贴进来的任意文本，'&' 或 '<' 会直接把 XML 打坏；
// 此外 MIDI 文本事件里偶尔夹带 0x01–0x1F 的噪声控制字节（例如 0x03），
// XML 1.0 连字符引用都禁止它们出现在文本里，必须丢弃，否则整份 MusicXML
// 非良构、MuseScore 直接打不开。HTML 写入器容忍这些字节，但严格的 XML
// 解析器不容忍，所以这里是 MusicXML 专属的清理。
static std::string xmlEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&apos;";
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            ;  // XML 1.0 禁止 C0 控制字符，丢弃
        else out += (char)c;
    }
    return out;
}

// 升号写法（调号不含降号时使用）
static const char* SHARP_STEP[12] = {
    "C", "C", "D", "D", "E", "F",
    "F", "G", "G", "A", "A", "B"
};
static const int SHARP_ALTER[12] = {
    0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0
};

// 降号写法（调号含降号时使用）
static const char* FLAT_STEP[12] = {
    "C", "D", "D", "E", "E", "F",
    "G", "G", "A", "A", "B", "B"
};
static const int FLAT_ALTER[12] = {
    0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0
};

// 和弦符号 <harmony> 的 root-step / bass-step：把音级写成「音名 + 升降」。
// MusicXML 用 <root-step>C</root-step><root-alter>1</root-alter> 这种两段式，
// 而不是一个 "C#" 字符串；升降为 0 时 <*-alter> 可以省掉。
static void emitHarmonyStep(std::ostringstream& xml, const char* tag, int pc, bool flat) {
    const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
    const int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];
    xml << "<" << tag << "-step>" << step << "</" << tag << "-step>";
    if (alter != 0) xml << "<" << tag << "-alter>" << alter << "</" << tag << "-alter>";
}

// 和弦符号。此前和弦只活在文本谱和 HTML 里，导进 MuseScore 就全没了；
// 现在写 <harmony>，MuseScore / Finale 会在谱表上方显示和弦名。
// kind 用 MusicXML 标准值（语义），text 属性放我们的显示后缀——m7b5、7sus4
// 没有对应的标准 kind，靠 text 才能原样显示。转位写 <bass>。
static void emitHarmonyXml(std::ostringstream& xml, const ChordMark& cm, bool flat) {
    if (cm.rootPc < 0) return;
    xml << "      <harmony><root>";
    emitHarmonyStep(xml, "root", cm.rootPc, flat);
    xml << "</root><kind";
    if (!cm.kindText.empty()) xml << " text=\"" << xmlEscape(cm.kindText) << "\"";
    xml << ">" << (cm.kind.empty() ? "major" : cm.kind) << "</kind>";
    if (cm.bassPc >= 0) {
        xml << "<bass>";
        emitHarmonyStep(xml, "bass", cm.bassPc, flat);
        xml << "</bass>";
    }
    xml << "</harmony>\n";
}

// 拍数 -> MusicXML <type> + 是否附点
//
// 传入的必须是**名义时值**：三连音八分音符实际占 1/3 拍，但它的 <type>
// 应写 eighth（名义 0.5 拍）并配 <time-modification>。若直接把 0.333 传进来，
// 会落到 16th 上，MuseScore 里三连音就画错了。
static std::pair<std::string, bool> durTypeDot(double beats) {
    static const struct { double base; const char* type; } stds[] = {
        {4.0, "whole"}, {2.0, "half"}, {1.0, "quarter"},
        {0.5, "eighth"}, {0.25, "16th"}, {0.125, "32nd"}, {0.0625, "64th"}
    };
    // 先精确匹配（含附点），容差按比例收紧到 8%，
    // 原来 15% 会把 5 拍误判成附点全音符。
    for (auto& s : stds) {
        if (std::abs(beats - s.base) / s.base < 0.08) return {s.type, false};
        double dotted = s.base * 1.5;
        if (std::abs(beats - dotted) / dotted < 0.08) return {s.type, true};
    }
    // 兜底：取最接近的基础时值
    double best = 1e9; std::string bestType = "quarter";
    for (auto& s : stds) {
        double d = std::abs(beats - s.base);
        if (d < best) { best = d; bestType = s.type; }
    }
    return {bestType, false};
}

// 休止符时值拆分：音符走 quantizeTiming() 吸附到标准档，休止符却是按两个
// 音头之间的原始 tick 间隙直接写出来的。durTypeDot() 的 8% 容差与「取最
// 接近基础时值」的兜底会把 1.75 拍写成二分休止、3.75 拍写成全休止 ——
// <duration> 精确但 <type> 只是近似值。MuseScore 按 type 画形状、按
// duration 推进游标，谱面就会把休止符画错。
// 这里把间隙贪心拆成若干标准时值（含附点）之和：每个 <note><rest/> 的
// duration 与 type/dot 严格一致，各段相加仍等于原间隙（拍数守恒）。
struct RestPart {
    long ticks = 0;
    const char* type = "quarter";
    bool dotted = false;
};

// units 以 64 分音符为单位：1 拍 = 16，whole = 64，附点全音符 = 96。
// 按 tick 长度从大到小排列，贪心才不会把 3 拍拆成「半 + 四分之一」。
static const struct { int units; const char* type; bool dotted; } kRestSteps[] = {
    {96, "whole", true},   {64, "whole", false},
    {48, "half", true},    {32, "half", false},
    {24, "quarter", true}, {16, "quarter", false},
    {12, "eighth", true},  {8, "eighth", false},
    {6, "16th", true},     {4, "16th", false},
    {3, "32nd", true},     {2, "32nd", false},
    {1, "64th", false},
};

// 返回空向量表示「这段间隙无法由标准时值精确拼出」，调用方保留旧兜底。
static std::vector<RestPart> splitRestDuration(long ticks, int division) {
    std::vector<RestPart> parts;
    if (ticks <= 0 || division <= 0) return parts;
    long long rem = ticks;
    while (rem > 0) {
        bool picked = false;
        for (const auto& step : kRestSteps) {
            const long long scaled = 1LL * division * step.units;
            if (scaled % 16 != 0) continue;   // 非整 tick 的档位跳过
            const long long len = scaled / 16;
            if (len > 0 && len <= rem) {
                parts.push_back({(long)len, step.type, step.dotted});
                rem -= len;
                picked = true;
                break;
            }
        }
        // 余下不足一个 64 分音符（或 division 让某档除不尽）：不擅自改时长，
        // 交回旧的单一休止符兜底。
        if (!picked) { parts.clear(); return parts; }
    }
    return parts;
}

// 一个待输出的音符片段。
// 跨小节的音符会被切成多段，段与段之间用 tie 连接——这是 MuseScore 正确
// 渲染延音的前提，也是小节时长不溢出的前提。
struct Segment {
    long start = 0;        // 绝对 tick
    long dur = 0;          // 本段持续 tick
    int bar = 0;           // 所属小节（从 0 计）
    int pitch = 0;
    int channel = 0;       // 演奏法按 (tick, channel) 查，不能只按 tick
    int voice = 0;         // 同轨复调的声部号（0 = 最高声部），写出时 +1 成 <voice>
    bool triplet = false;  // 属于三连音组，需要 <time-modification>
    bool tieStart = false; // 后面还有延续段 -> <tie type="start"/>
    bool tieEnd = false;   // 前面有被切开的段 -> <tie type="stop"/>
    bool grace = false;    // 倚音：<grace/> 不占时长，排在主音之前
    int staff = 1;         // 大谱表：1=高音谱表(上行谱), 2=低音谱表(下行谱)
};

std::string renderMusicXml(const MidiData& data, const KeyEstimate& key,
                           const std::vector<LyricLine>* lyricLines) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<score-partwise version=\"3.1\">\n";

    const int division = (data.division > 0) ? data.division : 480;

    // 弯音修正（只统计同通道，避免不同乐器互相干扰）
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

    // 按轨分组
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks, n.channel);
        if (std::abs(off) >= 0.5) n.pitch += (int)std::round(off);
        byTrack[n.track].push_back(n);
    }

    // 大谱表判定：键盘族乐器（钢琴/管风琴等）且音域跨度充分（≥40 半音，约
    // 三个八度以上）并同时跨中音 C 两侧时，应当排成上下两行谱（高音谱号 +
    // 低音谱号），而不是挤在一行高音谱号里满屏加线。MIDI 不携带「乐器是否用
    // 大谱表」的语义，只能靠乐器族 + 音域启发式判断；阈值刻意定得比仓库里所有
    // 测试素材都宽（最宽的键盘声部是 polyphony 的 38 半音），保证 76 份既有
    // 素材的 grand 恒为 false、输出字节零回归，只有真正宽的钢琴曲才会触发。
    auto isKeyboardProgram = [](int prg) {
        return (prg >= 0 && prg <= 7) || (prg >= 16 && prg <= 23);
    };
    auto isGrandStaffPart = [&](const std::vector<NoteEvent>& notes,
                               int track, long firstTick) -> bool {
        if (notes.empty()) return false;
        int mn = 200, mx = -200;
        for (const auto& n : notes) {
            if (n.pitch < mn) mn = n.pitch;
            if (n.pitch > mx) mx = n.pitch;
        }
        bool spans = (mn < 60) && (mx >= 60);          // 跨中音 C（C4=60）
        bool wide = (mx - mn) >= 40;                    // 跨度 ≥ 三个八度以上
        int dom = dominantChannel(notes);
        int prg = (dom >= 0) ? programAt(data, dom, firstTick) : -1;
        bool kb = isKeyboardProgram(prg);
        // 轨道名作为键盘乐器的补充线索（部分文件没写 0xC0 音色）
        auto it = data.trackNames.find(track);
        if (it != data.trackNames.end() && !it->second.empty()) {
            std::string low = it->second;
            for (char& c : low) c = (char)std::tolower((unsigned char)c);
            if (low.find("piano") != std::string::npos ||
                low.find("钢琴") != std::string::npos ||
                low.find("organ") != std::string::npos ||
                low.find("harpsi") != std::string::npos ||
                low.find("clav") != std::string::npos ||
                low.find("celesta") != std::string::npos ||
                low.find("key") != std::string::npos)
                kb = true;
        }
        return kb && spans && wide;
    };

    // part-list
    xml << "  <part-list>\n";
    int partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        std::string name;
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) name = it->second;
        else name = "Part " + std::to_string(t + 1);
        // 声部乐器：取该声部第一个音符的通道与时刻，按时间轴查当时的
        // 0xC0 音色号；中途换乐器的曲子不会整首都标成最后一件乐器。
        int pch = ns.empty() ? -1 : ns.front().channel;
        long firstTick = 0;
        if (!ns.empty()) {
            firstTick = ns.front().startTicks;
            for (const auto& nn : ns)
                if (nn.startTicks < firstTick) firstTick = nn.startTicks;
        }
        std::string inst;
        const int prg = pch < 0 ? -1 : programAt(data, pch, firstTick);
        if (prg >= 0) inst = gmInstrumentName(prg);
        xml << "    <score-part id=\"P" << partIdx << "\">"
            << "<part-name>" << xmlEscape(name) << "</part-name>";
        if (!inst.empty()) {
            xml << "<score-instrument id=\"P" << partIdx << "-I" << partIdx << "\">"
                << "<instrument-name>" << xmlEscape(inst) << "</instrument-name>"
                << "</score-instrument>";
        }
        xml << "</score-part>\n";
        partIdx++;
    }
    xml << "  </part-list>\n";

    // 小节长度（tick）。拍号缺失或非法时回退到 4/4，
    // 否则后面所有除法都会炸。弱起曲目的整套换算走 BarGrid。
    const BarGrid grid = barGridOf(data);
    // 调号按小节查表：中途转调时 MuseScore 必须看到新的 <fifths>/<mode>，
    // 音符的升/降等音写法也跟着当前调走。
    const KeyMap keys(data, key);
    // 演奏表情与文本谱、HTML 共用同一份判定。MusicXML 此前完全没写力度，
    // 导进 MuseScore 后「网页上有 mf、谱上什么都没有」。
    const ExpressionMap expr(data);

    // 速度：MIDI 里 tempo 是「每四分音符的微秒数」，取首个非零值
    double bpm = 120.0;
    if (data.tempo > 0) bpm = 60000000.0 / data.tempo;
    else if (!data.tempoChanges.empty() && data.tempoChanges[0].bpm > 0) bpm = data.tempoChanges[0].bpm;

    // 歌词只挂旋律轨。来源可能是 --lyrics 给的 LRC，也可能是 MIDI 自带的
    // 0x05 歌词；两者都走同一套 pickMelodyTrack（内置歌词还会给出人声轨证据）。
    const bool hasLyrics = lyricSourceOf(data, lyricLines) != LyricSource::None;
    const int mainTrack = hasLyrics ? pickMelodyTrack(data, lyricLines) : -1;

    // 和弦符号：和文本谱 / HTML 共用同一份 detectChords 结果。
    // 挂到旋律声部上方（标准 lead sheet 排法）；没有明确旋律声部时退化到
    // 第一个声部。按小节分桶，便于随音符位置插入 <harmony>。
    const std::vector<ChordMark> chordMarks = detectChords(data, key);
    const int chordTrack = pickMelodyTrack(data, lyricLines);
    std::map<int, std::vector<const ChordMark*>> chordsByBar;
    for (const auto& cm : chordMarks)
        chordsByBar[(int)grid.barIndex(cm.startTick)].push_back(&cm);

    // 速度：折叠成谱面记号（阶跃 / 渐快 / 渐慢），与文本谱、HTML 共用一份。
    //
    // 旧实现只在「某个 set_tempo 的 tick 正好等于小节线」时才写 <metronome>。
    // 渐慢在 MIDI 里是几十个自动化节点，几乎不可能落在小节线上，于是整段
    // rit. 在导出的谱里彻底消失，谱面只剩曲首那个速度。现在：
    //   阶跃 -> <metronome>♩=N
    //   渐变 -> <words>rit.</words>（一个文字记号），并在它落定的那一小节
    //           补一个 <metronome> 写终点速度 —— 这既是标准记法，
    //           也让 MuseScore 的播放跳到正确的速度，而不是一路 120 到曲终。
    // 速度记号按小节分桶。同一小节里的多个阶跃必须全部保留：旧实现用
    // map<bar, mark*> 只留最后一个，一小节内 120→90→60 会被吞成 60，
    // 播放时整小节都按 60 走。位置信息也不许丢：变速点很少正好落在小节线
    // 上（DAW 导出的 tempo map 常在第 3 拍），落点交给 <offset> 表达。
    const std::vector<TempoMark> tempoMarks = tempoMarksOf(data);
    std::map<int, std::vector<const TempoMark*>> stepAtBar;  // 阶跃记号（按 tick 升序）
    std::map<int, const TempoMark*> rampStartAtBar;  // 渐变起点
    std::map<int, const TempoMark*> rampEndAtBar;    // 渐变落定处
    for (size_t i = 0; i < tempoMarks.size(); i++) {
        const TempoMark& m = tempoMarks[i];
        const int b = (int)grid.barIndex(m.tick);
        if (m.kind == TempoMarkKind::Step) {
            // 曲首速度由 <attributes> 的 <metronome> 写过了，不要重复
            if (i == 0 && m.tick <= grid.barStart(0)) continue;
            stepAtBar[b].push_back(&m);
        } else {
            rampStartAtBar[b] = &m;
            const int eb = (int)grid.barIndex(m.endTick);
            if (rampEndAtBar.find(eb) == rampEndAtBar.end()) rampEndAtBar[eb] = &m;
        }
    }

    partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        if (ns.empty()) { partIdx++; continue; }

        // 大谱表：键盘族乐器且音域够宽并跨中音 C 时，本声部排成上下两行谱。
        // 音色按「本声部最早一个音」的 tick 取，避免中途换音色的文件被误判。
        long firstTick = ns.front().startTicks;
        for (const auto& n : ns)
            if (n.startTicks < firstTick) firstTick = n.startTicks;
        const bool grand = isGrandStaffPart(ns, t, firstTick);

        const LyricAssignment lyrics = (t == mainTrack)
                                     ? resolveLyrics(data, lyricLines, t)
                                     : LyricAssignment();

        // CC11 / 力度档位 / 渐变楔形都按通道归属，而且一个 part 里可能有
        // 多个 <voice>（同轨复调、或旋律 + 低音伴奏）。旧实现整份 part 只取
        // 一个主导通道，副声部的 crescendo 与 p→ff 会被整条吞掉；下面按
        // voice 分组各算一次主导通道，口径仍由 dominantChannel() 统一。

        // 和弦符号只写一份：旋律声部优先，没有旋律声部时写进第一个 part。
        const bool writeChords = (chordTrack >= 0) ? (t == chordTrack) : (partIdx == 1);
        std::vector<const ChordMark*> pendingChords;  // 当前小节待写的和弦（按 tick 升序）
        size_t pendingIdx = 0;
        // 把 tick <= uptoTick 的和弦写出来。MusicXML 的 <harmony> 与音符按文档
        // 顺序定位，所以必须插在「该 tick 之前已写到的位置」——写在小节开头会
        // 让后半小节换的和弦全挤到第一拍。
        auto flushChords = [&](long uptoTick) {
            if (!writeChords) return;
            while (pendingIdx < pendingChords.size() &&
                   pendingChords[pendingIdx]->startTick <= uptoTick) {
                const ChordMark& cm = *pendingChords[pendingIdx++];
                emitHarmonyXml(xml, cm, prefersFlats(keys.at(cm.startTick)));
            }
        };

        // ---- 1. 展开成片段：跨小节的音符在此切开 ----
        //
        // 先分声部：同轨两个声部（钢琴左右手写在同一个轨里）如果混在一起写，
        // 长音叠短音会被当成和弦塞进同一拍，<chord/> 的各个成员时值不同，
        // 小节总时值直接溢出（4/4 写成 8 拍），MuseScore 打不开。
        const std::vector<int> voiceOf = splitVoices(
            ns, 3, [&](const NoteEvent& n) { return expr.isGraceNote(n.startTicks, n.channel); });
        // 每个 <voice> 一套自己的通道 / 力度记忆：ns 按 splitVoices 的分组
        // 归位后，dominantChannel() 与文本谱 / HTML 的 PartView 口径一致。
        int voiceCount = 1;
        for (size_t i = 0; i < ns.size(); ++i) {
            const int v = (i < voiceOf.size()) ? voiceOf[i] : 0;
            if (v + 1 > voiceCount) voiceCount = v + 1;
        }
        std::vector<std::vector<NoteEvent>> voiceNotes(voiceCount);
        for (size_t i = 0; i < ns.size(); ++i) {
            const int v = (i < voiceOf.size()) ? voiceOf[i] : 0;
            voiceNotes[v].push_back(ns[i]);
        }
        std::vector<int> voiceChannel(voiceCount, -1);
        for (int v = 0; v < voiceCount; ++v)
            voiceChannel[v] = dominantChannel(voiceNotes[v]);
        // 每个 <voice> 归属哪个谱表：取该声部音符的多数音高侧（≥中音 C 在高音
        // 谱表 = 1，否则低音谱表 = 2）。大谱表下演奏记号要按此标 <staff>。
        std::vector<int> voiceStaff(voiceCount, 1);
        for (int v = 0; v < voiceCount; ++v) {
            int lo = 0, hi = 0;
            for (const auto& n : voiceNotes[v])
                (n.pitch < 60 ? lo : hi)++;
            voiceStaff[v] = (lo >= hi) ? 2 : 1;
        }
        std::vector<std::string> lastDyn(voiceCount);
        // 一条楔形（crescendo / diminuendo → stop）的起止必须落同一谱表：
        // 以「起点音头所在谱表」为锚，stop 复用该 <staff>。起点与终点常跨小节，
        // 所以锚记在 emitDirections 之外（按 voice 记），stop 消费后清零。
        std::vector<int> wedgeAnchorStaff(voiceCount, -1);
        auto triplets = detectTriplets(ns, division);
        std::vector<Segment> segs;
        segs.reserve(ns.size() * 2);

        for (size_t i = 0; i < ns.size(); i++) {
            const auto& n = ns[i];
            // 倚音（装饰音）：整颗跳过，改挂到紧随其后的主音上作为 <grace/>。
            // 否则它会自己占一格、还把主音往后推，整行节奏错位。
            if (expr.isGraceNote(n.startTicks, n.channel)) continue;

            long start = n.startTicks;
            long end = start + std::max<long>(n.durationTicks, 1);
            bool firstPart = true;

            while (start < end) {
                int bar = (int)grid.barIndex(start);
                long barEnd = grid.barEnd(bar);
                long segEnd = std::min(end, barEnd);
                // 防御：若换算出的小节末尾没有推进，直接吃到音符结束，避免死循环
                if (segEnd <= start) segEnd = end;

                Segment s;
                s.start = start;
                s.dur = segEnd - start;
                s.bar = bar;
                s.pitch = n.pitch;
                s.channel = n.channel;
                s.voice = (i < voiceOf.size()) ? voiceOf[i] : 0;
                // 大谱表：按音高分流到上下两行谱（中音 C 及以下进低音谱表）。
                s.staff = grand ? ((n.pitch < 60) ? 2 : 1) : 1;
                s.triplet = (i < triplets.size()) ? triplets[i] : false;
                s.tieEnd = !firstPart;      // 前面被切过
                s.tieStart = (segEnd < end); // 后面还有
                segs.push_back(s);

                start = segEnd;
                firstPart = false;
            }
        }

        // ---- 2. 按 (小节, 谱表, 声部, 起始 tick) 稳定排序 ----
        // 谱表当成第二排序键：大谱表下上行谱(1)的所有音必须先于下行谱(2)，
        // 才能在小节内用一次 <backup> 退回小节开头再写另一个谱表；声部是第三
        // 键，保证同一声部在一个谱表里连续输出。
        std::stable_sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) {
            if (a.bar != b.bar) return a.bar < b.bar;
            if (a.staff != b.staff) return a.staff < b.staff;
            if (a.voice != b.voice) return a.voice < b.voice;
            return a.start < b.start;
        });

        // ---- 3.5 连梁（beam）预计算 ----
        // MusicXML 用 <beam> 把 < 四分音符的连续音符连成梁；没有它时 MuseScore
        // 只能靠自动连梁，遇到跨拍的连音、混合时值、三连音就会连错。
        // 规则与 HTML 端减时线对齐：同一 (小节, 谱表, 声部) 内、时值 < 四分、
        // 首尾相接（中间无休止）、同三连音归属，且落在同一拍上（三连音例外，
        // 它本来就跨拍）的连续音符并成一组。单音不画梁。同拍和弦（多颗同
        // start 的音符）算一个事件。
        struct BeamInfo { int levels = 0; char state[5] = {0,0,0,0,0}; };
        std::vector<BeamInfo> beamOf(segs.size());
        // 4/5 是钩形梁：孤立短音符的次级梁画不成长梁，只能画一小段钩。
        // MusicXML 规范里 forward hook 从符干向右（组首，朝后一个音），
        // backward hook 向左（组尾，朝前一个音）。
        const char* kBeamWord[6] = {"", "begin", "continue", "end",
                                    "forward hook", "backward hook"};
        auto beamLevelOfType = [](const std::string& t) -> int {
            if (t == "eighth") return 1;
            if (t == "16th")   return 2;
            if (t == "32nd")   return 3;
            if (t == "64th")   return 4;
            return 0; // quarter/half/whole 不画梁
        };
        struct BeamEvent {
            long start = 0, end = 0, beat = 0;
            int bar = 0, staff = 0, voice = 0, trip = 0, level = 0;
            std::vector<int> segs;
        };
        std::vector<BeamEvent> bev;
        for (size_t i = 0; i < segs.size(); ) {
            size_t j = i;
            const long st = segs[i].start;
            while (j < segs.size() && segs[j].bar == segs[i].bar &&
                   segs[j].staff == segs[i].staff && segs[j].voice == segs[i].voice &&
                   segs[j].start == st) ++j;
            BeamEvent ev;
            ev.start = segs[i].start;
            ev.end   = segs[i].start + segs[i].dur;
            ev.bar   = segs[i].bar;  ev.staff = segs[i].staff;
            ev.voice = segs[i].voice;
            ev.trip  = segs[i].triplet ? 1 : 0;
            ev.level = beamLevelOfType(durTypeDot(
                segs[i].triplet ? (segs[i].dur / (double)division) * 1.5
                               : (segs[i].dur / (double)division)).first);
            // 梁组边长与 HTML 端同一口径，都走 TimeSignature::beamUnitTicks()：
            // 单拍子一拍、复拍子附点拍。既不把整小节连成一条跨强拍的长梁，
            // 也不会让 6/8 的每个八分各自成组（那样一条梁都画不出来）。
            const long beatLen = grid.sigForBar(ev.bar).beamUnitTicks(division);
            ev.beat = beatLen > 0 ? (ev.start - grid.barStart(ev.bar)) / beatLen : 0;
            for (size_t k = i; k < j; ++k) ev.segs.push_back((int)k);
            bev.push_back(ev);
            i = j;
        }
        auto flushBeamGroup = [&](std::vector<int>& grp) {
            if (grp.size() >= 2) {
                int maxL = 0;
                for (int ei : grp) maxL = std::max(maxL, bev[ei].level);
                for (int L = 1; L <= maxL; ++L) {
                    // 该层只连「连续的参与者」：中间夹着更短的音就断开，
                    // 否则次级梁会跨过它。断开后只剩一颗音时不能整条丢掉：
                    // 十六分音符少一层梁，在记谱软件里就显示成八分音符。
                    // 这种情况用钩形梁（hook）——组首向右画 forward hook，
                    // 其余位置向左画 backward hook，与 HTML 端按自身时值
                    // 画两条减时线表达的是同一件事。
                    size_t k = 0;
                    while (k < grp.size()) {
                        if (bev[grp[k]].level < L) { ++k; continue; }
                        size_t m = k;
                        while (m < grp.size() && bev[grp[m]].level >= L) ++m;
                        if (m - k >= 2) {
                            for (size_t u = k; u < m; ++u) {
                                const int w = (u == k) ? 1 : (u + 1 == m) ? 3 : 2;
                                for (int si : bev[grp[u]].segs)
                                    beamOf[si].state[L] = (char)w;
                            }
                        } else if (L >= 2) {
                            // 一级梁的参与者必然是整组（grp 里全是短音符），
                            // 所以孤立项只会出现在次级梁上。
                            const int w = (k == 0) ? 4 : 5;
                            for (int si : bev[grp[k]].segs)
                                beamOf[si].state[L] = (char)w;
                        }
                        k = m;
                    }
                }
            }
            grp.clear();
        };
        std::vector<int> beamGrp;
        int pb = -1, ps = -1, pv = -1, pt = -1;
        long pe = -1, pbeat = -1;
        bool prevBeamable = false;   // 上一个事件本身可连梁；长音会把梁截断
        for (size_t e = 0; e < bev.size(); ++e) {
            const BeamEvent& ev = bev[e];
            const bool sameCtx = (ev.bar == pb && ev.staff == ps && ev.voice == pv);
            const bool cont = prevBeamable && ev.level > 0 && sameCtx &&
                              ev.start == pe && ev.trip == pt &&
                              (ev.trip != 0 || ev.beat == pbeat);
            if (!cont) flushBeamGroup(beamGrp);   // 断点：先收上一组
            if (ev.level > 0) beamGrp.push_back((int)e);
            for (int si : ev.segs) beamOf[si].levels = ev.level;
            pb = ev.bar; ps = ev.staff; pv = ev.voice; pt = ev.trip;
            pe = ev.end; pbeat = ev.beat;
            prevBeamable = (ev.level > 0);
        }
        flushBeamGroup(beamGrp);

        // ---- 3. 谱号：音域偏低时用低音谱号，否则高音谱号 ----
        // 不判断的话钢琴左手会用高音谱号写，满屏加线。
        double pitchSum = 0;
        for (const auto& n : ns) pitchSum += n.pitch;
        double avgPitch = pitchSum / (double)ns.size();
        const bool bassClef = avgPitch < 58.0;  // 约 A#3 以下

        // ---- 4. 输出 ----
        xml << "  <part id=\"P" << partIdx << "\">\n";

        int curBar = -1;
        long writtenUpTo = 0;   // 当前小节内已写到的绝对 tick
        long lastSegStart = -1; // 识别同拍和弦
        long lastSegDur = -1;   // 和弦成员必须同拍且同值才是真和弦
        int curVoice = -1;      // 当前正在写的声部
        int curStaff = -1;      // 当前正在写的谱表（大谱表下 1=上行 2=下行）

        auto emitRest = [&](long from, long to, int voice) {
            if (to <= from) return;
            const long len = to - from;
            const std::vector<RestPart> parts = splitRestDuration(len, division);
            // 单段且精确时，输出与旧实现逐字节一致（durTypeDot 的精确匹配档）。
            // 无法精确拆分的极不规则 tick 也保留旧兜底，不动时间推进。
            if (parts.empty()) {
                auto [rt, rd] = durTypeDot(len / (double)division);
                xml << "      <note><rest/><duration>" << len
                    << "</duration><voice>" << (voice + 1) << "</voice><type>" << rt << "</type>";
                if (rd) xml << "<dot/>";
                if (grand && curStaff >= 1) xml << "<staff>" << curStaff << "</staff>";
                xml << "</note>\n";
                return;
            }
            for (const auto& p : parts) {
                xml << "      <note><rest/><duration>" << p.ticks
                    << "</duration><voice>" << (voice + 1) << "</voice><type>" << p.type << "</type>";
                if (p.dotted) xml << "<dot/>";
                // 大谱表：休止也要标到对应谱表，否则落到上行谱与音符错位。
                if (grand && curStaff >= 1) xml << "<staff>" << curStaff << "</staff>";
                xml << "</note>\n";
            }
        };

        // <attributes> 只能在第一个真正写出来的小节里出现一次。
        // 原来写死成 `bar == 0`，一旦乐曲开头有整拍休止（前几个小节由下面的
        // 空小节补齐逻辑生成），attributes 就整个丢失，divisions / 拍号 / 调号 /
        // 谱号全没了，导出的文件 MuseScore 打不开。
        // 中途变拍号时，在变化的那一小节再写一次只含 <time> 的 attributes：
        // 少写这一次，MuseScore 会把后半首也按曲首拍号解释，小节线全错。
        bool wroteAttrs = false;
        auto emitKeyXml = [&](const KeyEstimate& k) {
            xml << "        <key><fifths>" << keyFifths(k) << "</fifths>"
                << "<mode>" << (k.isMajor ? "major" : "minor") << "</mode></key>\n";
        };
        auto emitTimeXml = [&](const TimeSignature& sig) {
            xml << "        <time><beats>" << sig.numerator
                << "</beats><beat-type>" << (1 << sig.denomPow)
                << "</beat-type></time>\n";
        };
        auto emitAttributes = [&](int bar) {
            const bool meterChanged = grid.meterChangedAt(bar);
            const bool keyChanged = keys.keyChangedAtBar(bar, grid);
            const KeyEstimate k = keys.keyAtBar(bar, grid);
            const TimeSignature sig = grid.sigForBar(bar);
            if (!wroteAttrs) {
                wroteAttrs = true;
                xml << "      <attributes>\n"
                    // divisions 用文件自身的时值分辨率，保证 duration 全为整数
                    << "        <divisions>" << division << "</divisions>\n";
                emitKeyXml(k);
                emitTimeXml(sig);
                if (grand) {
                    // 大谱表：上行谱用高音谱号、下行谱用低音谱号，并声明两行谱。
                    xml << "        <staves>2</staves>\n"
                        << "        <clef number=\"1\"><sign>G</sign><line>2</line></clef>\n"
                        << "        <clef number=\"2\"><sign>F</sign><line>4</line></clef>\n";
                } else {
                    xml << "        <clef><sign>" << (bassClef ? "F" : "G") << "</sign><line>"
                        << (bassClef ? 4 : 2) << "</line></clef>\n";
                }
                xml << "      </attributes>\n";
                // 速度标记只写一遍，放在第一个声部
                if (partIdx == 1) {
                    xml << "      <direction placement=\"above\">\n"
                        << "        <direction-type><metronome>"
                        << "<beat-unit>quarter</beat-unit>"
                        << "<per-minute>" << (int)std::lround(bpm) << "</per-minute>"
                        << "</metronome></direction-type>"
                        << "<sound tempo=\"" << (int)std::lround(bpm) << "\"/>\n"
                        << "      </direction>\n";
                    // 民族调式在 MusicXML 里没有标准 <mode> 值，音高/调号仍按
                    // 宫音记为大调，把「G 徵调式」作为文字标记写在谱头，导入
                    // MuseScore 后能看见。label 由程序构造，不含 XML 特殊字符。
                    if (!key.scaleLabel.empty()) {
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><words>"
                            << xmlEscape(key.scaleLabel)
                            << "</words></direction-type>\n"
                            << "      </direction>\n";
                    }
                }
            } else if (meterChanged || keyChanged) {
                // attributes 的子元素有固定顺序：<key> 必须排在 <time> 前面，
                // 否则某些记谱软件只认到第一个。两者同时变化时合进一个块。
                xml << "      <attributes>\n";
                if (keyChanged) emitKeyXml(k);
                if (meterChanged) emitTimeXml(sig);
                xml << "      </attributes>\n";
            }
        };

        // 大谱表下 <direction> 的 <staff> 必须跟着「这条记号实际影响的音符」
        // 走，不能用全曲多数侧。钢琴曲里同一声部常整小节移到另一行谱（左手
        // 独奏段、右手华彩段），若按全曲多数算，力度 / 渐变会印到完全没有
        // 音符的那一行谱上，真正发声的谱表反而看不到记号。下面按「本小节、
        // 本声部、该拍位之后最近的音头」取谱表；整小节都是休止时才用全曲
        // 多数侧兜底。segs 已按 (小节, 谱表, 声部, tick) 排好序，但同一声部
        // 两行谱的片段并不相邻，所以要取 start 最小的那个而不是第一个命中。
        auto staffAt = [&](int bar, int voice, long tick) -> int {
            if (!grand) return 1;
            const Segment* next = nullptr;   // tick 之后（含）最近的音头
            const Segment* last = nullptr;   // 本小节最后一个音头（兜底）
            for (const auto& s : segs) {
                if (s.bar != bar || s.voice != voice) continue;
                if (!last || s.start > last->start) last = &s;
                if (s.start >= tick && (!next || s.start < next->start)) next = &s;
            }
            if (next) return next->staff;
            if (last) return last->staff;
            return voiceStaff[voice];
        };

        // 演奏表情写进 <direction>：必须排在 <attributes> 之后、第一个
        // <note> 之前，MuseScore 才会把它显示在小节上方而不是吞掉。
        // 每个 <voice> 各按自己的主导通道写力度与楔形、各记一份 lastDyn：
        // 同轨复调里第二声部的 crescendo / p→ff 不再被第一声部吞掉。
        // MusicXML 4.0 的 <direction> 子元素顺序是 direction-type+ →
        // offset? → voice? → staff? → sound?，<voice> 只能插在 sound 之前。
        auto emitDirections = [&](int bar) {
            if (!expr.enabled()) return;
            const long bs = grid.barStart(bar), be = grid.barEnd(bar);
            const int vmax = (int)voiceChannel.size();
            for (int v = 0; v < vmax; ++v) {
                const int ch = voiceChannel[v];
                if (ch < 0) continue;
                // 单声部保持旧版逐字节输出；多声部才补 <voice> 归属，并让
                // 每个声部用独立的 wedge number，避免两条楔形抢 number="1"。
                const std::string voiceTag = (vmax > 1)
                    ? ("        <voice>" + std::to_string(v + 1) + "</voice>\n")
                    : std::string();
                const std::string wedgeNum = std::to_string(v + 1);
                // 大谱表：每条方向记号按自己的拍位找该声部最近的音符，标到
                // 那颗音所在的谱表；声部在小节之间换谱表时记号不会留在空白谱上。
                auto staffTagFor = [&](long tick) -> std::string {
                    if (!grand) return std::string();
                    return "        <staff>" + std::to_string(staffAt(bar, v, tick))
                         + "</staff>\n";
                };
                const std::string dyn = expr.dynamicAt(bs, ch);
                if (!dyn.empty() && dyn != lastDyn[v]) {
                    xml << "      <direction placement=\"above\">\n"
                        << "        <direction-type><dynamics><" << dyn
                        << "/></dynamics></direction-type>\n"
                        << voiceTag
                        << staffTagFor(bs)
                        << "        <sound dynamics=\"" << dynamicToMidiVolume(dyn)
                        << "\"/>\n"
                        << "      </direction>\n";
                    lastDyn[v] = dyn;
                }
                // 小节中途的孤立换档：写回真实拍位。旧实现只在 barStart 取样，
                // 「第 2 拍从 ff 掉到 f」要等到下一小节才印出来，整条力度标记
                // 被推迟三拍，MuseScore 播放与谱面对不上。渐变段内部的中间档位
                // 由楔形表达，dynamicStepsInside() 已排除，不会多印。
                // <offset> 单位 = divisions（tick），必须排在 direction-type
                // 之后、voice/staff/sound 之前。
                for (const auto& m : expr.dynamicStepsInside(bs, be, ch)) {
                    if (m.dynamic == lastDyn[v]) continue;
                    xml << "      <direction placement=\"above\">\n"
                        << "        <direction-type><dynamics><" << m.dynamic
                        << "/></dynamics></direction-type>\n"
                        << "        <offset>" << (m.tick - bs) << "</offset>\n"
                        << voiceTag
                        << staffTagFor(m.tick)
                        << "        <sound dynamics=\"" << dynamicToMidiVolume(m.dynamic)
                        << "\"/>\n"
                        << "      </direction>\n";
                    lastDyn[v] = m.dynamic;
                }
                // 段落名 / 速度变化属于整份谱，只在第一个 part 的第一个
                // 声部写一次；力度与渐强渐弱按各自声部的通道写。
                if (partIdx == 1 && v == 0) {
                    // 速度记号：阶跃写 ♩=N；渐变写 rit. / accel.，并在它落定的
                    // 小节补一个终点速度（初始速度已在 emitAttributes 写）。
                    //
                    // 变速点很少正好落在小节线上：DAW 导出的 tempo map 常在
                    // 第 3 拍、渐慢的落定点也常在小节中途。旧实现一律写在小节
                    // 开头，MuseScore 于是把整小节都按新速度播放，前半小节被
                    // 提前变速。这里用 <offset>（单位 = divisions，即 tick）
                    // 把落点写回小节内的真实位置；正好落在小节线时不写，既有
                    // 素材输出逐字节不变。
                    auto tempoOffset = [&](long tick, bool ownLine) -> std::string {
                        const long off = tick - bs;
                        // 落在小节线上时返回空串：两种旧排版（阶跃 / 渐变落定
                        // 的 <sound> 紧贴 </direction-type>，rit. 起点的 <sound>
                        // 另起一行）逐字节保留，既有素材输出才不会变。
                        if (off <= 0) return ownLine ? "\n        " : std::string();
                        return "\n        <offset>" + std::to_string(off)
                             + "</offset>\n        ";
                    };
                    if (auto it = rampStartAtBar.find(bar); it != rampStartAtBar.end()) {
                        const TempoMark& m = *it->second;
                        const char* word = (m.kind == TempoMarkKind::Ritardando)
                                         ? "rit." : "accel.";
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><words>" << word
                            << "</words></direction-type>"
                            << tempoOffset(m.tick, true)
                            << "<sound tempo=\"" << m.bpm << "\"/>\n"
                            << "      </direction>\n";
                    }
                    if (auto it = stepAtBar.find(bar); it != stepAtBar.end()) {
                        // 同小节的多个阶跃各写一条，位置由各自的 <offset> 决定。
                        for (const TempoMark* sm : it->second) {
                            xml << "      <direction placement=\"above\">\n"
                                << "        <direction-type><metronome>"
                                << "<beat-unit>quarter</beat-unit>"
                                << "<per-minute>" << sm->bpm << "</per-minute>"
                                << "</metronome></direction-type>"
                                << tempoOffset(sm->tick, false)
                                << "<sound tempo=\"" << sm->bpm << "\"/>\n"
                                << "      </direction>\n";
                        }
                    } else if (auto rit = rampEndAtBar.find(bar); rit != rampEndAtBar.end() &&
                               rampStartAtBar.find(bar) == rampStartAtBar.end()) {
                        // 渐变落定：写终点速度，MuseScore 播放在这里跳到目标 BPM。
                        // 与它同小节已有的阶跃记号不重复写（阶跃更权威）。
                        const TempoMark& m = *rit->second;
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><metronome>"
                            << "<beat-unit>quarter</beat-unit>"
                            << "<per-minute>" << m.toBpm << "</per-minute>"
                            << "</metronome></direction-type>"
                            << tempoOffset(m.endTick, false)
                            << "<sound tempo=\"" << m.toBpm << "\"/>\n"
                            << "      </direction>\n";
                    }
                    for (const auto& m : expr.inRange(bs, be, ch)) {
                        if (!m.section.empty())
                            xml << "      <direction placement=\"above\">\n"
                                << "        <direction-type><words>"
                                << xmlEscape(m.section) << "</words></direction-type>\n"
                                << "      </direction>\n";
                    }
                }
                // 渐强渐弱是通道级演奏表情：每个 part / 每个 voice 按自己的
                // 主导通道各写一份，方向记号用 <voice> 标明归属。
                // 一条楔形必须落同一谱表：以起点音头所在谱表为锚，stop 复用
                // 同一 <staff>，不要再按各自拍位重新解析——大谱表里左手段与
                // 右手段的渐强，若起点在上一行、终点在下一行，旧写法把一条
                // hairpin 拆到两个谱表，MuseScore 渲染成断裂的楔形。
                for (const auto& m : expr.inRange(bs, be, ch)) {
                    if (m.wedge != 0) {
                        const int anchor = (grand ? staffAt(bar, v, m.tick) : 1);
                        wedgeAnchorStaff[v] = anchor;
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><wedge type=\""
                            << (m.wedge > 0 ? "crescendo" : "diminuendo")
                            << "\" number=\"" << wedgeNum << "\"/></direction-type>\n"
                            << voiceTag
                            << (grand ? ("        <staff>" + std::to_string(anchor)
                                         + "</staff>\n") : std::string())
                            << "      </direction>\n";
                    }
                    if (m.wedgeStop) {
                        const int anchor = (wedgeAnchorStaff[v] > 0)
                            ? wedgeAnchorStaff[v] : (grand ? staffAt(bar, v, m.tick) : 1);
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><wedge type=\"stop\" "
                            << "number=\"" << wedgeNum << "\"/></direction-type>\n"
                            << voiceTag
                            << (grand ? ("        <staff>" + std::to_string(anchor)
                                         + "</staff>\n") : std::string())
                            << "      </direction>\n";
                        wedgeAnchorStaff[v] = -1;
                    }
                }
            }
        };

        // 弱起小节要标 implicit="yes"，否则 MuseScore 会把它当成一个完整小节，
        // 并要求补齐 4 拍；小节号也从 0 起算，第一个完整小节显示成 1。
        auto openMeasure = [&](int bar) {
            xml << "    <measure number=\"" << grid.measureNumber(bar) << "\"";
            if (grid.pickup > 0 && bar == 0) xml << " implicit=\"yes\"";
            xml << ">\n";
            emitAttributes(bar);
            emitDirections(bar);
            pendingChords.clear();
            pendingIdx = 0;
            if (writeChords) {
                auto it = chordsByBar.find(bar);
                if (it != chordsByBar.end()) pendingChords = it->second;
            }
            writtenUpTo = grid.barStart(bar);
            curVoice = -1;
            curStaff = -1;
            lastSegStart = -1;
            lastSegDur = -1;
        };

        // 收尾当前声部：补上小节末尾的休止，把还没落笔的和弦符号写掉。
        // 换声部前必须先调一次，否则 <backup> 之后这一声部的尾巴就丢了。
        auto finishVoice = [&]() {
            long barEndTick = grid.barEnd(curBar);
            // 小节末尾还没被音符消费的和弦（后半小节是休止时常见）：先补到
            // 它所在的拍位再写符号，否则和弦会被顶到小节第一拍。
            // 和弦只写一份，交给最高声部。
            if (writeChords && curVoice == 0) {
                while (pendingIdx < pendingChords.size()) {
                    const ChordMark& cm = *pendingChords[pendingIdx++];
                    if (writtenUpTo < cm.startTick) {
                        emitRest(writtenUpTo, cm.startTick, curVoice);
                        writtenUpTo = cm.startTick;
                    }
                    emitHarmonyXml(xml, cm, prefersFlats(keys.at(cm.startTick)));
                }
            }
            if (curVoice >= 0 && writtenUpTo < barEndTick)
                emitRest(writtenUpTo, barEndTick, curVoice);
        };

        auto closeMeasure = [&]() {
            finishVoice();
            xml << "    </measure>\n";
        };

        for (size_t si = 0; si < segs.size(); ++si) {
            const Segment& seg = segs[si];
            const BeamInfo& bi = beamOf[si];
            if (seg.bar != curBar) {
                if (curBar >= 0) closeMeasure();

                // 补齐中间完全空的小节，否则小节编号会跳号
                for (int b = curBar + 1; b < seg.bar; b++) {
                    xml << "    <measure number=\"" << grid.measureNumber(b) << "\"";
                    // 弱起小节在别的声部里是空小节，同样要标 implicit，
                    // 否则各声部的小节号与 MuseScore 的编号对不上
                    if (grid.pickup > 0 && b == 0) xml << " implicit=\"yes\"";
                    xml << ">\n";
                    emitAttributes(b);   // 开头就是空小节时，属性要写在这里
                    emitDirections(b);  // 段落标记可能落在纯休止的前奏
                    if (grand) {
                        // 大谱表：上下两行谱各写一条全小节休止，中间退回谱表开头。
                        xml << "      <note><rest measure=\"yes\"/><duration>"
                            << grid.barSpan(b) << "</duration><voice>1</voice>"
                            << "<type>whole</type><staff>1</staff></note>\n"
                            << "      <backup><duration>" << grid.barSpan(b)
                            << "</duration></backup>\n"
                            << "      <note><rest measure=\"yes\"/><duration>"
                            << grid.barSpan(b) << "</duration><voice>1</voice>"
                            << "<type>whole</type><staff>2</staff></note>\n";
                    } else {
                        xml << "      <note><rest measure=\"yes\"/><duration>"
                            << grid.barSpan(b) << "</duration><voice>1</voice>"
                            << "<type>whole</type></note>\n";
                    }
                    xml << "    </measure>\n";
                }

                curBar = seg.bar;
                openMeasure(curBar);
                lastSegStart = -1;
                lastSegDur = -1;
            }

            // 大谱表：上行谱(1)写完换下行谱(2)时，先把本节已写满的谱表收尾
            // （用休止补到小节末），再退回小节开头从头写另一个谱表。这一步必须
            // 在声部切换之前：声部切换也用 <backup> 退回小节开头，二者共用同一
            // 条游标，顺序错了会多退一次跑到小节线前面。
            if (seg.staff != curStaff && curVoice >= 0) {
                finishVoice();
                xml << "      <backup><duration>" << grid.barSpan(curBar)
                    << "</duration></backup>\n";
                curVoice = -1;
                writtenUpTo = grid.barStart(curBar);
                lastSegStart = -1;
                lastSegDur = -1;
            }
            curStaff = seg.staff;

            // 同轨复调：一个声部写完后回到小节开头写下一个声部。
            // MusicXML 用 <backup> 把「当前时间游标」退回去，退的正好是一个
            // 小节的长度；不写这一步，第二个声部就会接在第一个声部后面继续
            // 排，小节时值成倍溢出。
            if (seg.voice != curVoice) {
                if (curVoice >= 0) {
                    finishVoice();
                    xml << "      <backup><duration>" << grid.barSpan(curBar)
                        << "</duration></backup>\n";
                }
                curVoice = seg.voice;
                writtenUpTo = grid.barStart(curBar);
                lastSegStart = -1;
                lastSegDur = -1;
            }

            // 间隙补休止符（和弦成员不重复补）。
            // 真和弦的判据必须带上时值：同拍不同值（长音叠短音）不是和弦，
            // 那是两个声部，分声部之后本就不会走到这里。
            bool isChordMember = (seg.start == lastSegStart && seg.dur == lastSegDur);
            if (!isChordMember && seg.start > writtenUpTo)
                emitRest(writtenUpTo, seg.start, curVoice);

            // 和弦符号按音头位置插入：此刻文档已推进到 seg.start，写在这里
            // 就正好落在该拍上方。只在最高声部写一次。
            if (curVoice == 0) flushChords(seg.start);

            int pc = seg.pitch % 12;
            if (pc < 0) pc += 12;
            int octave = seg.pitch / 12 - 1;
            const bool flat = prefersFlats(keys.at(seg.start));
            const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
            int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];

            double beats = seg.dur / (double)division;
            // 三连音：<type> 写名义时值，实际缩放交给 <time-modification>
            auto [ntype, ndot] = durTypeDot(seg.triplet ? beats * 1.5 : beats);

            // ---- 歌词与一字多音 ----
            // 只挂在旋律轨的「真实音头」上：跨小节切出来的延续段（tieEnd）
            // 与和弦的第二、三个音（chordMember）都不是新音节，重复写会让
            // MuseScore 把同一个字显示两遍。
            // 一个字唱多个音时：首音 syllabic=begin + slur start，中间音
            // middle，末音 end + slur stop；一音一字则写 single。
            std::string lyricXml, slurXml;
            if (!lyrics.empty() && seg.voice == 0 && !seg.tieEnd && !isChordMember) {
                auto pit = lyrics.at.find(seg.start);
                if (pit != lyrics.at.end()) {
                    const LyricPlacement& pl = pit->second;
                    const bool multi = (pl.noteCount >= 2);
                    lyricXml = std::string("<lyric number=\"1\"><syllabic>")
                             + (multi ? "begin" : "single")
                             + "</syllabic><text>" + xmlEscape(pl.text) + "</text></lyric>";
                    if (multi) slurXml = "<slur type=\"start\" number=\"1\"/>";
                } else {
                    int g = lyrics.groupAt(seg.start);
                    if (g > 0) {
                        const LyricPlacement& pl = lyrics.melismas[g - 1];
                        const bool last = (seg.start == pl.lastNoteTick);
                        // <extend/> 是 MusicXML 里表示「这个字还在拖」的标准
                        // 元素；只写 syllabic 不写 text 有些记谱软件会当成空词。
                        lyricXml = std::string("<lyric number=\"1\"><syllabic>")
                                 + (last ? "end" : "middle")
                                 + "</syllabic><extend/></lyric>";
                        if (last) slurXml = "<slur type=\"stop\" number=\"1\"/>";
                    }
                }
            }

            // 倚音（装饰音）：在主音之前写一个 <grace/> 音符，不占时长，
            // MuseScore 会把它渲染成主音左侧的小装饰音。只在主音的第一个分段
            // （真实音头，非跨小节延续段、非和弦成员）上写一次。
            if (!isChordMember && !seg.tieEnd && seg.voice == 0) {
                const NoteExpression ae0 = expr.noteAt(seg.start, seg.channel);
                if (ae0.hasGrace()) {
                    int gp = ae0.gracePitch;
                    double goff = bendOffset(seg.start - division / 16,
                                            seg.start, seg.channel);
                    if (std::abs(goff) >= 0.5) gp += (int)std::round(goff);
                    int gpc = gp % 12; if (gpc < 0) gpc += 12;
                    int goct = gp / 12 - 1;
                    const bool gflat = prefersFlats(keys.at(seg.start));
                    const char* gstep = gflat ? FLAT_STEP[gpc] : SHARP_STEP[gpc];
                    int galter = gflat ? FLAT_ALTER[gpc] : SHARP_ALTER[gpc];
                    xml << "      <note><grace/>"
                        << "<pitch><step>" << gstep << "</step>";
                    if (galter != 0) xml << "<alter>" << galter << "</alter>";
                    xml << "<octave>" << goct << "</octave></pitch>"
                        << "<voice>1</voice><type>eighth</type>";
                    if (grand) xml << "<staff>" << seg.staff << "</staff>";
                    xml << "</note>\n";
                }
            }

            xml << "      <note>";
            if (isChordMember) xml << "<chord/>";
            xml << "<pitch><step>" << step << "</step>";
            if (alter != 0) xml << "<alter>" << alter << "</alter>";
            xml << "<octave>" << octave << "</octave></pitch>"
                << "<duration>" << seg.dur << "</duration>";
            if (seg.tieEnd)   xml << "<tie type=\"stop\"/>";
            if (seg.tieStart) xml << "<tie type=\"start\"/>";
            xml << "<voice>" << (seg.voice + 1) << "</voice><type>" << ntype << "</type>";
            if (ndot) xml << "<dot/>";
            if (seg.triplet) {
                xml << "<time-modification>"
                    << "<actual-notes>3</actual-notes>"
                    << "<normal-notes>2</normal-notes>"
                    << "</time-modification>";
            }
            // 大谱表：每个音符标明所属谱表，否则 MuseScore 全堆到上行谱。
            if (grand) xml << "<staff>" << seg.staff << "</staff>";
            // 连梁：MusicXML 的 note 子元素顺序是 <type>/<dot>/
            // <time-modification> -> <staff> -> <beam> -> <notations>，
            // 写反了严格校验的软件会整条丢掉。
            for (int L = 1; L <= bi.levels; ++L) {
                int bst = bi.state[L];
                if (bst == 0) continue;
                xml << "<beam number=\"" << L << "\">" << kBeamWord[bst] << "</beam>";
            }
            // MusicXML 的 note 子元素有固定顺序：notations 在 lyric 之前。
            // tied 与 slur 必须合进同一个 <notations>，分两个写会让某些
            // 记谱软件只认第一个。
            std::string notations;
            if (seg.tieEnd)   notations += "<tied type=\"stop\"/>";
            if (seg.tieStart) notations += "<tied type=\"start\"/>";
            notations += slurXml;
            // 断音 / 重音 / 滑音 / 颤音挂在真实音头上：和弦成员共用第一音的
            // 记号，跨小节切出来的延续段 seg.start 不是音头，自然查不到。
            if (!isChordMember) {
                const NoteExpression ae = expr.noteAt(seg.start, seg.channel);
                std::string arts;
                if (ae.staccatissimo) arts += "<staccatissimo/>";
                else if (ae.staccato) arts += "<staccato/>";
                if (ae.accent) arts += "<accent/>";
                if (!arts.empty())
                    notations += "<articulations>" + arts + "</articulations>";
                if (ae.bend == BendMark::Trill) {
                    notations += "<ornaments><trill-mark/></ornaments>";
                } else if (ae.bend == BendMark::SlideUp ||
                           ae.bend == BendMark::SlideDown) {
                    // 同一个音符内 start+stop 表示「音内滑音」，不需要跨小节配对。
                    notations += "<slide line-type=\"solid\" number=\"2\" "
                                 "type=\"start\"/><slide line-type=\"solid\" "
                                 "number=\"2\" type=\"stop\"/>";
                }
            }
            if (!notations.empty()) xml << "<notations>" << notations << "</notations>";
            xml << lyricXml;
            xml << "</note>\n";

            lastSegStart = seg.start;
            lastSegDur = seg.dur;
            writtenUpTo = std::max(writtenUpTo, seg.start + seg.dur);
        }

        if (curBar >= 0) closeMeasure();
        xml << "  </part>\n";
        partIdx++;
    }

    xml << "</score-partwise>\n";
    return xml.str();
}

} // namespace m2j

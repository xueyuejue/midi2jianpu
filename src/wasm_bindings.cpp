// wasm_bindings.cpp - WASM C 风格绑定
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// 简单字符串缓冲区
static std::string g_result;
static std::string g_lyrics;  // 歌词文本(LRC格式)
static std::string g_title;   // 谱面标题（网页传文件名进来）
static bool g_useMidiLyrics = true;  // 是否使用 MIDI 自带的 0x05 歌词
static bool g_usePentatonic = true;  // 是否按五声调式校正宫音
static bool g_useDynamics = true;    // 是否写出力度 / 演奏法 / 段落标记
static int g_trackFilter = -1;       // 网页段落选择：-1 = 全部轨，与 CLI --track 共用
static double g_softPedalScale = 0.7; // 网页软踏板强度下拉：与 CLI --soft-pedal-scale 共用
static int g_barsPerRow = 0;          // 每行小节数，0 = 自动折算；与 CLI --bars 共用
// 网页弱起（v1.166.0）：与 CLI --pickup / --no-pickup 共用 applyPickupOption()。
// -1 = 自动判定（CLI 默认），0 = 关闭弱起（--no-pickup），>0 = 手动拍数。
static double g_pickupBeats = -1;
// 最近一次渲染里手动弱起被拒的原因（越界等），网页如实提示，绝不静默吞参数。
static std::string g_pickupError;
// 网页手动宫音（v1.167.0）：-1 = 不指定（沿用自动判定 / 网页调性下拉），
// 0..11 = 宫音音级。kFifthsAuto 之外的值记住 Bb/Gb 这类降号写法，
// 与 CLI --gong Bb 走同一条换算；越界时 set_gong 拒绝，不静默钳位。
static int g_gongPc = -1;
static int g_gongHint = m2j::kFifthsAuto;
// 网页时值量化（v1.168.0）：与 CLI --no-quantize 同一开关，默认开。
// 关掉后保留 MIDI 原始时长，人演奏的提前 note-off 会记成附点十六等非常规档。
static bool g_quantize = true;
static int g_quantizeAdjusted = 0;  // 最近一次渲染修正掉的音符数（页面如实提示）
// 网页文本谱行首小节号（v1.169.0）：与 CLI --bar-numbers 同一个布尔，
// 只作用于复制出去的文本谱（formatJianpu 第 3 参）；HTML/XML 本就无条件
// 显示小节号，这个开关对它们没有差别，与 CLI 的警告口径一致。
static bool g_barNumbers = false;
// 网页移调（v1.164.0）：与 CLI --transpose 共用 transposeData()。
static int g_transpose = 0;        // 半音数，0 = 不移调
static int g_transposeFolded = 0;  // 上一次移调按八度折回的音数（页面如实报告）
static bool g_keySigMoved = false; // 本次移调是否真的让文件自带调号变了（整八度不动）

// 网页上的两个渲染开关。清空内置歌词事件表而不是只跳过渲染：歌词事件同时
// 参与旋律轨判定，关掉就得连证据一起去掉。五声开关则直接落在 MidiData 上，
// CLI 的 --no-pentatonic 走的是同一个字段。
static void applyRenderOptions(m2j::MidiData& data) {
    if (!g_useMidiLyrics) data.midiLyrics.clear();
    data.usePentatonic = g_usePentatonic;
    data.useDynamics = g_useDynamics;
    // 弱起换算与 CLI 共用 applyPickupOption()。pickupTicks 只参与小节网格
    // （barGridOf）和页眉标签，量化与歌词铺排都不读它，所以放在量化前与
    // CLI 的「量化后设置」结果逐字节一致；四个渲染入口（文本/HTML/XML/
    // 歌词体检）都经过这里，不会漏。越界时保留自动判定并记下原因，绝不
    // 把半设置的越界值写进 data。
    // 网页的 0 = 「无弱起」（等价于 CLI --no-pickup）；-1 = 自动；>0 = 手动。
    g_pickupError = m2j::applyPickupOption(
        data, g_pickupBeats, g_pickupBeats == 0);
}

// 解析前的统一入口：CC67 软踏板力度衰减发生在解析阶段（applyControllerSemantics
// 直接改 notes.velocity），所以软踏板系数必须在 loadMidiFromMemory 之前落到
// MidiData 上，否则网页端的下拉就改不到谱面。所有读取文件的入口都走这里，
// 避免每个函数各自散落赋值。
static bool loadJ(const uint8_t* midiData, int size, m2j::MidiData& data,
                  bool keepAllFormat2, int trackFilter) {
    data.softPedalScale = g_softPedalScale;
    return m2j::loadMidiFromMemory(midiData, size, data, keepAllFormat2, trackFilter);
}

extern "C" {

// --transpose 的合理上限，与 CLI 的 kMaxTransposeSemis 同值（±5 个八度）。
static const int kMaxTransposeSemis = 60;

// 移调必须落在「解析完 → 量化前」这一段，位置与 CLI 完全一致
// （loadMidi → transposeData → quantizeTiming → estimateKey）。放在别处就会
// 出现「谱面音高移了、调性还按原调猜」或「量化网格跟着音高走」这类分叉。
// 每次渲染都从新解析出来的 data 开始，所以这里的移调不会跨次累加。
static void applyTranspose(m2j::MidiData& data) {
    g_transposeFolded = 0;
    g_keySigMoved = false;
    if (g_transpose == 0) return;
    const int sfBefore = data.keySig.valid ? data.keySig.sharpsFlats : 0;
    g_transposeFolded = m2j::transposeData(data, g_transpose);
    // 移整个八度时调号不动，页眉也就不能声称「已随移调平移」。
    g_keySigMoved = data.keySig.valid && data.keySig.sharpsFlats != sfBefore;
}

// 三个渲染入口（文本 / HTML / MusicXML）共用的时值量化。CLI 是
// `if (quantize) quantizeTiming(data);`，网页这层只是把布尔开关换成全局值，
// 调用的仍是同一个 m2j::quantizeTiming()，没有第二套网格/门限口径。
// 每次渲染前归零 adjusted，避免关掉量化后状态行还顶着上一次的「修正 N 个」。
static void applyQuantize(m2j::MidiData& data) {
    g_quantizeAdjusted = 0;
    if (!g_quantize) return;
    g_quantizeAdjusted = m2j::quantizeTiming(data).adjusted;
}

// 三个渲染入口（文本 / HTML / MusicXML）共用的调性决策。手动宫音优先于
// 手动调号，与 CLI 的 `--gong 优先于 --key` 同口径；越界的宫音在 set_gong
// 那里就被拒了，这里拿到的必定是合法值。
static m2j::KeyEstimate chooseKey(const m2j::MidiData& data, int keyPitch,
                                  int isMajor) {
    if (g_gongPc >= 0)
        return m2j::keyFromGongTransposed(
            data, g_gongPc, g_gongHint, (g_transpose % 12) ? g_transpose : 0);
    if (keyPitch >= 0) {
        m2j::KeyEstimate chosen;
        chosen.tonicPitchClass = keyPitch;
        chosen.isMajor = isMajor != 0;
        return (g_transpose % 12) ? m2j::transposeKey(chosen, g_transpose)
                                  : chosen;
    }
    return m2j::estimateKey(data);
}

// HTML 页眉的措辞与 CLI 的 keySource 逐字一致：报用户选的原宫音，移调后
// 补一句「已随移调平移」，不把平移后的宫音记到用户头上。
static std::string gongSourceNote() {
    m2j::KeyEstimate g;
    g.tonicPitchClass = g_gongPc;
    g.isMajor = true;
    g.fifthsHint = g_gongHint;
    return "手动指定宫音 " + m2j::keyName(g)
         + ((g_transpose % 12) ? "，已随移调平移" : "");
}

// 网页端的段落选择：与 CLI --track 走同一条解析层按轨隔离路径。
// -1 表示全部轨（默认）。网页每次转换与每次刷新文件提示前都会重设。
void midi2jianpu_set_track(int track) {
    g_trackFilter = track;
}

// 按轨隔离提示：网页选段后说明只留下了哪一轨、丢了多少音符。
// 返回 char*，网页必须过 UTF8ToString。
const char* midi2jianpu_track_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data,
               g_trackFilter >= 0, g_trackFilter)) {
        buf.clear(); return buf.c_str();
    }
    buf = m2j::trackLabel(data);
    return buf.c_str();
}

// JSON 字符串转义：轨名是 0x03 里的自由文本，引号、反斜杠、换行都可能出现。
// C0 控制字符一律转成 \u00XX，绝不原样塞进 JSON（那会让网页的 JSON.parse
// 直接抛错，整份轨道下拉框变成空的）。
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // C0 与 DEL 一律走 \u00XX：JSON 里它们虽然合法，但把 ESC 原样
                // 带进网页，下拉框一拼 innerHTML 就又是终端转义注入那条路。
                if (c < 0x20 || c == 0x7F) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (int)c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// 网页的轨道下拉框：返回真实 MTrk 块的清单（JSON 数组）。
// 网页过去直接读 MThd offset 10..11 的 16 位轨数来生成选项，而那个数字经常
// 是错的（合并轨后忘了改 / 溢出成 0 / 文件被截断）：一份 4 轨的文件在页面上
// 可能只有 0 个选项（选不了任何轨），也可能凭空多出 3 个空轨（选了就报
// 「未提取到音符」）。统计口径与 CLI --list-tracks 共用 trackSummaries()。
const char* midi2jianpu_track_list(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    // 这里永远按「全曲」解析：下拉框要列出全部轨，不能受当前选中轨影响，
    // 否则用户选了第 2 轨之后下拉框就只剩一个选项了。format 2 的其它序列
    // 也要留着：默认解析会把第 1 段之后的轨名 / 音符 / 歌词全丢掉，页面
    // 于是把 SongB 显示成空轨。
    if (!loadJ(midiData, size, data, true, -1)) {
        g_result = "[]";
        return g_result.c_str();
    }
    const auto tracks = m2j::trackSummaries(data);
    std::string json = "[";
    bool first = true;
    for (const auto& t : tracks) {
        if (!first) json += ",";
        first = false;
        // channels：这条轨实际用到的逻辑通道（升序）。一条轨混多个通道时
        // 下拉框要说明「乐器名只代表主导通道」，否则用户没法解释同一条轨
        // 里出现的两种乐器。字段是纯数字数组，不需要转义。
        std::string chans = "[";
        for (size_t i = 0; i < t.channels.size(); i++) {
            if (i) chans += ",";
            chans += std::to_string(t.channels[i].first);
        }
        chans += "]";
        // insts：逐通道 GM 乐器名（通道号 + 名字），网页下拉框靠它解释
        // 「同一条轨为什么有两种乐器」。名字来自 MIDI 0xC0 / 轨名，
        // 仍要走 jsonEscape；通道号是纯数字。
        std::string insts = "[";
        for (size_t i = 0; i < t.channelInstruments.size(); i++) {
            if (i) insts += ",";
            insts += "{\"c\":" + std::to_string(t.channelInstruments[i].first) +
                     ",\"n\":\"" + jsonEscape(t.channelInstruments[i].second) + "\"}";
        }
        insts += "]";
        // druminsts：鼓轨被跳过的鼓件清单（中文名 + 次数），由诊断层统一
        // 生成，避免网页和命令行对同一份文件报出不同的鼓名。非鼓轨是空串。
        std::string druminsts;
        // 与 --list-tracks 同口径：track_list 是完整诊断，列全所有鼓件种类。
        // 默认 4 项上限只给空轨提示，这里传全量，网页下拉框不会在鼓件多时
        // 冒出一个没有下文的「等」。
        if (t.drumNotes > 0) druminsts = m2j::drumInstrumentList(t, t.drumInstruments.size());
        json += "{\"i\":" + std::to_string(t.index) +
                ",\"name\":\"" + jsonEscape(t.name) + "\"" +
                ",\"notes\":" + std::to_string(t.notes) +
                ",\"drum\":" + std::to_string(t.drumNotes) +
                ",\"lyrics\":" + std::to_string(t.lyrics) +
                ",\"inst\":\"" + jsonEscape(t.instrument) + "\"" +
                ",\"channels\":" + chans + ",\"insts\":" + insts +
                ",\"druminsts\":\"" + jsonEscape(druminsts) + "\"}";
    }
    json += "]";
    g_result = json;
    return g_result.c_str();
}

// 「选中的轨没有旋律音符」时要补的那一句：这条轨里到底有什么、旋律在哪条轨。
// 解析层按轨过滤后 data 里只剩所选的那条轨，别的轨的音符早丢了，说不出
// 「旋律在轨 1」——所以这里重新按全曲解析一次。只走这条出错路径，正常转换
// 不会付这个代价。
static std::string emptyTrackNote(const uint8_t* midiData, int size, int track) {
    m2j::MidiData full;
    if (!loadJ(midiData, size, full, false, -1)) return std::string();
    return m2j::emptyTrackHint(full, track);
}

// 「没有旋律音符」的整句话。选了某条轨时说的是**这条轨**，没选时才是
// **这个文件**。两者混用会出很具体的假话：drums.mid 有 32 个旋律音，
// 只是第 3 轨是鼓轨，旧代码却说「文件里只有打击乐轨」。
static std::string noNoteMessage(const m2j::MidiData& data, const uint8_t* midiData,
                                 int size) {
    if (g_trackFilter >= 0) {
        std::string msg = "错误: 轨 " + std::to_string(g_trackFilter) + " 没有旋律音符";
        const std::string hint = emptyTrackNote(midiData, size, g_trackFilter);
        if (!hint.empty()) msg += "——" + hint;
        return msg;
    }
    if (data.drumNotesSkipped > 0) return "错误: 文件里只有打击乐轨，简谱没有鼓谱记法";
    return "错误: 未提取到音符";
}

// 解析MIDI并返回文本简谱
// midiData: MIDI二进制数据指针
// size: 数据大小
// keyPitch: 调性(0=C, -1=自动)
// isMajor: 是否大调
const char* midi2jianpu_parse(const uint8_t* midiData, int size, int keyPitch, int isMajor) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) {
        g_result = "错误: 无法解析MIDI文件";
        return g_result.c_str();
    }
    if (data.notes.empty()) {
        g_result = noNoteMessage(data, midiData, size);
        return g_result.c_str();
    }

    applyTranspose(data);
    applyRenderOptions(data);
    applyQuantize(data);

    // 手动宫音 / 手动调号 / 自动判定三选一，与 CLI 同一入口。
    const m2j::KeyEstimate key = chooseKey(data, keyPitch, isMajor);

    // 与 CLI 的 `formatJianpu(data, key, barNumbers)` 同一处传参：网页勾选
    // 「文本谱小节号」后，「复制简谱」拿到的文本才有行首 [N] 前缀。
    g_result = m2j::formatJianpu(data, key, g_barNumbers);
    return g_result.c_str();
}

// 网页端的歌词解析入口（v1.111.0）：与 CLI 同一个格式判定，LRC 与 JSON 都吃。
// 此前这里写死 parseLrcText，用户把 CLI 那份 JSON 歌词（[{"start_beat":…,
// "text":…}]）贴进网页，一行都取不到，页面静默回退成 MIDI 内置的「LY」音节——
// 而同一份 JSON 在命令行里是好好的。detectLyricFormat("") 走纯内容嗅探，
// 与 CLI 对「扩展名不认识」的文件用的是同一条规则，三端不会各说各话。
static std::vector<m2j::LyricLine> parseLyricsAuto(const std::string& text,
                                                   const m2j::MidiData& data,
                                                   size_t* dropped = nullptr) {
    if (dropped) *dropped = 0;
    if (text.empty()) return {};
    if (m2j::detectLyricFormat("", text) == m2j::LyricFormat::Lrc)
        return m2j::parseLrcText(text, data, dropped);
    // JSON 路径与 CLI 完全一致：每个字自成一行，tick 由 start_beat 直接给。
    std::vector<m2j::LyricLine> out;
    for (const auto& s : m2j::parseLyricsText(text, data.division, dropped))
        out.push_back({s.tick, s.text});
    return out;
}

// 生成HTML谱面
const char* midi2jianpu_html(const uint8_t* midiData, int size, int keyPitch, int isMajor) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) {
        g_result = "<p>错误: 无法解析MIDI文件</p>";
        return g_result.c_str();
    }
    if (data.notes.empty()) {
        g_result = "<p>" + noNoteMessage(data, midiData, size) + "</p>";
        return g_result.c_str();
    }

    applyTranspose(data);
    applyRenderOptions(data);
    applyQuantize(data);

    m2j::KeyEstimate key;
    // 网页版选调与 CLI 的 --key 是同一件事：这个调是人选的，不是文件里写的。
    // 页眉照旧印「MIDI调号」就等于把用户的选择记到 MIDI 文件头上。
    // 宫音优先于调号，和 CLI 的 --gong / --key 冲突规则一致（网页在 UI 上
    // 让两个下拉互斥，这里再兜一次底，避免漏调用时两处口径分叉）。
    std::string keySourceNote;
    if (g_gongPc >= 0) {
        key = chooseKey(data, keyPitch, isMajor);
        keySourceNote = gongSourceNote();
    } else if (keyPitch >= 0) {
        m2j::KeyEstimate chosen;
        chosen.tonicPitchClass = keyPitch;
        chosen.isMajor = isMajor != 0;
        // 措辞与 CLI 的 --key 分支逐字一致：先说「用户选的原调」，再点明
        // 「已随移调平移」。反过来说成移调后的调，就等于把用户没选过的调
        // 记到用户头上。
        // 大调说 keyName（1=X 的音名），小调说主音名：a 小调若写成「手动指定
        // C」，用户会以为自己选错了调——他选的是 a 小调，只是简谱记 1=C。
        const std::string chosenName = chosen.isMajor
            ? m2j::keyName(chosen)
            : (m2j::keyTonicName(chosen) + "小调");
        const bool shifted = (g_transpose % 12) != 0;
        key = shifted ? m2j::transposeKey(chosen, g_transpose) : chosen;
        keySourceNote = "手动指定 " + chosenName + (shifted ? "，已随移调平移" : "");
    } else {
        key = m2j::estimateKey(data);
        // 文件里写着 0x59、而移调真的把调号挪了：页眉不能再说「MIDI调号」，
        // 否则用户会以为文件本来就写着这个调（CLI 页眉在 v1.156 起就这么说）。
        if (g_keySigMoved) keySourceNote = "MIDI调号，已随移调平移";
    }

    // 解析歌词。原来这里复制了一份 LRC 解析，且用裸 std::stoi / std::stod：
    // WASM 版默认关闭 C++ 异常，用户贴一段畸形 LRC 就会 abort 崩掉整页。
    // 现在统一走 notation.cpp 里的共享实现（同样不抛异常）。
    std::vector<m2j::LyricLine> lyricLines;
    if (!g_lyrics.empty()) lyricLines = parseLyricsAuto(g_lyrics, data);

    g_result = m2j::renderHtml(data, key, g_title.empty() ? "简谱" : g_title,
                              lyricLines.empty() ? nullptr : &lyricLines,
                              false, g_barsPerRow, keySourceNote);
    return g_result.c_str();
}

// 网页移调：半音数，范围与 CLI --transpose 完全一致（±60 = ±5 个八度）。
// 越界不静默钳位——用户以为移了 100 半音、谱面却只动了 5 个八度，比直接
// 拒绝更糟。返回实际生效的半音数，页面据此把下拉框拉回真实值。
int midi2jianpu_set_transpose(int semitones) {
    if (semitones < -kMaxTransposeSemis || semitones > kMaxTransposeSemis)
        return g_transpose;
    g_transpose = semitones;
    return g_transpose;
}

// 上一次渲染中移调越界（0~127 之外）按整八度折回的音数。CLI 会打一行
// 「移调越界: N 个音…」，网页也得说，否则高音区的谱子悄悄换了八度。
int midi2jianpu_transpose_folded() {
    return g_transposeFolded;
}

// 网页「每行小节数」：0 = 自动折算（沿用「一行约 16 拍」），1~8 = 固定行内
// 小节数，取值域与 CLI --bars 完全一致。只作用于 HTML —— 文本谱与
// MusicXML 本来就不分行，CLI 同参数下也只会印一句「只作用于 --html」。
// 越界不静默钳位，同 set_transpose：返回实际生效值，页面据此把下拉拉回。
int midi2jianpu_set_bars(int perRow) {
    if (perRow < 0 || perRow > 8) return g_barsPerRow;
    g_barsPerRow = perRow;
    return g_barsPerRow;
}

// 网页「弱起」：-1 = 自动判定（CLI 默认），0 = 关闭（--no-pickup），
// >0 = 手动拍数（--pickup N，须短于小节）。取值域与 CLI 的 parseBeats 一致：
// 非有限值（NaN/Inf）与过大拍数直接拒绝，返回实际生效值让页面把下拉拉回。
// 真正的「短于小节」校验要等解析出 timeSig，由 applyPickupOption 在渲染时做。
double midi2jianpu_set_pickup(double beats) {
    if (!std::isfinite(beats) || beats < -1 || beats > 64) return g_pickupBeats;
    g_pickupBeats = beats;
    return g_pickupBeats;
}

// 最近一次渲染里手动弱起被拒的原因（空串 = 没有）。网页把它显示出来：
// 用户给了越界拍数（如 4/4 里的 4 拍）时，谱面退回自动判定但不能不吭声。
const char* midi2jianpu_pickup_error() {
    return g_pickupError.c_str();
}

// 网页「宫音」：-1 = 不指定（沿用自动判定或网页调性下拉），0..11 = 宫音音级。
// hint 与 CLI 的 --gong 同名写法同口径：kFifthsAuto(99) 让 keyName 查表，
// Bb/Gb/Ab/Eb/Db 传负五度数保留用户的降号写法。越界一律拒绝并返回当前值，
//  页面据此把下拉拉回，绝不静默钳成一个用户没选的宫音。
int midi2jianpu_set_gong(int pc, int hint) {
    if (pc < -1 || pc > 11) return g_gongPc;
    if (hint != m2j::kFifthsAuto && (hint < -7 || hint > 7)) return g_gongPc;
    g_gongPc = pc;
    g_gongHint = (pc < 0) ? m2j::kFifthsAuto : hint;
    return g_gongPc;
}

// 网页「时值量化」开关：非 0 = 开（默认，与 CLI 一致），0 = 关（--no-quantize，
// 保留 MIDI 原始时长）。布尔开关没有越界概念，按 set_use_* 的口径接受任意整数。
// 返回实际生效值，页面据此反映当前状态。
int midi2jianpu_set_quantize(int on) {
    g_quantize = (on != 0);
    return g_quantize ? 1 : 0;
}

// 最近一次渲染里被量化修正掉的音符数。CLI 会打「时值量化: 修正 N 个音符」，
// 网页也得说——否则人演奏的 note-off 提前被悄悄抹平，用户以为谱面就是原样。
// 关掉量化或本次没有修正时返回 0。
int midi2jianpu_quantize_adjusted() {
    return g_quantizeAdjusted;
}

// 网页「文本谱小节号」开关：非 0 = 开，0 = 关（默认，与 CLI 一致）。
// 只作用于 midi2jianpu_parse 的文本谱，HTML/XML 本来就无条件印小节号，
// 此开关对它们没有差别——与 CLI 的 --bar-numbers 警告同一口径。
int midi2jianpu_set_bar_numbers(int on) {
    g_barNumbers = (on != 0);
    return g_barNumbers ? 1 : 0;
}

// 获取结果字符串长度
int midi2jianpu_result_len() {
    return (int)g_result.size();
}

// 复制结果到用户缓冲区
void midi2jianpu_copy_result(char* buf, int maxLen) {
    int len = std::min((int)g_result.size(), maxLen - 1);
    std::memcpy(buf, g_result.data(), len);
    buf[len] = '\0';
}

// 设置歌词(LRC格式文本)
void midi2jianpu_set_lyrics(const char* lyrics) {
    g_lyrics = lyrics ? lyrics : "";
}

// 网页端歌词文件解码：把用户上传的原始字节（readAsArrayBuffer 取来）直接交给
// CLI 既有的 normalizeLyricFileBytes，与 CLI 读盘入口同款解码（UTF-16 LE/BE BOM
// 转 UTF-8，其余走 textenc::normalize：严格 UTF-8 原样返回，否则 GBK / Shift-JIS
// 打分，最后按 CP1252 逐字节折叠）。这样网页与 CLI 走同一套解码逻辑，完全保真、
// 零漂移，避免浏览器 readAsText 默认 UTF-8 把非 UTF-8 字节折成 U+FFFD。
// 返回 static 缓冲的 UTF-8 字符串，网页须过 UTF8ToString；不要 free（不是 malloc 出来的）。
const char* midi2jianpu_decode_lyric_file(const uint8_t* data, int size) {
    static std::string buf;
    if (!data || size < 0) { buf.clear(); return buf.c_str(); }
    buf = m2j::normalizeLyricFileBytes(std::string((const char*)data, (size_t)size));
    return buf.c_str();
}

// 网页端：统计「当前歌词文本 + 当前 MIDI」会有多少条越界时间戳被丢弃，与 CLI
// 的「警告: 已忽略 N 行歌词」同口径（都来自解析入口的 dropped 输出，
// v1.111.0 起 LRC / JSON 都走 parseLyricsAuto，JSON 的畸形对象也计入）。
// 此前网页在渲染路径里直接丢弃 dropped，用户贴一段带离谱 [offset:] / 巨大时间戳
// 的 LRC，对应的词会静默消失（0 行可解析时还会回退到 MIDI 内置「LY」音节），
// 用户只会以为程序漏了。这里把歌词文本作为参数传入（不依赖 wasm 全局 g_lyrics），
// 网页可随时查询并在提示栏显示。返回被丢弃的时间戳条数；空歌词 / 解析失败返回 0。
int midi2jianpu_lyric_dropped(const uint8_t* midiData, int size, const char* lyrics) {
    if (!lyrics) return 0;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    size_t dropped = 0;
    parseLyricsAuto(std::string(lyrics), data, &dropped);
    return (int)dropped;
}

// 网页端的歌词体检（v1.112.0）：一次回「按哪种格式解析出几个字、忽略几行」。
// 上一版只暴露了 dropped 计数，于是「贴进去的歌词一个字都没认出来」这件事
// 网页上完全看不见——谱面静默回退成 MIDI 内置的「LY」音节，提示栏一个字都
// 不说，用户只会以为程序把歌词吃掉了（纯文本、忘了写时间戳、把 JSON 写成
// 对象而不是数组，都是这种下场）。这里同样不做第二套嗅探：格式判定交
// detectLyricFormat("")、解析交 parseLyricsAuto，与 CLI 和 set_lyrics 同一条路。
// 没有 MIDI（无法把时间戳换算成 tick）或歌词全空白时返回空串，网页不显示。
const char* midi2jianpu_lyric_info(const uint8_t* midiData, int size,
                                   const char* lyrics) {
    g_result.clear();
    if (!lyrics) return g_result.c_str();
    const std::string text(lyrics);
    bool blank = true;
    for (char c : text) {
        if (!std::isspace((unsigned char)c)) { blank = false; break; }
    }
    if (blank) return g_result.c_str();

    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter))
        return g_result.c_str();

    // 体检必须和实际渲染吃同一份开关。`--no-dynamics` 会让 ExpressionMap
    // 不再把倚音判成装饰音，歌词铺排也跟着把那个音当成普通音头：CLI 在
    // 落 useDynamics 之后才数「挂上几字」，这里过去漏了这一步，于是关闭
    // 力度时提示「挂上 7 字」而 HTML / MusicXML 实际印 8 字。
    applyRenderOptions(data);

    const bool isLrc = m2j::detectLyricFormat("", text) == m2j::LyricFormat::Lrc;
    size_t dropped = 0;
    const std::vector<m2j::LyricLine> lines = parseLyricsAuto(text, data, &dropped);

    // 数「字」而不是数行：JSON 歌词每个字自成一行，数行对用户没有意义；
    // 按 UTF-8 首字节计数，与谱面上真正印出来的字数一致。
    size_t chars = 0;
    for (const auto& line : lines)
        for (char c : line.text)
            if (((unsigned char)c & 0xC0) != 0x80) ++chars;

    g_result = "歌词：";
    if (chars == 0) {
        // 一个字都没认出来分两种：歌词格式/字段不对，或者时间戳全都越界被丢。
        // 后者若按「LRC 要 [mm:ss.xx]」提示，用户会去改本来完全正确的 LRC——
        // 与 CLI 的「警告: 已忽略 N 行歌词」对齐，这里点名「越界」。
        g_result += dropped > 0
            ? std::to_string(dropped) +
              " 行全部越界被忽略（时间戳超出谱面可解释范围）"
            : std::string("未识别出歌词行（LRC 要 [mm:ss.xx]，JSON 要 start_beat + text）");
        return g_result.c_str();
    }
    g_result += std::string("按 ") + (isLrc ? "LRC" : "JSON") + " 解析 " +
                std::to_string(chars) + " 字";
    if (dropped > 0)
        g_result += "（忽略 " + std::to_string(dropped) + " 行越界时间戳）";
    // 解析出来 ≠ 挂得上去（v1.113.0）：一行字多于该段旋律的音头数时，
    // placeLyricLine 会把多出来的字 break 掉；同戳多行均分后一行一个音头
    // 都分不到时整行 continue。只报「解析 N 字」会让用户以为谱面上一定有
    // N 个字，翻到那句却只剩半行。这里补一句「挂上几个」，与 CLI 的
    // 「N 个歌词字没有音头可挂」警告同口径（同两个纯函数、同一次铺排）。
    const int lyricTrack = m2j::pickMelodyTrack(data, &lines);
    if (lyricTrack >= 0) {
        const m2j::LyricAssignment la = m2j::resolveLyrics(data, &lines, lyricTrack);
        g_result += "，挂上 " + std::to_string(la.charsPlaced) + " 字";
        // 丢字数用「解析出的字数 - 挂上的字数」算，而不是 la.charsSkipped()：
        // 行尾若带半截多字节序列，splitUtf8 会丢掉那一截，charsTotal 就比分母
        // 少一，三个数（解析 / 挂上 / 丢掉）在对用户的那句话里就对不上账了。
        const size_t skipped =
            chars > la.charsPlaced ? chars - la.charsPlaced : 0;
        std::string missing;
        if (skipped > 0)
            missing += std::to_string(skipped) + " 字没有音头可挂";
        if (la.linesEmpty > 0) {
            if (!missing.empty()) missing += "，";
            missing += std::to_string(la.linesEmpty) + " 行整行没挂上";
        }
        if (!missing.empty()) g_result += "（" + missing + "）";
    }
    return g_result.c_str();
}

// 设置谱面标题（网页传 MIDI 文件名进来，让谱子顶上是歌名而不是程序名）
void midi2jianpu_set_title(const char* title) {
    g_title = title ? title : "";
}

// 查询文件自带的可唱音节数，供网页在用户还没贴 LRC 时提示「无需另找歌词」。
// 只是读文件，不受「忽略内置歌词」开关影响；解析失败返回 0。
int midi2jianpu_midi_lyric_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    return (int)m2j::normalizeMidiLyrics(data).size();
}

// 内置歌词的一句话体检（网页提示栏）：词在哪条轨、有没有别的轨被忽略、
// 所选轨没词时词到底在谁身上。与 CLI 那段「MIDI 内置歌词: N 个音节（轨 K）」
// 同一口径（都走 midiLyricHint / droppedLyricNote）。
//
// 这里必须按「全曲」解析：按当前选中轨过滤后，所选轨以外的歌词事件已经在
// 解析层被丢掉，那样「选了一条没有词的轨」就只剩「此文件没有内置歌词」这句
// 错话——文件明明有词，只是不在这一轨上。format 2 同理，默认解析会把
// 第 1 段之后的歌词一并丢掉，连「词在轨 1」都说不出来。
const char* midi2jianpu_midi_lyric_info(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, true, -1)) {
        g_result.clear();
        return g_result.c_str();
    }
    g_result = m2j::midiLyricHint(data, g_trackFilter);
    return g_result.c_str();
}

// 网页版「使用 MIDI 自带歌词」开关：0 = 忽略，非 0 = 自动加载（默认）。
void midi2jianpu_set_use_midi_lyrics(int on) {
    g_useMidiLyrics = (on != 0);
}

// 网页版「五声调式宫音校正」开关：0 = 回到 Krumhansl-Schmuckler，非 0 = 校正。
void midi2jianpu_set_use_pentatonic(int on) {
    g_usePentatonic = (on != 0);
}

// 五声调式信息打包成一个整数，供网页提示「检测到 G 徵调式，1=C 按宫音校正」：
//   finalPc * 100 + gongPc * 10 + mode（mode: 宫1 商2 角3 徵5 羽6）
// 0 = 没检出 / 关了开关 / 文件自带 0x59 调号（此时以调号为准，不校正）。
int midi2jianpu_pentatonic_info(const uint8_t* midiData, int size) {
    if (!g_usePentatonic) return 0;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    // 移调会把宫音整个挪走：提示里还说原调的宫音，就与谱面「1=X」对不上了。
    applyTranspose(data);
    if (data.keySig.valid) return 0;
    const m2j::PentatonicEstimate p = m2j::detectPentatonic(data);
    if (!p.detected) return 0;
    return p.finalPitchClass * 100 + p.gongPitchClass * 10 + (int)p.mode;
}

// 查询文件里的实质转调点数，供网页提示「检测到 N 处转调，唱名已按段换算」。
// 与内置歌词计数一样只读文件，不受任何渲染开关影响；解析失败返回 0。
int midi2jianpu_key_change_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    return (int)m2j::keyChangesOf(data).size();
}

// 实质变速点数。网页贴了 LRC 时提示「已按 tempo map 分段对齐」，否则
// 用户看不出「后半段歌词为什么没跑偏」是刻意做的还是碰巧。
int midi2jianpu_tempo_change_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    return m2j::tempoChangeCount(data);
}

// 其中有多少处是渐快 / 渐慢（而不是阶跃变速）。
//
// 网页要解释一件事：一段 rit. 在 MIDI 里往往是几十个连续的 set_tempo，
// 谱面上却只标一个「rit. 120→60」。不说明的话用户会以为漏了速度数据。
int midi2jianpu_tempo_ramp_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    int n = 0;
    for (const auto& m : m2j::tempoMarksOf(data))
        if (m.kind != m2j::TempoMarkKind::Step) n++;
    return n;
}

// 网页版「力度与演奏法」开关：0 = 谱面只留音符，非 0 = 写出表情（默认）。
void midi2jianpu_set_use_dynamics(int on) {
    g_useDynamics = (on != 0);
}

// 网页版「软踏板强度」下拉：0~1 的系数，0 = 关闭（不衰减、连事件都不记），
// 1 = 不衰减，介于两者之间是踩下左踏板后击弦变轻的幅度。默认 0.7 与 CLI 一致。
// 必须在解析之前设置（loadJ 每次读取这个全局再 load），网页每次转换都重设。
void midi2jianpu_set_soft_pedal_scale(double scale) {
    if (scale < 0.0) scale = 0.0;
    else if (scale > 1.0) scale = 1.0;
    g_softPedalScale = scale;
}

// 文件里实际检出的演奏表情记号总数，供网页提示「检测到 N 处表情」。
// 只是读文件，不渲染；关掉开关时返回 0，网页就不必再自己判断复选框状态。
int midi2jianpu_expression_count(const uint8_t* midiData, int size) {
    if (!g_useDynamics) return 0;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) return 0;
    return m2j::ExpressionMap(data).totalCount();
}

// 声部 / 乐器摘要，供网页提示「声部：Vocal·钢琴, Piano·钢琴」。
// 只列含音符的声部及其 GM 乐器名；与表情计数一样只读文件，不受渲染开关影响。
const char* midi2jianpu_part_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::partSummary(data);
    return buf.c_str();
}

// 文件头读法提示：截断告警 + SMPTE 时间基准 + format 2 独立序列，用「 ｜ 」拼。
// 网页版读到 SMPTE 文件时，用户看到的 division 会和常见 PPQ 完全不同，
// 不说一句「已按 SMPTE 折算」就会被当成解析错误。都没触发时返回空串。
const char* midi2jianpu_file_header_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    // 浏览器里喂进来的可能就是下载到一半的 .mid，必须先说明数据不完整。
    buf = m2j::truncationLabel(data);
    const std::string tb = m2j::timeBaseLabel(data);
    if (!tb.empty()) buf += (buf.empty() ? "" : " ｜ ") + tb;
    const std::string fl = m2j::formatLabel(data);
    if (!fl.empty()) buf += (buf.empty() ? "" : " ｜ ") + fl;
    return buf.c_str();
}

// 控制器语义提示：延音踏板补长 / 持音踏板保持 / 强制清音截尾 / CC7 力度点。这四项都会在用户
// 看不见的地方改谱面（时值变长、幽灵音消失、多了力度记号），网页必须说一句，
// 否则用户会以为「转出来的谱和我在 DAW 里看到的对不上」。
const char* midi2jianpu_controller_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::controllerLabel(data);
    return buf.c_str();
}

// 弯音范围：RPN 0x0000 可以逐通道、逐时刻设定，同一个弯音轮位置在不同通道
// 会差出整整一个八度。网页文件提示里说一句，免得用户以为转谱算错了音高。
const char* midi2jianpu_bend_range_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::bendRangeLabel(data);
    return buf.c_str();
}

// 省略状态字节：系统事件（歌词 / Marker / SysEx）之后仍有导出器不重写
// 状态字节，靠上一条通道状态才能读回后半首曲子。网页提示里说一句补回几处。
const char* midi2jianpu_running_status_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::runningStatusLabel(data);
    return buf.c_str();
}

// MIDI Port / Channel Prefix 的文件提示。返回 char*，网页必须过 UTF8ToString。
const char* midi2jianpu_port_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::portLabel(data);
    return buf.c_str();
}

// SysEx 鼓模式：GS/XG 把非通道 10 指定为节奏声部时，被删掉的鼓件音、
// 被改回普通乐器的通道、以及 GM/GS/XG Reset 恢复默认，都要在网页文件面板
// 里说清楚。返回 char*，网页必须过 UTF8ToString。
const char* midi2jianpu_sysex_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::sysExLabel(data);
    return buf.c_str();
}

// Bank Select / Program Change：XG 鼓组通道、按时间轴取乐器名、中途换音色。
// 返回 char*，网页必须过 UTF8ToString。
const char* midi2jianpu_bank_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) { buf.clear(); return buf.c_str(); }
    buf = m2j::bankLabel(data);
    return buf.c_str();
}

// 获取MIDI音符数据(JSON格式, 用于播放)
const char* midi2jianpu_get_notes_json(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) {
        g_result = "[]";
        return g_result.c_str();
    }
    // 播放音高必须与谱面一致：谱面移了 +2，试听还是原调就成了「眼睛看到的
    // 和耳朵听到的不是一首曲子」。移调放在这里、与渲染路径同一个函数，
    // 两边不会各说各话。
    applyTranspose(data);

    // 生成JSON: [{time, pitch, duration, velocity}]
    std::string json = "[";
    bool first = true;
    for (const auto& n : data.notes) {
        if (!first) json += ",";
        first = false;
        char buf[256];
        // velocity 是 note-on 的原始力度；网页播放据此给每个音不同音量，
        // 以前注释里写了这个字段、实际却没输出。
        snprintf(buf, sizeof(buf),
                 "{\"time\":%.3f,\"pitch\":%d,\"duration\":%.3f,\"velocity\":%d}",
                 n.startSeconds, n.pitch, n.durationSeconds, n.velocity);
        json += buf;
    }
    json += "]";
    g_result = json;
    return g_result.c_str();
}

// 生成MusicXML
const char* midi2jianpu_musicxml(const uint8_t* midiData, int size, int keyPitch, int isMajor) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, g_trackFilter >= 0, g_trackFilter)) {
        g_result = "<error>无法解析MIDI</error>";
        return g_result.c_str();
    }
    applyTranspose(data);
    applyRenderOptions(data);
    applyQuantize(data);

    // 与文本 / HTML 完全同一条决策链，宫音也一起走（CLI 三端同源）。
    const m2j::KeyEstimate key = chooseKey(data, keyPitch, isMajor);
    // 网页版导出 MusicXML 也要带词：歌词文本由 midi2jianpu_set_lyrics 传进来，
    // 和 HTML 谱面走同一个解析入口（LRC / JSON 自动判定），连音线/音节划分
    // 才不会两边不一致。
    std::vector<m2j::LyricLine> lyricLines;
    const std::vector<m2j::LyricLine>* lyricPtr = nullptr;
    if (!g_lyrics.empty()) {
        lyricLines = parseLyricsAuto(g_lyrics, data);
        if (!lyricLines.empty()) lyricPtr = &lyricLines;
    }
    g_result = m2j::renderMusicXml(data, key, lyricPtr);
    return g_result.c_str();
}

} // extern "C"

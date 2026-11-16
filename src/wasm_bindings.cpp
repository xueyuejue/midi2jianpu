// wasm_bindings.cpp - WASM C 风格绑定
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

#include <cctype>
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

// 网页上的两个渲染开关。清空内置歌词事件表而不是只跳过渲染：歌词事件同时
// 参与旋律轨判定，关掉就得连证据一起去掉。五声开关则直接落在 MidiData 上，
// CLI 的 --no-pentatonic 走的是同一个字段。
static void applyRenderOptions(m2j::MidiData& data) {
    if (!g_useMidiLyrics) data.midiLyrics.clear();
    data.usePentatonic = g_usePentatonic;
    data.useDynamics = g_useDynamics;
}

// 解析前的统一入口：CC67 软踏板力度衰减发生在解析阶段（applyControllerSemantics
// 直接改 notes.velocity），所以软踏板系数必须在 loadMidiFromMemory 之前落到
// MidiData 上，否则网页端的下拉就改不到谱面。所有读取文件的入口都走这里，
// 避免每个函数各自散落赋值。
static bool loadJ(const uint8_t* midiData, int size, m2j::MidiData& data,
                  bool filtered, int track) {
    data.softPedalScale = g_softPedalScale;
    return m2j::loadMidiFromMemory(midiData, size, data, filtered, track);
}

extern "C" {

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
                if (c < 0x20) {
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
    // 否则用户选了第 2 轨之后下拉框就只剩一个选项了。
    if (!loadJ(midiData, size, data, false, -1)) {
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
        if (t.drumNotes > 0) druminsts = m2j::drumInstrumentList(t);
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

    applyRenderOptions(data);
    m2j::quantizeTiming(data);

    m2j::KeyEstimate key;
    if (keyPitch >= 0) {
        key.tonicPitchClass = keyPitch;
        key.isMajor = isMajor != 0;
    } else {
        key = m2j::estimateKey(data);
    }

    g_result = m2j::formatJianpu(data, key);
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

    applyRenderOptions(data);
    m2j::quantizeTiming(data);

    m2j::KeyEstimate key;
    if (keyPitch >= 0) {
        key.tonicPitchClass = keyPitch;
        key.isMajor = isMajor != 0;
    } else {
        key = m2j::estimateKey(data);
    }

    // 解析歌词。原来这里复制了一份 LRC 解析，且用裸 std::stoi / std::stod：
    // WASM 版默认关闭 C++ 异常，用户贴一段畸形 LRC 就会 abort 崩掉整页。
    // 现在统一走 notation.cpp 里的共享实现（同样不抛异常）。
    std::vector<m2j::LyricLine> lyricLines;
    if (!g_lyrics.empty()) lyricLines = parseLyricsAuto(g_lyrics, data);

    g_result = m2j::renderHtml(data, key, g_title.empty() ? "简谱" : g_title,
                              lyricLines.empty() ? nullptr : &lyricLines);
    return g_result.c_str();
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
// 错话——文件明明有词，只是不在这一轨上。
const char* midi2jianpu_midi_lyric_info(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!loadJ(midiData, size, data, false, -1)) {
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
    applyRenderOptions(data);
    m2j::quantizeTiming(data);

    m2j::KeyEstimate key;
    if (keyPitch >= 0) {
        key.tonicPitchClass = keyPitch;
        key.isMajor = isMajor != 0;
    } else {
        key = m2j::estimateKey(data);
    }
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

// wasm_bindings.cpp - WASM C 风格绑定
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

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

// 网页上的两个渲染开关。清空内置歌词事件表而不是只跳过渲染：歌词事件同时
// 参与旋律轨判定，关掉就得连证据一起去掉。五声开关则直接落在 MidiData 上，
// CLI 的 --no-pentatonic 走的是同一个字段。
static void applyRenderOptions(m2j::MidiData& data) {
    if (!g_useMidiLyrics) data.midiLyrics.clear();
    data.usePentatonic = g_usePentatonic;
    data.useDynamics = g_useDynamics;
}

extern "C" {

// 解析MIDI并返回文本简谱
// midiData: MIDI二进制数据指针
// size: 数据大小
// keyPitch: 调性(0=C, -1=自动)
// isMajor: 是否大调
const char* midi2jianpu_parse(const uint8_t* midiData, int size, int keyPitch, int isMajor) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) {
        g_result = "错误: 无法解析MIDI文件";
        return g_result.c_str();
    }
    if (data.notes.empty()) {
        g_result = data.drumNotesSkipped > 0
                       ? "错误: 文件里只有打击乐轨，简谱没有鼓谱记法"
                       : "错误: 未提取到音符";
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

// 生成HTML谱面
const char* midi2jianpu_html(const uint8_t* midiData, int size, int keyPitch, int isMajor) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) {
        g_result = "<p>错误: 无法解析MIDI文件</p>";
        return g_result.c_str();
    }
    if (data.notes.empty()) {
        g_result = data.drumNotesSkipped > 0
                       ? "<p>错误: 文件里只有打击乐轨，简谱没有鼓谱记法</p>"
                       : "<p>错误: 未提取到音符</p>";
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
    if (!g_lyrics.empty()) lyricLines = m2j::parseLrcText(g_lyrics, data);

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

// 设置谱面标题（网页传 MIDI 文件名进来，让谱子顶上是歌名而不是程序名）
void midi2jianpu_set_title(const char* title) {
    g_title = title ? title : "";
}

// 查询文件自带的可唱音节数，供网页在用户还没贴 LRC 时提示「无需另找歌词」。
// 只是读文件，不受「忽略内置歌词」开关影响；解析失败返回 0。
int midi2jianpu_midi_lyric_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    return (int)m2j::normalizeMidiLyrics(data).size();
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
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    if (data.keySig.valid) return 0;
    const m2j::PentatonicEstimate p = m2j::detectPentatonic(data);
    if (!p.detected) return 0;
    return p.finalPitchClass * 100 + p.gongPitchClass * 10 + (int)p.mode;
}

// 查询文件里的实质转调点数，供网页提示「检测到 N 处转调，唱名已按段换算」。
// 与内置歌词计数一样只读文件，不受任何渲染开关影响；解析失败返回 0。
int midi2jianpu_key_change_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    return (int)m2j::keyChangesOf(data).size();
}

// 实质变速点数。网页贴了 LRC 时提示「已按 tempo map 分段对齐」，否则
// 用户看不出「后半段歌词为什么没跑偏」是刻意做的还是碰巧。
int midi2jianpu_tempo_change_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    return m2j::tempoChangeCount(data);
}

// 其中有多少处是渐快 / 渐慢（而不是阶跃变速）。
//
// 网页要解释一件事：一段 rit. 在 MIDI 里往往是几十个连续的 set_tempo，
// 谱面上却只标一个「rit. 120→60」。不说明的话用户会以为漏了速度数据。
int midi2jianpu_tempo_ramp_count(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    int n = 0;
    for (const auto& m : m2j::tempoMarksOf(data))
        if (m.kind != m2j::TempoMarkKind::Step) n++;
    return n;
}

// 网页版「力度与演奏法」开关：0 = 谱面只留音符，非 0 = 写出表情（默认）。
void midi2jianpu_set_use_dynamics(int on) {
    g_useDynamics = (on != 0);
}

// 文件里实际检出的演奏表情记号总数，供网页提示「检测到 N 处表情」。
// 只是读文件，不渲染；关掉开关时返回 0，网页就不必再自己判断复选框状态。
int midi2jianpu_expression_count(const uint8_t* midiData, int size) {
    if (!g_useDynamics) return 0;
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) return 0;
    return m2j::ExpressionMap(data).totalCount();
}

// 声部 / 乐器摘要，供网页提示「声部：Vocal·钢琴, Piano·钢琴」。
// 只列含音符的声部及其 GM 乐器名；与表情计数一样只读文件，不受渲染开关影响。
const char* midi2jianpu_part_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) { buf.clear(); return buf.c_str(); }
    buf = m2j::partSummary(data);
    return buf.c_str();
}

// 文件头读法提示：SMPTE 时间基准 + format 2 独立序列，两段文字用「 ｜ 」拼。
// 网页版读到 SMPTE 文件时，用户看到的 division 会和常见 PPQ 完全不同，
// 不说一句「已按 SMPTE 折算」就会被当成解析错误。都没触发时返回空串。
const char* midi2jianpu_file_header_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) { buf.clear(); return buf.c_str(); }
    buf = m2j::timeBaseLabel(data);
    const std::string fl = m2j::formatLabel(data);
    if (!fl.empty()) buf += (buf.empty() ? "" : " ｜ ") + fl;
    return buf.c_str();
}

// 控制器语义提示：踏板补长 / 强制清音截尾 / CC7 力度点。这三项都会在用户
// 看不见的地方改谱面（时值变长、幽灵音消失、多了力度记号），网页必须说一句，
// 否则用户会以为「转出来的谱和我在 DAW 里看到的对不上」。
const char* midi2jianpu_controller_info(const uint8_t* midiData, int size) {
    static std::string buf;
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) { buf.clear(); return buf.c_str(); }
    buf = m2j::controllerLabel(data);
    return buf.c_str();
}

// 获取MIDI音符数据(JSON格式, 用于播放)
const char* midi2jianpu_get_notes_json(const uint8_t* midiData, int size) {
    m2j::MidiData data;
    if (!m2j::loadMidiFromMemory(midiData, size, data)) {
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
    if (!m2j::loadMidiFromMemory(midiData, size, data)) {
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
    // 和 HTML 谱面走同一个 LRC 解析器，连音线/音节划分才不会两边不一致。
    std::vector<m2j::LyricLine> lyricLines;
    const std::vector<m2j::LyricLine>* lyricPtr = nullptr;
    if (!g_lyrics.empty()) {
        lyricLines = m2j::parseLrcText(g_lyrics, data);
        if (!lyricLines.empty()) lyricPtr = &lyricLines;
    }
    g_result = m2j::renderMusicXml(data, key, lyricPtr);
    return g_result.c_str();
}

} // extern "C"

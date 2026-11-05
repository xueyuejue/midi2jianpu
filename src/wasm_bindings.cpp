// wasm_bindings.cpp - WASM C 风格绑定
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

#include <cstring>
#include <string>

// 简单字符串缓冲区
static std::string g_result;
static std::string g_lyrics;  // 歌词文本(LRC格式)

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
        g_result = "错误: 未提取到音符";
        return g_result.c_str();
    }

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
        g_result = "<p>错误: 未提取到音符</p>";
        return g_result.c_str();
    }

    m2j::KeyEstimate key;
    if (keyPitch >= 0) {
        key.tonicPitchClass = keyPitch;
        key.isMajor = isMajor != 0;
    } else {
        key = m2j::estimateKey(data);
    }

    // 解析歌词
    std::vector<m2j::LyricLine> lyricLines;
    if (!g_lyrics.empty()) {
        int bpm = (int)(60000000.0 / data.tempo);
        // 简单LRC解析: [mm:ss.xx]歌词
        std::string lrc = g_lyrics;
        size_t pos = 0;
        while (pos < lrc.size()) {
            size_t lineStart = lrc.find('[', pos);
            if (lineStart == std::string::npos) break;
            size_t close = lrc.find(']', lineStart);
            if (close == std::string::npos) break;
            std::string ts = lrc.substr(lineStart + 1, close - lineStart - 1);
            // 解析mm:ss.xx
            size_t colon = ts.find(':');
            if (colon != std::string::npos && isdigit(ts[0])) {
                int mm = std::stoi(ts.substr(0, colon));
                double ss = std::stod(ts.substr(colon + 1));
                double totalSec = mm * 60.0 + ss;
                double secPerBeat = 60.0 / bpm;
                long startTick = (long)(totalSec / secPerBeat * data.division);
                // 找歌词文本
                size_t nextLine = lrc.find('\n', close);
                if (nextLine == std::string::npos) nextLine = lrc.size();
                std::string text = lrc.substr(close + 1, nextLine - close - 1);
                // trim
                while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
                if (!text.empty()) lyricLines.push_back({startTick, text});
            }
            pos = close + 1;
        }
    }

    g_result = m2j::renderHtml(data, key, "midi2jianpu", lyricLines.empty() ? nullptr : &lyricLines);
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
        snprintf(buf, sizeof(buf), "{\"time\":%.3f,\"pitch\":%d,\"duration\":%.3f}",
                 n.startSeconds, n.pitch, n.durationSeconds);
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
    m2j::KeyEstimate key;
    if (keyPitch >= 0) {
        key.tonicPitchClass = keyPitch;
        key.isMajor = isMajor != 0;
    } else {
        key = m2j::estimateKey(data);
    }
    g_result = m2j::renderMusicXml(data, key);
    return g_result.c_str();
}

} // extern "C"

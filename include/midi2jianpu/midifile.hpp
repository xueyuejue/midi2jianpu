#pragma once
// midifile.hpp - 标准 MIDI 文件(SMF)解析
// 手写二进制解析，不依赖第三方库。提取音符事件供后续简谱转换。
#include <cstdint>
#include <string>
#include <vector>

namespace m2j {

// 单个音符事件
struct NoteEvent {
    double startSeconds;  // 起始时间（秒）
    double durationSeconds; // 持续时间（秒）
    int pitch;            // MIDI 音号 0-127 (60=C4)
    int channel;
    int track;
};

// 解析结果
struct MidiData {
    int division = 480;      // ticks per quarter note
    std::vector<NoteEvent> notes;
    double tempo = 500000.0; // microseconds per quarter note
};

// 从文件路径解析 MIDI，成功返回 true
bool loadMidiFile(const std::string& path, MidiData& out);

} // namespace m2j

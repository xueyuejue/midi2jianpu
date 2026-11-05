#pragma once
// midifile.hpp - 标准 MIDI 文件(SMF)解析
// 手写二进制解析，不依赖第三方库。提取音符事件、拍号、速度。
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace m2j {

// 单个音符事件
struct NoteEvent {
    double startSeconds;
    double durationSeconds;
    long startTicks = 0;      // 起始 tick（用于划分小节）
    long durationTicks = 0;    // 持续 tick
    int pitch;
    int channel;
    int track;
};

// 拍号（从 MIDI 0x58 meta 事件解析）
struct TimeSignature {
    int numerator = 4;     // 分子
    int denomPow = 2;      // 分母 = 2^denomPow（2=四分音符为一拍, 3=八分, 1=二分）
    bool valid = false;

    int beatsPerBar() const { return numerator; }
    double barLengthTicks(long division) const {
        double oneBeat = division * 4.0 / (1 << denomPow);
        return oneBeat * numerator;
    }
    std::string toString() const {
        return std::to_string(numerator) + "/" +
               std::to_string(1 << denomPow);
    }
};

// 调号（从 MIDI 0x59 meta 事件解析）
struct KeySignature {
    int sharpsFlats = 0;   // 升号数(正)或降号数(负), -7..+7
    bool isMinor = false;
    bool valid = false;
};

// 力度变化点（从 CC 0x0B Expression 解析）
struct DynPoint {
    long tick;
    int value;
};

// 速度变化点（从 0x51 set_tempo 解析）
struct TempoPoint {
    long tick;
    double bpm;  // 每分钟拍数
};

// 解析结果
struct MidiData {
    int division = 480;
    std::vector<NoteEvent> notes;
    double tempo = 500000.0;
    TimeSignature timeSig;
    KeySignature keySig;
    std::vector<DynPoint> dynamics;
    std::vector<TempoPoint> tempoChanges;
    std::map<int, std::string> trackNames;  // 轨道名称(0x03 meta)
};

bool loadMidiFile(const std::string& path, MidiData& out);

} // namespace m2j

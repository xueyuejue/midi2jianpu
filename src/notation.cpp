// notation.cpp - 调性识别与简谱转换实现
#include "midi2jianpu/notation.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace m2j {

static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

// 自然大调/小调各级相对主音的半音距离（7个音级）
static const std::array<int, 7> MAJOR = {0, 2, 4, 5, 7, 9, 11};
static const std::array<int, 7> MINOR = {0, 2, 3, 5, 7, 8, 10};

std::string midiNoteName(int pitch) {
    std::ostringstream os;
    os << NOTE_NAMES[pitch % 12] << (pitch / 12 - 1);
    return os.str();
}

KeyEstimate estimateKey(const std::vector<NoteEvent>& notes) {
    std::map<int, double> weight;
    for (const auto& n : notes) {
        weight[n.pitch % 12] += n.durationSeconds;
    }
    if (weight.empty()) return KeyEstimate{};

    int pc = 0;
    double best = -1;
    for (auto& [k, v] : weight) {
        if (v > best) { best = v; pc = k; }
    }

    double maj3 = weight.count((pc + 4) % 12) ? weight[(pc + 4) % 12] : 0;
    double min3 = weight.count((pc + 3) % 12) ? weight[(pc + 3) % 12] : 0;

    KeyEstimate r;
    r.tonicPitchClass = pc;
    r.isMajor = maj3 >= min3;
    return r;
}

std::string pitchToJianpu(int pitch, const KeyEstimate& key) {
    const auto& scale = key.isMajor ? MAJOR : MINOR;
    int semi = ((pitch - key.tonicPitchClass) % 12 + 12) % 12;
    int octave = (pitch - 60) / 12;  // C4=60 为简谱中音

    int degree = -1;
    std::string accidental;
    for (int i = 0; i < 7; i++) {
        if (scale[i] == semi) { degree = i; break; }
    }
    if (degree < 0) {
        for (int i = 6; i >= 0; i--) {
            if (scale[i] < semi) {
                degree = i;
                accidental = (semi == scale[i] + 1) ? "#" : "b";
                break;
            }
        }
        if (degree < 0) { degree = 6; accidental = (semi == 11) ? "#" : ""; }
    }

    std::string s = accidental + std::to_string(degree + 1);
    if (octave > 0) s.append(octave, '\'');
    else if (octave < 0) s.append(-octave, ',');
    return s;
}

std::string durationSuffix(long durationTicks, int division) {
    // 以四分音符为一拍，换算音符类型
    double beats = (double)durationTicks / division;
    // 近似到标准时值（全/二分/四分/八分/十六分）
    if (beats >= 3.5) return "---";       // 全音符
    if (beats >= 1.5) return "-";         // 二分音符
    if (beats >= 0.75) return "";         // 四分音符（基准）
    if (beats >= 0.375) return ".";      // 八分音符（下划线，文本用.）
    return "..";                          // 十六分
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream os;
    os << "1 = " << NOTE_NAMES[key.tonicPitchClass]
       << (key.isMajor ? " 大调" : " 小调")
       << "    " << data.timeSig.toString() << "\n"
       << "----------------------------------------\n";

    double barLen = data.timeSig.barLengthTicks(data.division);
    int currentBar = 0;

    for (const auto& n : data.notes) {
        // 是否进入新小节
        int bar = (int)(n.startTicks / barLen);
        if (bar != currentBar) {
            os << " |\n";
            currentBar = bar;
        }
        os << pitchToJianpu(n.pitch, key)
           << durationSuffix(n.durationTicks, data.division) << " ";
    }
    os << " |\n";
    return os.str();
}

} // namespace m2j

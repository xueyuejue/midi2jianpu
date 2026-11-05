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

// 自然大调/小调各级相对主音的半音距离
static const std::array<int, 7> MAJOR = {0, 2, 4, 5, 7, 9, 11};
static const std::array<int, 7> MINOR = {0, 3, 5, 7, 8, 10};

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

std::string formatJianpu(const std::vector<NoteEvent>& notes,
                         const KeyEstimate& key) {
    std::ostringstream os;
    os << "主音(1=): " << NOTE_NAMES[key.tonicPitchClass]
       << (key.isMajor ? " 大调" : " 小调") << "\n"
       << "----------------------------------------\n";
    for (const auto& n : notes) {
        os << pitchToJianpu(n.pitch, key)
           << "  (" << midiNoteName(n.pitch)
           << ", " << std::fixed << std::setprecision(2)
           << n.durationSeconds << "s)\n";
    }
    return os.str();
}

} // namespace m2j

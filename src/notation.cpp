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

KeyEstimate estimateKey(const MidiData& data) {
    // 1) 优先用 MIDI 内置调号事件
    if (data.keySig.valid) {
        int sf = data.keySig.sharpsFlats;
        // 大调主音: C=0, 每升一个调号主音上移纯五度(+7半音)
        int majorTonic = ((7 * sf) % 12 + 12) % 12;
        KeyEstimate r;
        r.isMajor = !data.keySig.isMinor;
        r.tonicPitchClass = r.isMajor ? majorTonic
                                       : (majorTonic - 3 + 12) % 12;
        return r;
    }

    // 2) 猜测：强拍加权 + 结尾音加权
    std::map<int, double> weight;
    double barLen = data.timeSig.barLengthTicks(data.division);
    size_t n = data.notes.size();

    for (size_t i = 0; i < n; i++) {
        const auto& note = data.notes[i];
        double w = note.durationSeconds;
        // 强拍加权：落在小节第1拍或第3拍(4/4)位置
        long posInBar = (long)(note.startTicks) % (long)barLen;
        if (posInBar < data.division / 4 ||
            posInBar >= barLen / 2.0 - data.division / 4) {
            w *= 1.5;
        }
        // 结尾音加权：最后4个音符更可能是主音
        if (i + 4 >= n) w *= 2.0;
        weight[note.pitch % 12] += w;
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
    double beats = (double)durationTicks / division;
    // 标准时值（四分音符=1）：找最近的
    struct Dur { double beats; std::string suffix; };
    Dur table[] = {
        {0.25, ".."},   // 十六分
        {0.5,  "."},    // 八分
        {0.75, "."},    // 附点八分（文本简化）
        {1.0,  ""},     // 四分
        {1.5,  ".'"},   // 附点四分
        {2.0,  "-"},    // 二分
        {3.0,  "-.'"},  // 附点二分
        {4.0,  "---"},  // 全音符
    };
    std::string best = "";
    double bestDist = 1e9;
    for (auto& d : table) {
        double dist = std::abs(beats - d.beats);
        if (dist < bestDist) { bestDist = dist; best = d.suffix; }
    }
    return best;
}

// 输出单个轨的简谱，返回该轨输出文本
static std::string formatTrack(
    const std::vector<NoteEvent>& notes,
    const KeyEstimate& key, const MidiData& data, bool isMain) {

    std::ostringstream os;
    double barLen = data.timeSig.barLengthTicks(data.division);
    int currentBar = -1;
    long prevEnd = 0;  // 上一个音符结束tick

    for (const auto& n : notes) {
        int bar = (int)(n.startTicks / barLen);
        if (bar != currentBar) {
            if (currentBar >= 0) os << " |\n";
            currentBar = bar;
        }
        // 休止符：与上一个音符之间有超过半拍的空隙
        long gap = n.startTicks - prevEnd;
        if (gap >= data.division / 2 && prevEnd > 0) {
            os << "0" << durationSuffix(gap, data.division) << " ";
        }
        os << pitchToJianpu(n.pitch, key)
           << durationSuffix(n.durationTicks, data.division) << " ";
        prevEnd = n.startTicks + n.durationTicks;
    }
    os << " |\n";
    return os.str();
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream os;
    os << "1 = " << NOTE_NAMES[key.tonicPitchClass]
       << (key.isMajor ? " 大调" : " 小调")
       << "    " << data.timeSig.toString() << "\n"
       << "----------------------------------------\n";

    // 按 track 分组
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (const auto& n : data.notes) byTrack[n.track].push_back(n);

    // 找出音符最多的轨作为主旋律
    int mainTrack = -1;
    size_t maxNotes = 0;
    for (auto& [t, ns] : byTrack) {
        if (ns.size() > maxNotes) { maxNotes = ns.size(); mainTrack = t; }
    }

    for (auto& [t, ns] : byTrack) {
        bool isMain = (t == mainTrack);
        os << (isMain ? "[旋律轨] " : "[伴奏轨" + std::to_string(t) + "] ")
           << "(" << ns.size() << " 音符)\n";
        os << formatTrack(ns, key, data, isMain);
    }
    return os.str();
}

} // namespace m2j

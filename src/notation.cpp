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

    // 2) 猜测：只用旋律轨（音符最多的轨），避免伴奏和弦干扰
    std::map<int, size_t> trackCount;
    for (const auto& note : data.notes) trackCount[note.track]++;
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, c] : trackCount) if (c > maxN) { maxN = c; mainTrack = t; }

    std::map<int, double> weight;
    double barLen = data.timeSig.barLengthTicks(data.division);
    size_t n = 0;
    for (const auto& note : data.notes) if (note.track == mainTrack) n++;

    size_t idx = 0;
    for (const auto& note : data.notes) {
        if (note.track != mainTrack) continue;
        double w = note.durationSeconds;
        long posInBar = (long)(note.startTicks) % (long)barLen;
        if (posInBar < data.division / 4 ||
            posInBar >= barLen / 2.0 - data.division / 4) {
            w *= 1.5;
        }
        if (idx + 6 >= n) w *= 3.0;  // 结尾6个音符权重x3
        weight[note.pitch % 12] += w;
        idx++;
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

// 查询某 tick 位置的当前力度（从 dynamics 曲线），映射为简谱动态标记
static std::string dynAt(long tick, const MidiData& data) {
    if (data.dynamics.empty()) return "";
    int val = 80;  // 默认 mf
    for (const auto& dp : data.dynamics) {
        if (dp.tick <= tick) val = dp.value;
        else break;
    }
    if (val < 20) return "pp";
    if (val < 40) return "p";
    if (val < 60) return "mp";
    if (val < 80) return "mf";
    if (val < 100) return "f";
    return "ff";
}

// 检测三连音: 连续3个等长音符(误差10%), 每个约0.5-0.8拍
std::vector<bool> detectTriplets(const std::vector<NoteEvent>& notes, int division) {
    std::vector<bool> isTriplet(notes.size(), false);
    double beat = division;  // 1拍 = division ticks
    for (size_t i = 0; i + 2 < notes.size(); i++) {
        double d1 = notes[i].durationTicks / beat;
        double d2 = notes[i+1].durationTicks / beat;
        double d3 = notes[i+2].durationTicks / beat;
        // 三个时值相近
        double avg = (d1 + d2 + d3) / 3;
        if (avg < 0.25 || avg > 0.9) continue;
        if (std::abs(d1 - d2) / avg > 0.15) continue;
        if (std::abs(d2 - d3) / avg > 0.15) continue;
        // 三个连续无间隙
        if (notes[i+1].startTicks - notes[i].startTicks > notes[i].durationTicks * 1.1) continue;
        if (notes[i+2].startTicks - notes[i+1].startTicks > notes[i+1].durationTicks * 1.1) continue;
        isTriplet[i] = isTriplet[i+1] = isTriplet[i+2] = true;
    }
    return isTriplet;
}

// 输出单个轨的简谱，返回该轨输出文本
static std::string formatTrack(
    const std::vector<NoteEvent>& notes,
    const KeyEstimate& key, const MidiData& data, bool isMain) {

    std::ostringstream os;
    double barLen = data.timeSig.barLengthTicks(data.division);
    int currentBar = -1;
    long prevEnd = 0;
    int lastPitch = -1;
    std::string lastDyn;
    auto triplets = detectTriplets(notes, data.division);
    bool inTriplet = false;

    for (size_t idx = 0; idx < notes.size(); idx++) {
        const auto& n = notes[idx];
        int bar = (int)(n.startTicks / barLen);
        if (bar != currentBar) {
            if (currentBar >= 0) os << " |\n";
            currentBar = bar;
            lastPitch = -1;
            std::string d = dynAt(n.startTicks, data);
            if (!d.empty() && d != lastDyn) {
                os << "[" << d << "] ";
                lastDyn = d;
            }
        }
        // 三连音边界
        if (triplets[idx] && !inTriplet) { os << "3("; inTriplet = true; }
        if (!triplets[idx] && inTriplet) { os << ") "; inTriplet = false; }

        long gap = n.startTicks - prevEnd;
        bool hasRest = (gap >= data.division / 2 && prevEnd > 0);
        if (hasRest) { os << "0 "; lastPitch = -1; }
        // 三连音组内不做同音合并
        if (!triplets[idx] && n.pitch == lastPitch && !hasRest && gap < data.division / 4) {
            os << "- ";
        } else {
            os << pitchToJianpu(n.pitch, key) << " ";
            lastPitch = n.pitch;
        }
        prevEnd = n.startTicks + n.durationTicks;
    }
    if (inTriplet) os << ")";
    os << " |\n";
    return os.str();
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream os;
    os << "1 = " << NOTE_NAMES[key.tonicPitchClass]
       << (key.isMajor ? " 大调" : " 小调")
       << "    " << data.timeSig.toString();
    if (!data.tempoChanges.empty()) {
        os << "    ♩=" << (int)data.tempoChanges[0].bpm;
    }
    os << "\n----------------------------------------\n";

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
        std::string label;
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) {
            label = it->second;
        } else {
            label = isMain ? "旋律轨" : "伴奏轨" + std::to_string(t);
        }
        os << "[" << label << "] (" << ns.size() << " 音符)\n";
        os << formatTrack(ns, key, data, isMain);
    }
    return os.str();
}

} // namespace m2j

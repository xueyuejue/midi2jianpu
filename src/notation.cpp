// notation.cpp - 调性识别与简谱转换实现
#include "midi2jianpu/notation.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
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

    // 2) Krumhansl-Schmuckler 调性检测算法
    // 只用旋律轨（音符最多的轨），避免伴奏和弦干扰
    std::map<int, size_t> trackCount;
    for (const auto& note : data.notes) trackCount[note.track]++;
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, c] : trackCount) if (c > maxN) { maxN = c; mainTrack = t; }

    // Krumhansl-Schmuckler 特征轮廓 (C大调/C小调基准)
    static const double MAJOR_PROFILE[12] = {
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
        2.52, 5.19, 2.39, 3.66, 2.29, 2.88
    };
    static const double MINOR_PROFILE[12] = {
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
        2.54, 4.75, 3.98, 2.69, 3.34, 3.17
    };

    // 统计每个音级的权重
    std::array<double, 12> pitchWeights{};
    double totalWeight = 0;
    for (const auto& note : data.notes) {
        if (note.track != mainTrack) continue;
        double w = note.durationSeconds;
        pitchWeights[note.pitch % 12] += w;
        totalWeight += w;
    }
    if (totalWeight <= 0) return KeyEstimate{};

    // 归一化
    for (auto& w : pitchWeights) w /= totalWeight;

    // 计算与每个调性的相关性
    double bestCorr = -1e9;
    int bestTonic = 0;
    bool bestIsMajor = true;

    for (int tonic = 0; tonic < 12; tonic++) {
        // 计算与大调轮廓的相关性
        double corrMajor = 0;
        double corrMinor = 0;
        double meanPitch = 0, meanMajor = 0, meanMinor = 0;
        for (int i = 0; i < 12; i++) {
            meanPitch += pitchWeights[i];
            meanMajor += MAJOR_PROFILE[i];
            meanMinor += MINOR_PROFILE[i];
        }
        meanPitch /= 12; meanMajor /= 12; meanMinor /= 12;

        double covMajor = 0, covMinor = 0;
        double varPitch = 0, varMajor = 0, varMinor = 0;
        for (int i = 0; i < 12; i++) {
            int shifted = ((i - tonic) + 12) % 12;
            double dPitch = pitchWeights[i] - meanPitch;
            double dMajor = MAJOR_PROFILE[shifted] - meanMajor;
            double dMinor = MINOR_PROFILE[shifted] - meanMinor;
            covMajor += dPitch * dMajor;
            covMinor += dPitch * dMinor;
            varPitch += dPitch * dPitch;
            varMajor += dMajor * dMajor;
            varMinor += dMinor * dMinor;
        }
        corrMajor = covMajor / (sqrt(varPitch * varMajor) + 1e-9);
        corrMinor = covMinor / (sqrt(varPitch * varMinor) + 1e-9);

        if (corrMajor > bestCorr) {
            bestCorr = corrMajor;
            bestTonic = tonic;
            bestIsMajor = true;
        }
        if (corrMinor > bestCorr) {
            bestCorr = corrMinor;
            bestTonic = tonic;
            bestIsMajor = false;
        }
    }

    KeyEstimate r;
    r.tonicPitchClass = bestTonic;
    r.isMajor = bestIsMajor;
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
    // 标准时值（四分音符=1）：精确匹配附点节奏
    // 按从长到短排序，优先匹配长时值
    struct Dur { double beats; std::string suffix; double weight; };
    Dur table[] = {
        {4.0,    "---",   1.0},   // 全音符
        {3.0,    "-.-",   0.5},   // 附点二分
        {2.0,    "-",     1.0},   // 二分音符
        {1.5,    ".-",    0.8},   // 附点四分 + 八分
        {1.0,    "",      1.0},   // 四分音符
        {0.75,   ".",     0.8},   // 附点八分
        {0.5,    ".",     1.0},   // 八分音符
        {0.375,  "..",    0.8},   // 附点十六分
        {0.25,   "..",    1.0},   // 十六分
        {0.125,  "...",   0.5},   // 三十二分
    };
    std::string best = "";
    double bestDist = 1e9;
    for (auto& d : table) {
        double dist = std::abs(beats - d.beats) * d.weight;
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

    // 弯音修正：对每个音符，计算其持续期间的平均弯音偏移
    auto bendOffset = [&](long start, long end) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * data.bendRange;  // ±2半音范围
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    // 按 track 分组（应用弯音修正）
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks);
        if (std::abs(off) >= 0.5) {
            n.pitch += (int)std::round(off);
        }
        byTrack[n.track].push_back(n);
    }

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

// 简单JSON歌词解析: 找"start_beat"和"text"字段
std::vector<LyricSyllable> parseLyrics(const std::string& jsonPath, int division) {
    std::vector<LyricSyllable> result;
    std::ifstream f(jsonPath);
    if (!f.is_open()) return result;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // 简单解析: 找每个{"start_beat":N,"text":"..."}
    size_t pos = 0;
    while ((pos = content.find("\"start_beat\"", pos)) != std::string::npos) {
        // 提取数字
        size_t colon = content.find(":", pos);
        size_t end = content.find_first_of(",}", colon);
        int beat = std::stoi(content.substr(colon + 1, end - colon - 1));

        // 找对应的text
        size_t textPos = content.find("\"text\"", end);
        if (textPos == std::string::npos) break;
        size_t q1 = content.find("\"", textPos + 6);
        size_t q2 = content.find("\"", q1 + 1);
        std::string line = content.substr(q1 + 1, q2 - q1 - 1);

        // 按字拆分, 每个字分配一个tick (均匀分布在该行的beat范围内)
        long startTick = (long)beat * division;
        // 找下一行的start_beat来确定该行长度
        size_t nextStart = content.find("\"start_beat\"", q2);
        long lineLenTicks = division * 4;  // 默认4拍
        if (nextStart != std::string::npos) {
            size_t nc = content.find(":", nextStart);
            size_t ne = content.find_first_of(",}", nc);
            int nextBeat = std::stoi(content.substr(nc + 1, ne - nc - 1));
            lineLenTicks = (long)(nextBeat - beat) * division;
        }
        long perChar = lineLenTicks / std::max((int)line.size(), 1);
        for (size_t i = 0; i < line.size(); i++) {
            // UTF-8中文字符: 每个字3字节
            size_t charLen = 1;
            unsigned char c = line[i];
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            if (i + charLen > line.size()) break;
            std::string ch = line.substr(i, charLen);
            result.push_back({startTick + (long)i * perChar, ch});
            i += charLen - 1;
        }
        pos = q2;
    }
    return result;
}

// 解析LRC歌词文件: [mm:ss.xx]歌词
std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, int division, int bpm) {
    std::vector<LyricSyllable> result;
    std::ifstream f(lrcPath);
    if (!f.is_open()) return result;
    std::string line;
    double secPerBeat = 60.0 / bpm;  // 每拍秒数
    while (std::getline(f, line)) {
        // 找 [mm:ss.xx]
        if (line.size() < 10 || line[0] != '[') continue;
        size_t close = line.find(']');
        if (close == std::string::npos) continue;
        std::string ts = line.substr(1, close - 1);
        // 跳过元数据 [ti:] [ar:] 等
        if (ts.find(':') == std::string::npos || !isdigit(ts[0])) continue;
        // 解析 mm:ss.xx
        size_t colon = ts.find(':');
        if (colon == std::string::npos) continue;
        int mm = std::stoi(ts.substr(0, colon));
        double ss = std::stod(ts.substr(colon + 1));
        double totalSec = mm * 60.0 + ss;
        long startTick = (long)(totalSec / secPerBeat * division);

        // 歌词文本
        std::string text = line.substr(close + 1);
        if (text.empty()) continue;

        // 按字拆分, 假设一行约4拍
        long lineLenTicks = (long)(4.0 / secPerBeat * division);
        int charCount = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = text[i];
            int charLen = 1;
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            if (i + charLen > text.size()) break;
            charCount++;
            i += charLen;
        }
        if (charCount == 0) continue;
        long perChar = lineLenTicks / charCount;
        int idx = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = text[i];
            int charLen = 1;
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            if (i + charLen > text.size()) break;
            std::string ch = text.substr(i, charLen);
            result.push_back({startTick + (long)idx * perChar, ch});
            i += charLen;
            idx++;
        }
    }
    return result;
}

// 解析LRC为行列表
std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, int division, int bpm) {
    std::vector<LyricLine> lines;
    std::ifstream f(lrcPath);
    if (!f.is_open()) return lines;
    std::string line;
    double secPerBeat = 60.0 / bpm;
    while (std::getline(f, line)) {
        if (line.size() < 10 || line[0] != '[') continue;
        size_t close = line.find(']');
        if (close == std::string::npos) continue;
        std::string ts = line.substr(1, close - 1);
        if (ts.find(':') == std::string::npos || !isdigit(ts[0])) continue;
        size_t colon = ts.find(':');
        if (colon == std::string::npos) continue;
        int mm = std::stoi(ts.substr(0, colon));
        double ss = std::stod(ts.substr(colon + 1));
        double totalSec = mm * 60.0 + ss;
        long startTick = (long)(totalSec / secPerBeat * division);
        std::string text = line.substr(close + 1);
        if (!text.empty()) lines.push_back({startTick, text});
    }
    return lines;
}

} // namespace m2j

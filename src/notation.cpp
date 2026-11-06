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

// 大调主音音级 -> 调号升降数（取绝对值最小的等音写法）
// C=0, C#/Db=-5, D=2, D#/Eb=-3, E=4, F=-1, F#/Gb=6, G=1, G#/Ab=-4, A=3, A#/Bb=-2, B=5
static const int MAJOR_FIFTHS[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};

int keyFifths(const KeyEstimate& key) {
    int pc = key.tonicPitchClass;
    if (key.isMajor) return MAJOR_FIFTHS[pc];
    // 关系小调的主音比关系大调高小三度
    return MAJOR_FIFTHS[(pc + 3) % 12];
}

bool prefersFlats(const KeyEstimate& key) {
    return keyFifths(key) < 0;
}

std::string keyName(const KeyEstimate& key) {
    static const char* SHARP_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* FLAT_NAMES[12]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    return prefersFlats(key) ? FLAT_NAMES[key.tonicPitchClass]
                             : SHARP_NAMES[key.tonicPitchClass];
}

std::string pitchToJianpu(int pitch, const KeyEstimate& key) {
    const auto& scale = key.isMajor ? MAJOR : MINOR;
    int semi = ((pitch - key.tonicPitchClass) % 12 + 12) % 12;

    // 八度以"主音所在的中音区"为基准（旧实现以 C4=60 为基准，
    // 导致 G/D/A 等主音高于 C 的调性整体高一个八度点）
    int tonicRef = 60 + key.tonicPitchClass;
    int diff = pitch - tonicRef;
    int octave = (diff >= 0) ? (diff / 12) : -(((-diff) + 11) / 12);

    int degree = -1;
    std::string accidental;
    for (int i = 0; i < 7; i++) {
        if (scale[i] == semi) { degree = i; break; }
    }
    if (degree < 0) {
        // 非音阶音：找出上下相邻的两个音级，按调性习惯决定用 # 还是 b
        int lower = -1, upper = -1;
        for (int i = 0; i < 7; i++) if (scale[i] < semi) lower = i;
        for (int i = 0; i < 7; i++) if (scale[i] > semi) { upper = i; break; }

        bool useFlat = false;
        // 降号调优先用"降某音级"的写法；但主音上方半音习惯写成 #1 而不是 b2
        if (upper >= 0 && scale[upper] - semi == 1 && prefersFlats(key) && lower != 0) useFlat = true;
        if (upper >= 0 && scale[upper] - semi == 1 && lower < 0) useFlat = true;

        if (useFlat) { degree = upper; accidental = "b"; }
        else if (lower >= 0) { degree = lower; accidental = (semi == scale[lower] + 1) ? "#" : "b"; }
        else { degree = upper >= 0 ? upper : 0; accidental = "b"; }
    }

    std::string s = accidental + std::to_string(degree + 1);
    if (octave > 0) s.append(octave, '\'');
    else if (octave < 0) s.append(-octave, ',');
    return s;
}

std::string durationSuffix(long durationTicks, int division) {
    if (division <= 0) return "";
    double beats = (double)durationTicks / division;

    // 文本简谱记法（与 HTML 版的视觉记法一一对应）：
    //   延长线 "-"  每多一拍加一条
    //   下划线 "_" 表示八分，"=" 表示十六分，"%%" 表示三十二分
    //   附点在数字右侧用 "." 表示
    struct Dur { double beats; const char* suffix; };
    static const Dur table[] = {
        {0.125, "%%"},   // 三十二分
        {0.25,  "="},    // 十六分
        {0.375, "=."},   // 附点十六分
        {0.5,   "_"},    // 八分
        {0.75,  "_."},   // 附点八分
        {1.0,   ""},     // 四分
        {1.5,   "."},    // 附点四分
        {2.0,   "-"},    // 二分
        {3.0,   "--"},   // 附点二分
        {4.0,   "---"},  // 全音符
    };

    if (beats > 4.5) {  // 超长音符退化为延长线计数
        int n = (int)std::lround(beats) - 1;
        return std::string(n > 0 ? n : 0, '-');
    }

    const char* best = "";
    double bestDist = 1e9;
    for (const auto& d : table) {
        double dist = std::abs(beats - d.beats);
        if (dist < bestDist) { bestDist = dist; best = d.suffix; }
    }
    return best;
}

// 查询某 tick 位置、某通道的当前力度（从 dynamics 曲线），映射为简谱动态标记
// 旧实现不看通道，会把伴奏的力度标到旋律上
static std::string dynAt(long tick, int channel, const MidiData& data) {
    int val = -1;
    for (const auto& dp : data.dynamics) {
        if (dp.tick > tick) break;
        if (dp.channel == channel) val = dp.value;
    }
    if (val < 0) return "";
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

// 输出一个音符记号：延长线拆成独立 token，读起来是 "1 - -" 而不是 "1--"
static void emitNoteToken(std::ostringstream& os, const std::string& head,
                          const std::string& dur) {
    os << head;
    bool onlyDashes = !dur.empty() &&
                      dur.find_first_not_of('-') == std::string::npos;
    if (onlyDashes) {
        for (size_t i = 0; i < dur.size(); i++) os << " -";
    } else {
        os << dur;
    }
    os << " ";
}

// 输出单个轨的简谱，返回该轨输出文本
static std::string formatTrack(
    const std::vector<NoteEvent>& notes,
    const KeyEstimate& key, const MidiData& data,
    const std::map<long, std::string>* chords = nullptr) {

    std::ostringstream os;
    double barLen = data.timeSig.barLengthTicks(data.division);
    int currentBar = -1;
    bool started = false;
    long prevEnd = 0;
    std::string lastDyn;

    // 先合并"同音高、首尾相接"的连续音符（1 1 1 → 1 - -）。
    // 三连音组内的音符不参与合并，否则会把三连音拆成普通长音。
    //
    // 还要排除多声部：C 和弦的 5 与紧接的 G 和弦的 5 音高相同、时间也相接，
    // 合并后 G 和弦会凭空少一个音。只在两处都是单声部时才允许合并。
    auto trip0 = detectTriplets(notes, data.division);
    std::map<long, int> onsetCount;
    for (const auto& n : notes) onsetCount[n.startTicks]++;
    auto isSingle = [&](long tick) {
        auto cit = onsetCount.find(tick);
        return cit == onsetCount.end() || cit->second <= 1;
    };

    std::vector<NoteEvent> merged;
    merged.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); i++) {
        if (i > 0 && !trip0[i] && !trip0[i - 1] && !merged.empty() &&
            notes[i].pitch == merged.back().pitch &&
            notes[i].startTicks == merged.back().startTicks + merged.back().durationTicks &&
            isSingle(notes[i].startTicks) && isSingle(merged.back().startTicks)) {
            merged.back().durationTicks += notes[i].durationTicks;
            merged.back().durationSeconds += notes[i].durationSeconds;
            continue;
        }
        merged.push_back(notes[i]);
    }
    auto triplets = detectTriplets(merged, data.division);
    bool inTriplet = false;

    for (size_t idx = 0; idx < merged.size(); idx++) {
        const auto& n = merged[idx];
        int bar = (int)(n.startTicks / barLen);
        if (bar != currentBar) {
            if (currentBar >= 0) os << " |\n";
            currentBar = bar;
            // 和弦标记只标在旋律轨上，避免伴奏轨重复标注
            if (chords) {
                long barStart = bar * (long)barLen;
                auto it = chords->find(barStart);
                if (it != chords->end()) {
                    os << "[" << it->second << "] ";
                }
            }
            std::string d = dynAt(n.startTicks, n.channel, data);
            if (!d.empty() && d != lastDyn) {
                os << "[" << d << "] ";
                lastDyn = d;
            }
        }
        // 三连音边界
        if (triplets[idx] && !inTriplet) { os << "3("; inTriplet = true; }
        if (!triplets[idx] && inTriplet) { os << ") "; inTriplet = false; }

        long gap = n.startTicks - prevEnd;
        bool hasRest = (gap >= data.division / 2) && (started || n.startTicks > 0);
        if (hasRest) {
            emitNoteToken(os, "0", durationSuffix(gap, data.division));
        }
        // 三连音组内的音符不标时值后缀（"3(...)" 已表达节奏）
        std::string dur = triplets[idx] ? "" : durationSuffix(n.durationTicks, data.division);
        emitNoteToken(os, pitchToJianpu(n.pitch, key), dur);
        started = true;
        prevEnd = n.startTicks + n.durationTicks;
    }
    if (inTriplet) os << ")";
    os << " |\n";
    return os.str();
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream os;
    os << "1 = " << keyName(key)
       << (key.isMajor ? " 大调" : " 小调")
       << "    " << data.timeSig.toString();
    if (!data.tempoChanges.empty()) {
        os << "    ♩=" << (int)data.tempoChanges[0].bpm;
    }
    os << "\n----------------------------------------\n";

    // 弯音修正：只统计与音符同通道的弯音事件
    auto bendOffset = [&](long start, long end, int channel) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.channel == channel && pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * data.bendRange;  // ±2半音范围
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    // 按 track 分组（应用弯音修正）
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks, n.channel);
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

    // 检测和弦(按小节)
    auto chords = detectChords(data, key);
    std::map<long, std::string> chordByBar;
    for (const auto& c : chords) {
        chordByBar[c.startTick] = c.name;
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
        os << "[" << label << "] (" << ns.size() << " 音符)";
        if (t == mainTrack && !chordByBar.empty()) {
            os << " · 和弦: " << chordByBar.size() << " 个";
        }
        os << "\n";
        os << formatTrack(ns, key, data, (t == mainTrack) ? &chordByBar : nullptr);
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

// 和弦检测：从伴奏轨中按小节提取音符，匹配和弦类型
std::vector<ChordMark> detectChords(const MidiData& data, const KeyEstimate& key) {
    std::vector<ChordMark> chords;

    // 找伴奏轨（音符数第二多的轨，或者非旋律轨）
    std::map<int, size_t> trackCount;
    for (const auto& note : data.notes) trackCount[note.track]++;
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, c] : trackCount) if (c > maxN) { maxN = c; mainTrack = t; }

    // 按小节分组音符
    double barLen = data.timeSig.barLengthTicks(data.division);
    std::map<long, std::vector<int>> barNotes;  // barStartTick -> pitchClasses
    for (const auto& note : data.notes) {
        if (note.track == mainTrack) continue;  // 跳过旋律轨
        long bar = (long)(note.startTicks / barLen);
        long barStart = bar * (long)barLen;
        barNotes[barStart].push_back(note.pitch % 12);
    }

    // 和弦类型定义（音程间隔）
    struct ChordType { const char* suffix; std::vector<int> intervals; };
    std::vector<ChordType> chordTypes = {
        {"",     {0, 4, 7}},      // 大三和弦
        {"m",    {0, 3, 7}},      // 小三和弦
        {"7",    {0, 4, 7, 10}},  // 属七和弦
        {"m7",   {0, 3, 7, 10}},  // 小七和弦
        {"maj7", {0, 4, 7, 11}},  // 大七和弦
        {"dim",  {0, 3, 6}},      // 减三和弦
        {"aug",  {0, 4, 8}},      // 增三和弦
        {"sus4", {0, 5, 7}},      // 挂四和弦
        {"sus2", {0, 2, 7}},      // 挂二和弦
    };

    static const char* NOTE_NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    // 对每个小节检测和弦
    for (auto& [barStart, pitches] : barNotes) {
        if (pitches.size() < 2) continue;  // 至少2个音符才可能是和弦

        // 统计音级出现次数
        std::map<int, int> pitchCount;
        for (int p : pitches) pitchCount[p]++;

        // 尝试每个音级作为根音
        int bestRoot = -1;
        std::string bestChord;
        int bestScore = 0;

        for (auto& [root, count] : pitchCount) {
            for (auto& ct : chordTypes) {
                // 检查这个和弦的所有音是否都在小节中出现
                int score = 0;
                bool allPresent = true;
                for (int interval : ct.intervals) {
                    int pitchClass = (root + interval) % 12;
                    if (pitchCount.count(pitchClass)) {
                        score += pitchCount[pitchClass];
                    } else {
                        allPresent = false;
                    }
                }
                // 所有音都存在才得分
                if (allPresent && score > bestScore) {
                    bestScore = score;
                    bestRoot = root;
                    bestChord = NOTE_NAMES[root] + std::string(ct.suffix);
                }
            }
        }

        if (bestRoot >= 0 && bestScore >= 3) {
            chords.push_back({barStart, bestChord});
        }
    }

    return chords;
}

} // namespace m2j

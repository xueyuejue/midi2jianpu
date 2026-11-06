// notation.cpp - 调性识别与简谱转换实现
#include "midi2jianpu/notation.hpp"

#include <array>
#include <algorithm>
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
    //
    // 除了时长，还要给终止音加权。Krumhansl-Schmuckler 轮廓假定旋律是完整的
    // 七声音阶，遇到五声/六声旋律会判偏：本仓库测试用的《蝶衣碎雪》只用了
    // A B C D E G 六个音，纯时长算出来是 E 小调，但 E 小调必须有 F#，
    // 全曲一个 F# 都没有。终止音（finalis）是调性最强的证据，加权后回到
    // 正确的 a 小调。
    long lastTick = -1;
    for (const auto& note : data.notes) {
        if (note.track != mainTrack) continue;
        if (note.startTicks > lastTick) lastTick = note.startTicks;
    }

    std::array<double, 12> pitchWeights{};
    double totalWeight = 0;
    for (const auto& note : data.notes) {
        if (note.track != mainTrack) continue;
        double w = note.durationSeconds;
        if (note.startTicks == lastTick) w *= 5.0;   // 终止音
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

int notationDoPitchClass(const KeyEstimate& key) {
    // 首调：大调的 1 就是主音；小调的 1 是关系大调主音（主音高小三度）
    return key.isMajor ? key.tonicPitchClass : (key.tonicPitchClass + 3) % 12;
}

std::string keyName(const KeyEstimate& key) {
    static const char* SHARP_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* FLAT_NAMES[12]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    int pc = notationDoPitchClass(key);
    return prefersFlats(key) ? FLAT_NAMES[pc] : SHARP_NAMES[pc];
}

std::string keyDescription(const KeyEstimate& key) {
    std::ostringstream os;
    os << "1=" << keyName(key);
    if (key.isMajor) {
        os << " 大调";
    } else {
        static const char* SHARP_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        static const char* FLAT_NAMES[12]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
        const char* n = prefersFlats(key) ? FLAT_NAMES[key.tonicPitchClass]
                                          : SHARP_NAMES[key.tonicPitchClass];
        os << " (" << n << "小调, 主音=6)";
    }
    return os.str();
}

std::string pitchToJianpu(int pitch, const KeyEstimate& key) {
    // 首调唱名：小调也按关系大调的音阶来数。小调音阶与关系大调本就是同一组音，
    // 只是主音落在 6 上。这样 a 小调写成 1=C、主音 A 记作 6，与简谱歌本一致；
    // 和声小调的升七级会自然落进下面的变音分支，记作 #5。
    const int doPc = notationDoPitchClass(key);
    const auto& scale = MAJOR;
    int semi = ((pitch - doPc) % 12 + 12) % 12;

    // 八度以"do 所在的中音区"为基准（旧实现以 C4=60 为基准，
    // 导致主音高于 C 的调性整体高一个八度点）
    int tonicRef = 60 + doPc;
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

// ---- 时值量化 ----
// 合法的记谱时值（单位为拍，1 拍 = 四分音符）。
// 同时包含二进制细分与三连音两套体系，量化时取最近者。
static const double NOTATABLE_BEATS[] = {
    1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3,
    3.0 / 4, 1.0, 3.0 / 2, 2.0, 3.0, 4.0
};

static double snapBeats(double beats) {
    if (beats <= 0) return NOTATABLE_BEATS[0];
    const int n = (int)(sizeof(NOTATABLE_BEATS) / sizeof(NOTATABLE_BEATS[0]));
    if (beats >= NOTATABLE_BEATS[n - 1]) return NOTATABLE_BEATS[n - 1];
    double best = NOTATABLE_BEATS[0];
    double bestDist = 1e9;
    for (int i = 0; i < n; i++) {
        double d = std::abs(beats - NOTATABLE_BEATS[i]);
        // 距离相同时取较短的那个：宁可写成八分+休止，也别硬拉成附点四分
        if (d < bestDist - 1e-9) { bestDist = d; best = NOTATABLE_BEATS[i]; }
    }
    return best;
}

QuantizeReport quantizeTiming(MidiData& data) {
    QuantizeReport rep;
    if (data.division <= 0 || data.notes.empty()) return rep;

    const int div = data.division;
    // 音头网格取 1/24 拍：32 分音符 = 3 格，十六分三连音 = 4 格，八分三连音 = 8 格，
    // 两套细分体系都能落格。
    long grid = div / 24;
    if (grid < 1) grid = 1;
    rep.gridTicks = grid;

    std::map<int, std::vector<size_t>> byTrack;
    for (size_t i = 0; i < data.notes.size(); i++)
        byTrack[data.notes[i].track].push_back(i);

    // ---- 第一遍：全曲估计门限因子 ----
    // 录音 MIDI 的 note-off 会提前，且门限时间通常是 DAW 里的一个全局常数，
    // 所以取 dur/ioi 的众数最稳；中位数会被"音符后面跟着长休止"的样本拉低。
    std::vector<double> ratios;
    for (auto& kv : byTrack) {
        auto& idxs = kv.second;
        std::sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
            return data.notes[a].startTicks < data.notes[b].startTicks;
        });
        std::vector<long> onsets;
        for (size_t i : idxs) {
            long s = data.notes[i].startTicks;
            if (onsets.empty() || onsets.back() != s) onsets.push_back(s);
        }
        for (size_t k = 0; k + 1 < onsets.size(); k++) {
            long ioi = onsets[k + 1] - onsets[k];
            if (ioi <= 0) continue;
            for (size_t i : idxs) {
                if (data.notes[i].startTicks != onsets[k]) continue;
                double r = (double)data.notes[i].durationTicks / ioi;
                if (r > 0.2 && r <= 1.0) ratios.push_back(r);
            }
        }
    }
    double gate = 1.0;
    if (ratios.size() >= 8) {
        // 按 0.02 分桶取众数
        std::map<int, int> hist;
        for (double r : ratios) hist[(int)std::lround(r * 50)]++;
        int bestBin = -1, bestCnt = 0;
        for (auto& kv : hist) if (kv.second > bestCnt) { bestCnt = kv.second; bestBin = kv.first; }
        // 众数至少要占样本的四分之一，否则说明这份 MIDI 没有统一门限
        if (bestCnt * 4 >= (int)ratios.size()) {
            gate = bestBin / 50.0;
            if (gate < 0.45 || gate > 1.0) gate = 1.0;
        }
    }
    rep.gate = gate;

    // ---- 第二遍：吸附音头与音尾 ----
    for (auto& kv : byTrack) {
        auto& idxs = kv.second;
        std::sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
            if (data.notes[a].startTicks != data.notes[b].startTicks)
                return data.notes[a].startTicks < data.notes[b].startTicks;
            return data.notes[a].pitch < data.notes[b].pitch;
        });

        for (size_t i : idxs) {
            long q = (long)std::llround((double)data.notes[i].startTicks / grid) * grid;
            if (q != data.notes[i].startTicks) {
                data.notes[i].startTicks = q;
                rep.adjusted++;
            }
        }

        std::vector<long> onsets;
        for (size_t i : idxs) {
            long s = data.notes[i].startTicks;
            if (onsets.empty() || onsets.back() != s) onsets.push_back(s);
        }

        // 该轨是否单声部：不同音头的音之间没有交叠，才敢用"到下一音头的间隔"当音值。
        // 钢琴这类复调轨一旦套用，左手长音会被右手音头截断。
        bool monophonic = true;
        {
            std::vector<long> ends(onsets.size(), 0);
            for (size_t i : idxs) {
                for (size_t k = 0; k < onsets.size(); k++) {
                    if (data.notes[i].startTicks != onsets[k]) continue;
                    long e = data.notes[i].startTicks + data.notes[i].durationTicks;
                    if (e > ends[k]) ends[k] = e;
                    break;
                }
            }
            for (size_t k = 0; k + 1 < onsets.size(); k++)
                if (ends[k] > onsets[k + 1] + grid / 2) { monophonic = false; break; }
        }

        for (size_t k = 0; k < onsets.size(); k++) {
            long s = onsets[k];
            long ioi = (k + 1 < onsets.size()) ? onsets[k + 1] - s : 0;

            for (size_t i : idxs) {
                if (data.notes[i].startTicks != s) continue;
                long oldDur = data.notes[i].durationTicks;
                double est = (double)oldDur / gate;   // 还原门限衰减

                long dur;
                if (monophonic && ioi > 0 && est >= 0.9 * ioi) {
                    // 连奏：音符一直响到下一个音头
                    dur = ioi;
                } else {
                    dur = (long)std::llround(snapBeats(est / div) * div);
                }
                if (monophonic && ioi > 0 && dur > ioi) dur = ioi;

                long snapped = (long)std::llround(snapBeats((double)dur / div) * div);
                if (snapped > 0) dur = snapped;
                if (dur < 1) dur = 1;

                if (dur != oldDur) {
                    data.notes[i].durationTicks = dur;
                    rep.adjusted++;
                }
            }
        }
    }

    // 音头被吸附后顺序可能变化，重新排一次
    std::stable_sort(data.notes.begin(), data.notes.end(),
                     [](const NoteEvent& a, const NoteEvent& b) {
                         if (a.startTicks != b.startTicks) return a.startTicks < b.startTicks;
                         return a.pitch < b.pitch;
                     });
    return rep;
}

// 查询某 tick 位置、某通道的当前力度（从 dynamics 曲线），映射为简谱动态标记
// 旧实现不看通道，会把伴奏的力度标到旋律上
static std::string dynAt(long tick, int channel, const MidiData& data) {    int val = -1;
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

// 检测三连音: 返回每个音符是否属于三连音组
//
// 旧实现只判断"连续三个时值相近的音符"，于是任何八分音符连奏都会被误判，
// 整份谱子被 3( ... ) 包住。三连音的时值必然属于 1/3 拍体系（1/3、1/6、1/12 拍），
// 而这个区间又和二分数值明显不同，据此判断才可靠。
std::vector<bool> detectTriplets(const std::vector<NoteEvent>& notes, int division) {
    std::vector<bool> isTriplet(notes.size(), false);
    if (division <= 0 || notes.size() < 3) return isTriplet;

    static const double TERNARY[] = {1.0 / 3, 1.0 / 6, 1.0 / 12};
    static const double BINARY[]  = {3.0 / 2, 1.0, 3.0 / 4, 1.0 / 2, 3.0 / 8, 1.0 / 4, 1.0 / 8};

    auto relErr = [](double v, const double* arr, int n) {
        double best = 1e9;
        for (int i = 0; i < n; i++)
            best = std::min(best, std::abs(v - arr[i]) / arr[i]);
        return best;
    };

    long tol = std::max<long>(2, division / 40);
    for (size_t i = 0; i + 2 < notes.size(); i++) {
        double d1 = (double)notes[i].durationTicks / division;
        double d2 = (double)notes[i + 1].durationTicks / division;
        double d3 = (double)notes[i + 2].durationTicks / division;
        if (d1 <= 0) continue;

        // 三音等长
        if (std::abs(d1 - d2) / d1 > 0.12) continue;
        if (std::abs(d2 - d3) / d2 > 0.12) continue;
        // 时值必须贴近 1/3 体系，且不能同时贴近二分数值
        if (relErr(d1, TERNARY, 3) > 0.12) continue;
        if (relErr(d1, BINARY, 7) < 0.10) continue;
        // 三个音首尾相接，无间隙
        if (std::abs((notes[i + 1].startTicks - notes[i].startTicks)
                     - notes[i].durationTicks) > tol) continue;
        if (std::abs((notes[i + 2].startTicks - notes[i + 1].startTicks)
                     - notes[i + 1].durationTicks) > tol) continue;

        isTriplet[i] = isTriplet[i + 1] = isTriplet[i + 2] = true;
        i += 2;
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
    os << keyDescription(key)
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
// 宽松数字解析（定义见文件下方）。WASM 版默认关闭 C++ 异常，
// std::stoi / std::stod 遇到畸形输入会直接 abort 整个页面，故统一用它。
static double parseNum(const std::string& s, size_t from, size_t to);

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
        int beat = (int)parseNum(content, colon + 1, end);

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
            int nextBeat = (int)parseNum(content, nc + 1, ne);
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
        double mm = parseNum(ts, 0, colon);
        double ss = parseNum(ts, colon + 1, ts.size());
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
// 宽松数字解析。WASM 版默认关闭 C++ 异常，std::stoi / std::stod 遇到畸形输入
// 会直接 abort（整个页面崩掉），所以这里手写一个不抛异常的版本，非法内容按 0 处理。
static double parseNum(const std::string& s, size_t from, size_t to) {
    if (to > s.size()) to = s.size();
    size_t i = from;
    while (i < to && (s[i] == ' ' || s[i] == '\t')) i++;
    bool neg = false;
    if (i < to && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }
    double v = 0;
    bool frac = false;
    double scale = 1;
    for (; i < to; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (frac) { scale /= 10.0; v += (c - '0') * scale; }
            else v = v * 10.0 + (c - '0');
        } else if (c == '.') {
            if (frac) break;
            frac = true;
        } else {
            break;
        }
    }
    return neg ? -v : v;
}

std::vector<LyricLine> parseLrcText(const std::string& lrc, int division, int bpm) {
    std::vector<LyricLine> lines;
    if (division <= 0 || bpm <= 0) return lines;
    const double secPerBeat = 60.0 / bpm;

    size_t pos = 0;
    while (pos < lrc.size()) {
        size_t nl = lrc.find('\n', pos);
        if (nl == std::string::npos) nl = lrc.size();
        std::string line = lrc.substr(pos, nl - pos);
        pos = (nl >= lrc.size()) ? lrc.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.size() < 4 || line[0] != '[') continue;
        size_t close = line.find(']');
        if (close == std::string::npos) continue;
        std::string ts = line.substr(1, close - 1);
        // 跳过 [ti:] [ar:] [al:] [by:] 等元数据
        if (ts.empty() || !isdigit((unsigned char)ts[0])) continue;
        size_t colon = ts.find(':');
        if (colon == std::string::npos) continue;

        double mm = parseNum(ts, 0, colon);
        double ss = parseNum(ts, colon + 1, ts.size());
        long startTick = (long)((mm * 60.0 + ss) / secPerBeat * division);

        std::string text = line.substr(close + 1);
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
            text.pop_back();
        if (!text.empty()) lines.push_back({startTick, text});
    }
    return lines;
}

std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, int division, int bpm) {
    std::ifstream f(lrcPath);
    if (!f.is_open()) return {};
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseLrcText(all, division, bpm);
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

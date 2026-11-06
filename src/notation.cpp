// notation.cpp - 调性识别与简谱转换实现
#include "midi2jianpu/notation.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <map>
#include <set>
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
    // 只用旋律轨，避免伴奏和弦把音级分布拉平（判定逻辑见 pickMelodyTrack）
    int mainTrack = pickMelodyTrack(data, nullptr);

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

    // 3) 五声调式校正：KS 用的是七声轮廓，遇到纯五声会把宫音（简谱的 1）
    // 判到终止音上。只在没有 0x59 调号、检测高置信、且宫音与 KS 结果冲突时
    // 才改写；宫音一致时保留原大小调描述，只补一条调式说明——这样普通
    // 流行歌和 a 小调式羽调旋律的唱名一个字都不会变。
    if (data.usePentatonic) {
        const PentatonicEstimate p = detectPentatonic(data);
        if (p.detected) {
            r.scaleLabel = p.label();
            if (notationDoPitchClass(r) != p.gongPitchClass) {
                r.tonicPitchClass = p.gongPitchClass;
                r.isMajor = true;   // 按宫音记谱，do 才落在宫音上
            }
        }
    }
    return r;
}

// ---- 五声调式（宫商角徵羽）----
static const char* SHARP_PC[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char* FLAT_PC[12] = {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

static std::string pitchClassName(int pc, bool flat) {
    pc = ((pc % 12) + 12) % 12;
    return flat ? FLAT_PC[pc] : SHARP_PC[pc];
}

std::string pentatonicModeName(PentatonicMode mode) {
    switch (mode) {
        case PentatonicMode::Gong:  return "宫";
        case PentatonicMode::Shang: return "商";
        case PentatonicMode::Jue:   return "角";
        case PentatonicMode::Zhi:   return "徵";
        case PentatonicMode::Yu:    return "羽";
        default:                    return "";
    }
}

static PentatonicMode modeOfFinal(int gong, int finalPc) {
    switch (((finalPc - gong) % 12 + 12) % 12) {
        case 0:  return PentatonicMode::Gong;
        case 2:  return PentatonicMode::Shang;
        case 4:  return PentatonicMode::Jue;
        case 7:  return PentatonicMode::Zhi;
        case 9:  return PentatonicMode::Yu;
        default: return PentatonicMode::None;
    }
}

static std::string makeScaleLabel(int gong, int finalPc, PentatonicMode mode) {
    KeyEstimate k;
    k.tonicPitchClass = ((gong % 12) + 12) % 12;
    k.isMajor = true;
    const bool flat = keyFifths(k) < 0;
    std::ostringstream os;
    os << pitchClassName(finalPc >= 0 ? finalPc : gong, flat)
       << " " << pentatonicModeName(mode) << "调式（宫音 "
       << pitchClassName(gong, flat) << "）";
    return os.str();
}

std::string PentatonicEstimate::label() const {
    if (!detected) return {};
    return makeScaleLabel(gongPitchClass, finalPitchClass, mode);
}

// 旋律轨的音级时长分布。权重口径与 estimateKey 完全一致（终止音 ×5），
// 两个判定看到的证据才不会互相打架。
struct PitchProfile {
    std::array<double, 12> weight{};
    double total = 0;
    int finalPc = -1;
    int distinct = 0;
};

static PitchProfile melodyPitchProfile(const MidiData& data) {
    PitchProfile r;
    const int mainTrack = pickMelodyTrack(data, nullptr);
    long lastTick = -1;
    for (const auto& n : data.notes)
        if (n.track == mainTrack && n.startTicks > lastTick) lastTick = n.startTicks;
    if (lastTick < 0) return r;

    // 终止音取最后一拍里最低的那个音：旋律落音通常是调式主音，柱式终止
    // 和弦取最低音也符合歌本习惯。
    int finalPitch = 1 << 30;
    for (const auto& n : data.notes) {
        if (n.track != mainTrack) continue;
        double w = (n.durationSeconds > 0) ? n.durationSeconds : 1.0;
        if (n.startTicks == lastTick) {
            w *= 5.0;
            if (n.pitch < finalPitch) {
                finalPitch = n.pitch;
                r.finalPc = ((n.pitch % 12) + 12) % 12;
            }
        }
        r.weight[((n.pitch % 12) + 12) % 12] += w;
        r.total += w;
    }
    if (r.total <= 0 || r.finalPc < 0) return r;
    for (double w : r.weight)
        if (w > r.total * 0.005) r.distinct++;
    return r;
}

PentatonicEstimate detectPentatonic(const MidiData& data) {
    PentatonicEstimate r;
    const PitchProfile prof = melodyPitchProfile(data);
    r.distinctPitchClasses = prof.distinct;
    if (prof.distinct < 4 || prof.distinct > 6) return r;  // 音太少定不了调式；≥7 是七声
    if (prof.finalPc < 0) return r;

    static const int PENTA[5] = {0, 2, 4, 7, 9};  // 宫 商 角 徵 羽
    // 六声里允许出现的偏音（相对宫音）：清角 4、闰 b7、变徵 #4、变宫 7
    static const int BIAS[4] = {5, 10, 6, 11};

    double bestCov = -1, secondCov = -1, bestOutside = 0;
    int bestGong = -1;
    for (int g = 0; g < 12; g++) {
        bool in[12] = {};
        for (int d : PENTA) in[(g + d) % 12] = true;
        if (!in[prof.finalPc]) continue;   // 终止音必须是正音，否则不是这个宫系统

        double outside = 0;
        int outsidePc = -1, outsideCount = 0;
        for (int i = 0; i < 12; i++) {
            if (in[i]) continue;
            outside += prof.weight[i];
            if (prof.weight[i] > prof.total * 0.005) { outsideCount++; outsidePc = i; }
        }
        if (outsideCount > 1) continue;     // 两个以上偏音 = 七声，交回 KS
        if (outsideCount == 1) {
            const int rel = ((outsidePc - g) % 12 + 12) % 12;
            bool allowed = false;
            for (int b : BIAS) if (rel == b) allowed = true;
            if (!allowed) continue;
        }
        const double cov = 1.0 - outside / prof.total;
        if (cov < 0.92) continue;          // 偏音占比过高，不能算五声/六声
        if (cov > bestCov) { secondCov = bestCov; bestCov = cov; bestGong = g; bestOutside = outside; }
        else if (cov > secondCov) secondCov = cov;
    }
    if (bestGong < 0) return r;
    // 只用四五个音级时可能有两个宫系统都完全覆盖（例如 C D G A 既像
    // C 宫也像 F 宫）。宁可不报，也不要把 1 定错。
    if (secondCov > 0 && bestCov - secondCov < 0.004) return r;

    const PentatonicMode mode = modeOfFinal(bestGong, prof.finalPc);
    if (mode == PentatonicMode::None) return r;
    r.detected = true;
    r.mode = mode;
    r.gongPitchClass = bestGong;
    r.finalPitchClass = prof.finalPc;
    r.confidence = bestCov;
    r.outsideWeight = bestOutside / prof.total;
    return r;
}

KeyEstimate keyFromGong(const MidiData& data, int gongPitchClass) {
    KeyEstimate k;
    k.tonicPitchClass = ((gongPitchClass % 12) + 12) % 12;
    k.isMajor = true;
    const PitchProfile prof = melodyPitchProfile(data);
    PentatonicMode mode = PentatonicMode::Gong;
    int finalPc = -1;
    if (prof.finalPc >= 0) {
        finalPc = prof.finalPc;
        const PentatonicMode m = modeOfFinal(k.tonicPitchClass, finalPc);
        if (m != PentatonicMode::None) mode = m;
    }
    k.scaleLabel = makeScaleLabel(k.tonicPitchClass, finalPc, mode);
    return k;
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
    if (!key.scaleLabel.empty()) os << " · " << key.scaleLabel;
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

// 实质性的拍号变化：排序、去掉多轨重复声明、去掉与当前生效拍号相同的事件。
// format 1 的 conductor 轨和乐器轨经常各自写一遍 4/4，那不是变拍号。
std::vector<TimeSigPoint> meterChangesOf(const MidiData& data) {
    if (data.timeSigChanges.empty()) return {};
    std::vector<TimeSigPoint> v = data.timeSigChanges;
    std::sort(v.begin(), v.end(),
              [](const TimeSigPoint& a, const TimeSigPoint& b) { return a.tick < b.tick; });
    std::vector<TimeSigPoint> uniq;
    uniq.reserve(v.size());
    for (const auto& p : v) {
        if (!uniq.empty() && uniq.back().tick == p.tick) uniq.back() = p;
        else uniq.push_back(p);
    }
    TimeSignature cur = data.timeSig.valid ? data.timeSig : uniq.front().sig;
    std::vector<TimeSigPoint> out;
    for (const auto& p : uniq) {
        if (p.tick <= 0) { cur = p.sig; continue; }   // tick 0 是曲首拍号
        if (p.sig.toString() == cur.toString()) continue;
        out.push_back(p);
        cur = p.sig;
    }
    return out;
}

// 拍号变化摘要。无变化时就是曲首拍号，调用方可以直接替换原来的 toString()。
std::string meterSummary(const MidiData& data) {
    TimeSignature first = data.timeSig.valid ? data.timeSig : TimeSignature{4, 2, true};
    std::ostringstream os;
    os << first.toString();
    const auto changes = meterChangesOf(data);
    if (changes.empty()) return os.str();

    const BarGrid grid = barGridOf(data);
    for (size_t i = 0; i < changes.size() && i < 4; i++) {
        long bar = grid.barIndex(changes[i].tick);
        os << " → " << changes[i].sig.toString()
           << "(第" << grid.measureNumber(bar) << "小节)";
    }
    if (changes.size() > 4) os << " → …";
    return os.str();
}

// ---- 弱起小节 ----
//
// 判定见 notation.hpp 的注释：只用「从 tick 0 起唱」+「末音收在小节线上」两条
// 硬证据。两条都成立时，弱起长度只有一种解释，才敢改网格。
BarGrid barGridOf(const MidiData& data) {
    BarGrid g;
    TimeSignature first = data.timeSig.valid ? data.timeSig : TimeSignature{4, 2, true};
    auto lenOf = [&](const TimeSignature& s) -> long {
        long l = (long)std::llround(s.barLengthTicks(data.division));
        if (l <= 0) l = (long)data.division * 4;
        return l > 0 ? l : 1920;
    };
    long bl = lenOf(first);
    g.barLen = bl;
    g.beatsPerBar = first.numerator > 0 ? first.numerator : 4;
    g.tailBarLen = bl;
    g.pickup = effectivePickupTicks(data);
    // 弱起不可能有小节那么长；越界的值一律按「没有弱起」处理，
    // 否则后面 barIndex/barStart 会算出负的小节
    if (g.pickup < 0 || g.pickup >= g.barLen) g.pickup = 0;

    const auto changes = meterChangesOf(data);

    // 边界表要铺到最后一个音之后再多一节，延长段才不会跨出表尾
    long songEnd = 0;
    for (const auto& n : data.notes)
        songEnd = std::max(songEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    const long horizon = songEnd + bl;

    long cur = 0;
    size_t ci = 0;
    TimeSignature sig = first;
    long curLen = bl;
    if (g.pickup > 0) {
        g.starts.push_back(0);          // bar 0 = 弱起那一小截
        g.sigs.push_back(first);
        cur = g.pickup;
    }
    // 上限防止坏文件里「极小 barLen + 巨大 tick」把内存吃光
    while (cur <= horizon && g.starts.size() < 20000) {
        while (ci < changes.size() && changes[ci].tick <= cur) {
            sig = changes[ci].sig;
            curLen = lenOf(sig);
            ci++;
        }
        g.starts.push_back(cur);
        g.sigs.push_back(sig);
        g.tailBarLen = curLen;
        long nxt = cur + curLen;
        // 0x58 应该落在小节线上，但导出器写歪的很常见：此时在当前小节中间
        // 截断，让新拍号从事件 tick 起生效，比把它推迟一整节更接近作者意图。
        if (ci < changes.size() && changes[ci].tick > cur && changes[ci].tick < nxt)
            nxt = changes[ci].tick;
        cur = nxt;
    }
    if (g.starts.empty()) { g.starts.push_back(0); g.sigs.push_back(first); }
    return g;
}

long effectivePickupTicks(const MidiData& data) {
    if (data.pickupTicks >= 0) return data.pickupTicks;   // 手动指定
    return detectPickupTicks(data);
}

long detectPickupTicks(const MidiData& data) {
    if (data.division <= 0 || data.notes.empty()) return 0;
    // 拍号会变的曲子里，「末音 mod 小节长 = 弱起长度」这个公式根本不成立：
    // 总长是各段小节长之和，不再是某个常量的整数倍。探针里 4/4→3/4→6/8
    // 就被算成「弱起 4 拍」，网格又整体平移一次，错得比不改还离谱。
    // 宁可不判（用户还能 --pickup 手动指定），也不能拿来路不明的弱起。
    if (!meterChangesOf(data).empty()) return 0;
    double barLenD = data.timeSig.barLengthTicks(data.division);
    if (!(barLenD > 0)) return 0;
    long barLen = (long)std::llround(barLenD);
    int beats = data.timeSig.numerator > 0 ? data.timeSig.numerator : 4;
    if (beats < 2 || barLen <= 0) return 0;
    long beatLen = barLen / beats;
    if (beatLen <= 0) return 0;

    long firstOnset = -1;
    long lastEnd = 0;
    for (const auto& n : data.notes) {
        if (firstOnset < 0 || n.startTicks < firstOnset) firstOnset = n.startTicks;
        lastEnd = std::max(lastEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    }
    // 条件 1：曲子必须从开头直接起唱。开头若干小节空白是前奏休止，不是弱起
    if (firstOnset < 0 || firstOnset > beatLen / 8) return 0;

    // 条件 2：末音收在小节线上。弱起的总长 = P + N*小节长，
    // 所以 lastEnd mod 小节长 就是弱起长度，就近吸附到整拍。
    long rem = lastEnd % barLen;
    long tol = beatLen / 4;        // 允许收尾差四分之一拍（门限衰减、气口）
    long bestP = 0;
    long bestErr = barLen;
    for (int k = 1; k < beats; k++) {
        long p = beatLen * k;
        long err = rem >= p ? rem - p : p - rem;
        if (err < bestErr) { bestErr = err; bestP = p; }
    }
    if (bestP <= 0 || bestErr > tol) return 0;

    // 条件 3：弱起之后的第一拍必须真有音起。弱起是「引向下一小节强拍的几个音」，
    // 下一小节的强拍如果没有音起（只是上一个音在延续），那说明根本没有弱起，
    // 只是最后一个音停在了小节中间——这正是误判最常发生的样子。
    bool onsetAtDownbeat = false;
    for (const auto& n : data.notes) {
        if (std::labs(n.startTicks - bestP) <= beatLen / 8) { onsetAtDownbeat = true; break; }
    }
    if (!onsetAtDownbeat) return 0;

    // 弱起之后至少要有一个完整小节，否则「弱起」和「整曲只有一小节」分不开
    if (lastEnd < bestP + barLen) return 0;
    return bestP;
}

// 弱起的小节数按「拍」来报：简谱读者想的是一共少了几拍，不是几百个 tick。
std::string pickupLabel(const MidiData& data) {
    BarGrid grid = barGridOf(data);
    if (grid.pickup <= 0) return std::string();
    long beatLen = grid.barLen / std::max(1, grid.beatsPerBar);
    if (beatLen <= 0) return std::string();
    double beats = (double)grid.pickup / (double)beatLen;
    std::ostringstream os;
    os << "弱起 ";
    if (std::fabs(beats - std::round(beats)) < 0.05) {
        os << (long)std::lround(beats);
    } else {
        os.setf(std::ios::fixed);
        os.precision(1);
        os << beats;
    }
    os << " 拍";
    return os.str();
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

// ---- 调性 map（中途转调） ----

std::vector<KeySigPoint> keyChangesOf(const MidiData& data) {
    std::vector<KeySigPoint> out;
    if (!data.applyKeyChanges || data.keySigChanges.size() < 2) return out;
    // 与「当前生效调号」比较而不是与曲首比较：C→D→C 的最后一次是转回原调，
    // 相对曲首没变，但谱面上必须再写一次 1=C。
    int cur = data.keySigChanges.front().sig.sharpsFlats;
    for (size_t i = 1; i < data.keySigChanges.size(); i++) {
        const auto& p = data.keySigChanges[i];
        if (p.sig.sharpsFlats == cur) continue;   // 重复声明，不是转调
        out.push_back(p);
        cur = p.sig.sharpsFlats;
    }
    return out;
}

KeyMap::KeyMap(const MidiData& data, const KeyEstimate& base) : base_(base) {
    const auto pts = keyChangesOf(data);
    if (pts.empty()) return;

    const BarGrid grid = barGridOf(data);
    const int sf0 = data.keySigChanges.front().sig.sharpsFlats;
    const int doPc0 = notationDoPitchClass(base);
    int curDo = doPc0;

    for (const auto& p : pts) {
        // 首调 do 只由调号的升降数决定，与大小调无关（a 小调与 C 大调同是
        // 1=C）。五度循环里每多一个升号，do 上移 7 个半音。所以转调幅度是
        // 一个「相对量」：用户 --key 手工指定曲首时，整条转调链跟着平移，
        // 而不是跳回 MIDI 文件写的绝对调号。
        int d = (7 * (p.sig.sharpsFlats - sf0)) % 12;
        if (d < 0) d += 12;
        const int doPc = (doPc0 + d) % 12;
        if (doPc == curDo) continue;   // 等音/绕回原调，唱名没变

        KeyEstimate k;
        k.tonicPitchClass = base.isMajor ? doPc : (doPc - 3 + 12) % 12;
        k.isMajor = base.isMajor;      // mode 继承曲首，见头文件说明
        // 吸附到小节起点：简谱的转调标记落在小节线上，不允许一小节里
        // 前半用旧唱名、后半用新唱名。
        const long tick = grid.barStart(grid.barIndex(p.tick));
        KeyChange ch{tick, k, d, "1=" + keyName(k)};
        if (!changes_.empty() && changes_.back().tick >= tick) changes_.back() = ch;
        else changes_.push_back(ch);
        curDo = doPc;
    }
}

KeyEstimate KeyMap::at(long tick) const {
    KeyEstimate k = base_;
    for (const auto& c : changes_) {
        if (c.tick > tick) break;
        k = c.key;
    }
    return k;
}

bool KeyMap::keyChangedAtBar(long bar, const BarGrid& grid) const {
    if (bar <= 0 || changes_.empty()) return false;
    return notationDoPitchClass(keyAtBar(bar, grid)) !=
           notationDoPitchClass(keyAtBar(bar - 1, grid));
}

std::string KeyMap::summary(const BarGrid& grid) const {
    std::ostringstream os;
    os << keyDescription(base_);
    for (const auto& c : changes_) {
        os << " → " << c.label << "(第"
           << grid.measureNumber(grid.barIndex(c.tick)) << "小节)";
    }
    return os.str();
}

// 输出单个轨的简谱，返回该轨输出文本
static std::string formatTrack(
    const std::vector<NoteEvent>& notes,
    const KeyMap& keys, const MidiData& data,
    const std::map<long, std::string>* chords = nullptr) {

    std::ostringstream os;
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
    const BarGrid grid = barGridOf(data);
    auto barIndexOf = [&](long tick) { return grid.barIndex(tick); };
    for (size_t i = 0; i < notes.size(); i++) {
        if (i > 0 && !trip0[i] && !trip0[i - 1] && !merged.empty() &&
            notes[i].pitch == merged.back().pitch &&
            notes[i].startTicks == merged.back().startTicks + merged.back().durationTicks &&
            isSingle(notes[i].startTicks) && isSingle(merged.back().startTicks) &&
            // 不跨小节线合并：真实演奏里同音相邻几乎都是两次发音而不是延音，
            // 合并后会把小节线上的音吞掉（第 15 小节开头的 6 变成 `-`）。
            // 跨小节的长音本来就该显式切分。
            barIndexOf(notes[i].startTicks) == barIndexOf(merged.back().startTicks)) {
            merged.back().durationTicks += notes[i].durationTicks;
            merged.back().durationSeconds += notes[i].durationSeconds;
            continue;
        }
        merged.push_back(notes[i]);
    }
    auto triplets = detectTriplets(merged, data.division);
    bool inTriplet = false;

    // ---- 小节线推进 ----
    // 关键：小节线只能由「拍子位置」决定，不能只在音符音头处切换。
    // 否则整段前奏（没有任何音符）会全部堆进第 1 小节，挤出一串十几个 '-'
    // 而丢掉 3 条小节线；跨小节的长音同样会溢出到下一小节之外。
    auto barOf = [&](long tick) -> long { return grid.barIndex(tick); };
    std::string lastChord;
    long lastChordTick = -1;   // 已经处理过的和弦位置，避免同一和弦标两次
    auto switchToBar = [&](long bar, long atTick, int channel) {
        if ((int)bar == currentBar) return;
        if (currentBar >= 0) os << " |\n";
        currentBar = (int)bar;
        // 变拍号处把新拍号印在谱面上（只印旋律轨，伴奏轨不重复）。
        // 页眉的摘要说明「什么时候变」，这里说明「从哪一栏开始按新拍子数」。
        if (chords && grid.meterChangedAt(bar))
            os << "[" << grid.sigForBar(bar).toString() << "] ";
        // 转调标记落在起作用的那一小节：页眉的摘要说「什么时候变」，
        // 这里说「从哪一栏起按新调唱」。用「转」字与 [3/4] 拍号、
        // [C] 和弦区分开，纯文本里也不会看混。
        if (chords && keys.keyChangedAtBar(bar, grid))
            os << "[转" << ("1=" + keyName(keys.keyAtBar(bar, grid))) << "] ";
        // 和弦标记只标在旋律轨上，避免伴奏轨重复标注；
        // 连续小节是同一个和弦时不重复写——歌本里和弦只在变化处标注
        if (chords) {
            auto it = chords->find(grid.barStart(bar));
            if (it != chords->end() && it->second != lastChord) {
                os << "[" << it->second << "] ";
                lastChord = it->second;
            }
            lastChordTick = std::max(lastChordTick, grid.barStart(bar));
        }
        std::string d = dynAt(atTick, channel, data);
        if (!d.empty() && d != lastDyn) {
            os << "[" << d << "] ";
            lastDyn = d;
        }
    };
    // 跨小节时，后段用延长线续写：整拍输出 "-"，不足一拍带时值后缀
    auto emitHeld = [&](long segTicks) {
        if (segTicks <= 0) return;
        if (segTicks % data.division == 0) {
            for (long k = 0; k < segTicks / data.division; ++k) os << "- ";
        } else {
            emitNoteToken(os, "-", durationSuffix(segTicks, data.division));
        }
    };

    for (size_t idx = 0; idx < merged.size(); idx++) {
        const auto& n = merged[idx];

        // 右括号必须先于休止输出，否则 3(5 5 5 0 ) 会把休止吞进三连音组
        if (!triplets[idx] && inTriplet) { os << ") "; inTriplet = false; }

        // 休止：同样按小节切开，长前奏才能写成每小节一个 "0 - - -"
        long gap = n.startTicks - prevEnd;
        bool hasRest = (gap >= data.division / 2) && (started || n.startTicks > 0);
        if (hasRest) {
            long t = prevEnd;
            while (t < n.startTicks) {
                long bar = barOf(t);
                switchToBar(bar, t, n.channel);
                long seg = std::min(n.startTicks, grid.barEnd(bar)) - t;
                if (seg <= 0) break;                    // 防御：避免死循环
                emitNoteToken(os, "0", durationSuffix(seg, data.division));
                t += seg;
            }
        }

        switchToBar(barOf(n.startTicks), n.startTicks, n.channel);

        // 小节内换和弦（第 3 拍、6/8 的第二个附点四分……）。
        // 只在小节起点找和弦会把这类变化整个漏掉——而流行歌里一小节两个
        // 和弦比一小节一个更常见。跨过多个和弦时只留最后一个：文本谱一行
        // 挤三个 [C][Am][F] 反而看不出位置。
        if (chords) {   // 传进来的和弦表本来就是 nullptr（非旋律轨）
            std::string pending;
            auto cit = chords->upper_bound(lastChordTick);
            for (; cit != chords->end() && cit->first <= n.startTicks; ++cit) {
                if (cit->second != lastChord) pending = cit->second;
                lastChordTick = cit->first;
            }
            if (!pending.empty()) {
                os << "[" << pending << "] ";
                lastChord = pending;
            }
        }

        // 左括号放在小节线之后，否则 "3(" 会留在上一行末
        if (triplets[idx] && !inTriplet) { os << "3("; inTriplet = true; }

        // 三连音组内的音符不标时值后缀（"3(...)" 已表达节奏）
        std::string head = pitchToJianpu(n.pitch, keys.at(n.startTicks));
        long noteEnd = n.startTicks + n.durationTicks;
        long barEnd = grid.barEnd(barOf(n.startTicks));
        if (triplets[idx] || noteEnd <= barEnd) {
            emitNoteToken(os, head, triplets[idx] ? "" : durationSuffix(n.durationTicks, data.division));
        } else {
            // 长音跨小节：先写满本小节，再逐小节用延长线续上
            emitNoteToken(os, head, durationSuffix(barEnd - n.startTicks, data.division));
            long t = barEnd;
            while (t < noteEnd) {
                long bar = barOf(t);
                switchToBar(bar, t, n.channel);
                long seg = std::min(noteEnd, grid.barEnd(bar)) - t;
                if (seg <= 0) break;                    // 防御：避免死循环
                emitHeld(seg);
                t += seg;
            }
        }
        started = true;
        prevEnd = noteEnd;
    }
    if (inTriplet) os << ")";
    os << " |\n";
    return os.str();
}

// 不同和弦名的个数（同一个小节反复出现的同一个和弦只算一个）
static size_t distinctChordCount(const std::map<long, std::string>& byBar) {
    std::set<std::string> names;
    for (const auto& kv : byBar) names.insert(kv.second);
    return names.size();
}

// 速度摘要：单一速度写 "♩=120"，变速曲目写 "♩=120 → 90(第5小节)"。
// 只写第一个速度是错的：后半首按 90 唱，谱面却还印着 120。
std::string tempoSummary(const MidiData& data) {
    if (data.tempoChanges.empty()) return std::string();
    std::ostringstream os;
    os << "♩=" << (int)std::lround(data.tempoChanges[0].bpm);
    if (data.tempoChanges.size() > 1) {
        // 小节号走网格换算：弱起曲目里 tick/小节长 会整体偏一号
        BarGrid grid = barGridOf(data);
        for (size_t i = 1; i < data.tempoChanges.size() && i <= 4; i++) {
            os << (i > 1 ? " → " : " → ")
               << (int)std::lround(data.tempoChanges[i].bpm)
               << "(第" << grid.measureNumber(grid.barIndex(data.tempoChanges[i].tick))
               << "小节)";
        }
        if (data.tempoChanges.size() > 5) os << " → …";
    }
    return os.str();
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key) {
    const KeyMap keys(data, key);
    const BarGrid grid = barGridOf(data);
    std::ostringstream os;
    os << keys.summary(grid)
       << "    " << meterSummary(data);
    std::string tempo = tempoSummary(data);
    if (!tempo.empty()) os << "    " << tempo;
    std::string pickup = pickupLabel(data);
    if (!pickup.empty()) os << "    " << pickup;
    // 鼓轨被丢弃这件事必须让用户看见，否则会以为谱子漏了内容。
    // 写进谱面头部而不是只打在命令行，网页版（WASM）才同样能看到。
    if (data.drumNotesSkipped > 0) {
        os << "    (已跳过 " << data.drumNotesSkipped << " 个打击乐音符)";
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

    // 选出旋律轨：和弦标记与歌词只给它
    int mainTrack = pickMelodyTrack(data, nullptr);

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
            os << " · 和弦: " << distinctChordCount(chordByBar) << " 个";
        }
        os << "\n";
        os << formatTrack(ns, keys, data, (t == mainTrack) ? &chordByBar : nullptr);
    }
    return os.str();
}

// ---- 旋律轨判定 ----
std::vector<TrackProfile> profileTracks(const MidiData& data,
                                       const std::vector<LyricLine>* lyrics) {
    std::vector<TrackProfile> out;
    std::map<int, std::vector<const NoteEvent*>> byTrack;
    for (const auto& n : data.notes) byTrack[n.track].push_back(&n);

    double barLen = data.timeSig.barLengthTicks(data.division);
    if (!(barLen > 0)) barLen = data.division * 4.0;

    // 内置歌词事件所在的轨是文件作者明确标出来的「人声轨」，比按音符数、
    // 单音率猜都可靠。format 0 或歌词分散在多轨时退回软证据：把歌词 tick
    // 当作行首，走下面原有的对齐打分。
    const int builtinLyricTrack = midiLyricTrack(data);
    std::vector<LyricLine> hints;
    if (lyrics && !lyrics->empty()) {
        hints = *lyrics;
    } else if (!data.midiLyrics.empty()) {
        for (const auto& s : normalizeMidiLyrics(data)) hints.push_back({s.tick, s.text});
    }

    for (auto& kv : byTrack) {
        const auto& ns = kv.second;
        TrackProfile p;
        p.track = kv.first;
        p.noteCount = ns.size();

        std::set<long> onsets;
        std::set<int> pitches;
        long sumPitch = 0;
        p.minPitch = 127;
        p.maxPitch = 0;
        for (const auto* n : ns) {
            onsets.insert(n->startTicks);
            pitches.insert(n->pitch);
            sumPitch += n->pitch;
            p.minPitch = std::min(p.minPitch, n->pitch);
            p.maxPitch = std::max(p.maxPitch, n->pitch);
        }
        p.onsetCount = onsets.size();
        p.distinctPitches = pitches.size();
        p.avgPitch = ns.empty() ? 0.0 : (double)sumPitch / (double)ns.size();
        p.polyphony = onsets.empty() ? 1.0 : (double)ns.size() / (double)onsets.size();

        // 歌词对齐：歌词行首的 tick 落在该轨某个音符的半个小节内，算命中一行。
        // LRC 的时间戳精度只到 10ms，且常与 MIDI 有几十毫秒偏差，容差不能太紧。
        if (!hints.empty()) {
            size_t hit = 0;
            for (const auto& line : hints) {
                for (const auto* n : ns) {
                    if (std::fabs((double)n->startTicks - (double)line.startTick) <= barLen * 0.5) {
                        hit++;
                        break;
                    }
                }
            }
            p.lyricAligned = hit * 2 >= hints.size();
        }

        double s = std::log((double)p.noteCount + 1.0);
        s -= (p.polyphony - 1.0) * 4.0;                       // 复音惩罚
        s += std::min<size_t>(p.distinctPitches, 12) * 0.25;  // 音高丰富度
        int range = p.maxPitch - p.minPitch;
        s += (range >= 12) ? 1.0 : range / 12.0;              // 音域
        if (p.lyricAligned) s += 4.0;
        if (p.track == builtinLyricTrack) s += 5.0;   // 文件自己标的人声轨，最强证据
        if (p.avgPitch < 48) s -= 1.5;                        // 低音区多半是贝斯
        p.score = s;
        out.push_back(p);
    }
    return out;
}

int pickMelodyTrack(const MidiData& data, const std::vector<LyricLine>* lyrics) {
    int best = -1;
    double bestScore = -1e9;
    for (const auto& p : profileTracks(data, lyrics)) {
        if (p.score > bestScore) { bestScore = p.score; best = p.track; }
    }
    return best;
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

// ---- 和弦检测 ----
//
// 粒度是一个「和声窗」，不是一整小节。
//
// 早先按小节汇总伴奏音，一小节只出一个和弦。真实流行歌最常见的写法恰恰是
// 一小节两个和弦（`| C  Am |`、`| F  G |`），按小节汇总会把后半小节的和声
// 整个吞掉；更糟的是整小节混池之后，「和弦各音都在场」的判定会把 C 与 Am
// 的音凑成 Cmaj7 之类的假和弦——谱面上是错的，而且看不出来错。
//
// 窗口取法跟着拍号走：4/4 取半小节、6/8 取附点四分、3/4 取整小节。
// 相邻窗口同名就合并，所以和声不变的地方不会多出标记。
static long chordWindowFor(const TimeSignature& sig, int division) {
    double barLen = sig.barLengthTicks(division);
    if (!(barLen > 0)) barLen = division * 4.0;
    long barLenI = std::max<long>(1, (long)std::llround(barLen));
    int beats = sig.numerator > 0 ? sig.numerator : 4;
    // 偶数拍（4/4、6/8、12/8）折半：和弦常在第三拍或第二个附点四分处换。
    // 奇数拍（3/4、3/8）本来就是一小节一个和声，再细分只会放大噪声。
    int k = (beats % 2 == 0) ? 2 : 1;
    return std::max<long>(1, barLenI / k);
}

long chordWindowTicks(const MidiData& data) {
    return chordWindowFor(data.timeSig, data.division);
}

std::vector<ChordMark> detectChords(const MidiData& data, const KeyEstimate& key) {
    // 和弦根名用绝对音名，但等音写法要跟「当前小节的调」走：转进降号调后
    // 该写 Bb 而不是 A#。无转调时 KeyMap 只有一个点，与旧行为完全一致。
    const KeyMap keys(data, key);
    std::vector<ChordMark> chords;
    if (data.division <= 0 || data.notes.empty()) return chords;

    // 旋律轨不参与和声判定：它自带经过音与装饰音，会把和弦算歪
    int mainTrack = pickMelodyTrack(data, nullptr);
    const long win = chordWindowTicks(data);
    const BarGrid grid = barGridOf(data);

    // 和声窗的起点要跟着小节网格走。弱起曲目若还按 `tick / win` 切窗，
    // 每个窗都错开半拍，小节开头的和弦会被算到上一个窗里，
    // 谱面上第一个和弦就往左挪了一拍。变拍号曲目还要按该小节的拍号取窗长，
    // 否则 3/4 段的和弦会被 4/4 的半小节窗切碎。
    auto windowOf = [&](long tick) -> long {
        const long bar = grid.barIndex(tick);
        if (grid.pickup > 0 && bar == 0) return 0;    // 弱起整段算一个窗
        const long bs = grid.barStart(bar);
        const long w = grid.hasMeterChange()
                     ? chordWindowFor(grid.sigForBar(bar), data.division) : win;
        long local = tick - bs;
        if (local < 0) local = 0;
        return bs + (local / w) * w;
    };

    std::map<long, std::map<int, int>> winPitch;  // 窗口起点 -> 音级 -> 次数
    for (const auto& note : data.notes) {
        if (note.track == mainTrack) continue;
        winPitch[windowOf(note.startTicks)][note.pitch % 12]++;
    }

    struct ChordType { const char* suffix; std::vector<int> intervals; };
    const std::vector<ChordType> chordTypes = {
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

    static const char* NOTE_NAMES_SHARP[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    static const char* NOTE_NAMES_FLAT[12] = {
        "C", "Db", "D", "Eb", "E", "F",
        "Gb", "G", "Ab", "A", "Bb", "B"
    };

    // 单个窗口里最可能的和弦；证据不足时返回空串
    auto bestChordOf = [&](const std::map<int, int>& pitchCount,
                           const char* const* names) -> std::string {
        if (pitchCount.size() < 2) return std::string();   // 只有一个音级不构成和弦

        std::string best;
        int bestScore = 0;
        for (const auto& kv : pitchCount) {
            const int root = kv.first;
            for (const auto& ct : chordTypes) {
                int score = 0;
                bool allPresent = true;
                for (int interval : ct.intervals) {
                    auto f = pitchCount.find((root + interval) % 12);
                    if (f == pitchCount.end()) { allPresent = false; break; }
                    score += f->second;
                }
                if (allPresent && score > bestScore) {
                    bestScore = score;
                    best = names[root] + std::string(ct.suffix);
                }
            }
        }
        return bestScore >= 3 ? best : std::string();
    };

    // 相邻同名合并。空窗口不改 prevName，所以「C · 空 · C」不会标两次。
    std::string prevName;
    for (const auto& kv : winPitch) {
        const char* const* names = prefersFlats(keys.at(kv.first))
                                 ? NOTE_NAMES_FLAT : NOTE_NAMES_SHARP;
        std::string name = bestChordOf(kv.second, names);
        if (name.empty() || name == prevName) continue;
        chords.push_back({kv.first, name});
        prevName = name;
    }
    return chords;
}

// UTF-8 按「字」拆开：中文三字节、英文一字节都算一个音节。
// 按字节切会把一个汉字拆成三个音节，歌词数直接翻三倍。
static std::vector<std::string> splitUtf8(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;      // 截断的多字节序列，丢掉比拆碎好
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

LyricAssignment assignLyrics(const MidiData& data,
                             const std::vector<LyricLine>& lines,
                             int mainTrack) {
    LyricAssignment out;
    if (mainTrack < 0 || lines.empty()) return out;

    // 只统计旋律轨的音头。伴奏的和弦音也在同一 tick 上，混进来会让「一字
    // 多音」的组数翻倍——柱式伴奏三个音就是三个 onset（去重后虽仍是一个，
    // 但换轨会让歌词挂到错误的声部上）。
    std::vector<long> onsets;
    long songEnd = 0;
    for (const auto& n : data.notes) {
        if (n.track != mainTrack) continue;
        onsets.push_back(n.startTicks);
        songEnd = std::max(songEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    }
    if (onsets.empty()) return out;
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());

    for (size_t li = 0; li < lines.size(); li++) {
        const long from = lines[li].startTick;
        // 最后一行的右端取「全曲最后一个音之后」，保证末字也能覆盖到尾音
        const long to = (li + 1 < lines.size()) ? lines[li + 1].startTick : songEnd + 1;
        std::vector<long> inLine;
        for (long t : onsets) if (t >= from && t < to) inLine.push_back(t);

        const auto syls = splitUtf8(lines[li].text);
        const size_t n = inLine.size(), m = syls.size();
        if (n == 0 || m == 0) continue;

        // 第 i 个字覆盖的音符区间是 [startOf(i), startOf(i+1))。
        // n > m 时按 floor(i*n/m) 均匀分布，让一行词铺满自己的时间跨度；
        // 早先「第 k 个音领第 k 个字」会把 7 个字在第一小节一口气用完。
        auto startOf = [&](size_t i) -> size_t {
            if (i >= m) return n;
            if (n > m) {
                size_t v = (size_t)((unsigned long long)i * n / m);
                return v < n ? v : n - 1;
            }
            return i < n ? i : n;
        };

        for (size_t i = 0; i < m; i++) {
            size_t a = startOf(i);
            if (a >= n) break;                  // 音不够，剩下的字没地方挂
            size_t b = startOf(i + 1);
            if (b <= a) b = a + 1;
            if (b > n) b = n;

            LyricPlacement pl;
            pl.text = syls[i];
            pl.tick = inLine[a];
            pl.noteCount = (int)(b - a);
            pl.lastNoteTick = inLine[b - 1];
            // 右端不能越过下一行歌词的起点，否则连音线会拖进下一句
            pl.spanEnd = (b < n) ? inLine[b] : to;
            if (pl.spanEnd <= pl.tick) pl.spanEnd = pl.tick + 1;

            out.at[pl.tick] = pl;
            if (pl.noteCount >= 2) out.melismas.push_back(pl);
        }
    }
    return out;
}

// ---- MIDI 内置歌词（0x05 lyric meta） ----

// 拆成「可唱单位」：中文一个汉字、英文一个连续单词。
// assignLyrics 用的 splitUtf8 是按字节序列逐字拆的，套到英文歌词上会把
// "Hello" 拆成 5 个音节；英文 .mid 里一个单词通常就是一个 lyric 事件。
static std::vector<std::string> splitMidiSyllables(const std::string& s) {
    std::vector<std::string> out;
    auto isWordChar = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '\'';
    };
    static const std::set<std::string> kPunct = {
        "，", "。", "！", "？", "、", "；", "：", "“", "”", "‘", "’",
        "（", "）", "【", "】", "…", "—", "～", "·", "《", "》", "「", "」"
    };
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (isWordChar(c)) {
                size_t j = i;
                while (j < s.size() && isWordChar((unsigned char)s[j])) j++;
                out.push_back(s.substr(i, j - i));
                i = j;
            } else {
                i++;   // 空格、ASCII 标点、'-' '_' '/' 一律不占音节
            }
            continue;
        }
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;        // 截断的多字节序列，丢掉比拆碎好
        std::string u = s.substr(i, len);
        if (!kPunct.count(u)) out.push_back(u);
        i += len;
    }
    return out;
}

std::vector<MidiSyllable> normalizeMidiLyrics(const MidiData& data) {
    std::vector<MidiSyllable> out;
    if (data.midiLyrics.empty()) return out;

    std::vector<const LyricEvent*> evs;
    evs.reserve(data.midiLyrics.size());
    for (const auto& e : data.midiLyrics) evs.push_back(&e);
    // 稳定排序：同一 tick 上多个事件的先后由文件顺序决定，不能打乱歌词次序
    std::stable_sort(evs.begin(), evs.end(),
                     [](const LyricEvent* a, const LyricEvent* b) { return a->tick < b->tick; });

    struct Batch { long tick; std::vector<std::string> syls; };
    std::vector<Batch> batches;
    for (const auto* e : evs) {
        std::string text;
        for (char ch : e->text) {
            if (ch != '\r' && ch != '\n') text.push_back(ch);
        }
        // trim 首尾空白（"Hello " / " 你" 都很常见）
        size_t b = 0, z = text.size();
        while (b < z && (text[b] == ' ' || text[b] == '\t')) b++;
        while (z > b && (text[z - 1] == ' ' || text[z - 1] == '\t')) z--;
        text = text.substr(b, z - b);

        if (text.empty() || text[0] == '@') continue;   // @KMIDI / @TITLE 等元数据
        if (text[0] == '+') {                           // 续行标记
            text.erase(0, 1);
            if (text.empty()) continue;
        }
        // "-" / "_" / "/" / "\" 组成的事件是延续或换行，不产生新音节。
        // 跳过之后，前一个字的 spanEnd 自然延长到下一个有效事件 —— 这正是
        // 简谱里「一个字拖过几个音」要画连音线的地方。
        bool onlyMarks = true;
        for (char ch : text) {
            if (ch != '-' && ch != '_' && ch != '/' && ch != '\\' &&
                ch != ' ' && ch != '\t') { onlyMarks = false; break; }
        }
        if (onlyMarks) continue;

        auto syls = splitMidiSyllables(text);
        if (syls.empty()) continue;
        if (!batches.empty() && batches.back().tick == e->tick) {
            auto& last = batches.back().syls;
            last.insert(last.end(), syls.begin(), syls.end());
        } else {
            batches.push_back({e->tick, syls});
        }
    }

    for (size_t i = 0; i < batches.size(); i++) {
        // 末批 spanEnd = 0，由 assignMidiLyrics 填「曲终之后」
        const long right = (i + 1 < batches.size()) ? batches[i + 1].tick : 0;
        for (const auto& s : batches[i].syls)
            out.push_back({batches[i].tick, s, right});
    }
    return out;
}

LyricAssignment assignMidiLyrics(const MidiData& data, int mainTrack) {
    LyricAssignment out;
    const std::vector<MidiSyllable> syls = normalizeMidiLyrics(data);
    if (mainTrack < 0 || syls.empty()) return out;

    std::vector<long> onsets;
    long songEnd = 0;
    for (const auto& n : data.notes) {
        if (n.track != mainTrack) continue;
        onsets.push_back(n.startTicks);
        songEnd = std::max(songEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    }
    if (onsets.empty()) return out;
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());

    // 同 tick 的几个音节是一批（一个事件里写了多个字），共享覆盖区间
    for (size_t i = 0; i < syls.size();) {
        size_t j = i;
        while (j < syls.size() && syls[j].tick == syls[i].tick) j++;
        const long from = syls[i].tick;
        const long to = (syls[i].spanEnd > from) ? syls[i].spanEnd : songEnd + 1;
        const size_t m = j - i;

        std::vector<long> inLine;
        for (long t : onsets) if (t >= from && t < to) inLine.push_back(t);
        if (inLine.empty()) {
            // 歌词事件偶尔写在音符前一两 tick，或挤在末音之后。宁可挂到
            // 最近的下一个音头，也不要整段歌词凭空消失。
            auto it = std::lower_bound(onsets.begin(), onsets.end(), from);
            if (it != onsets.end()) inLine.push_back(*it);
        }
        if (!inLine.empty()) {
            const size_t n = inLine.size();
            auto startOf = [&](size_t k) -> size_t {
                if (k >= m) return n;
                if (m == 1) return 0;          // 单音节：整段都归它唱
                size_t v = (size_t)((unsigned long long)k * n / m);
                return v < n ? v : n - 1;
            };
            for (size_t k = 0; k < m; k++) {
                size_t a = startOf(k);
                if (a >= n) break;
                size_t b = startOf(k + 1);
                if (b <= a) b = a + 1;
                if (b > n) b = n;

                LyricPlacement pl;
                pl.text = syls[i + k].text;
                pl.tick = inLine[a];
                pl.noteCount = (int)(b - a);
                pl.lastNoteTick = inLine[b - 1];
                pl.spanEnd = (b < n) ? inLine[b] : to;
                if (pl.spanEnd <= pl.tick) pl.spanEnd = pl.tick + 1;
                if (out.at.count(pl.tick)) continue;   // 同音头不重复挂词
                out.at[pl.tick] = pl;
                if (pl.noteCount >= 2) out.melismas.push_back(pl);
            }
        }
        i = j;
    }
    return out;
}

int midiLyricTrack(const MidiData& data) {
    // 只有元数据 / 延续符的文件（例如拿 0x05 存段落标记）没有可唱音节，
    // 这种「歌词轨」不能拿来给旋律轨加分。
    if (normalizeMidiLyrics(data).empty()) return -1;
    std::set<int> noteTracks;
    for (const auto& n : data.notes) noteTracks.insert(n.track);
    // format 0 只有一条轨，歌词轨号不携带任何信息
    if (noteTracks.size() < 2) return -1;
    const int t = data.midiLyrics.front().track;
    if (t < 0 || !noteTracks.count(t)) return -1;
    for (const auto& e : data.midiLyrics) if (e.track != t) return -1;
    return t;
}

LyricSource lyricSourceOf(const MidiData& data, const std::vector<LyricLine>* external) {
    if (external && !external->empty()) return LyricSource::External;
    if (!normalizeMidiLyrics(data).empty()) return LyricSource::Midi;
    return LyricSource::None;
}

std::string lyricSourceLabel(const MidiData& data,
                             const std::vector<LyricLine>* external) {
    switch (lyricSourceOf(data, external)) {
        case LyricSource::External: return "歌词：外部 LRC";
        case LyricSource::Midi:
            return "歌词：MIDI 内置（" +
                   std::to_string(normalizeMidiLyrics(data).size()) + " 音节）";
        default: return std::string();
    }
}

LyricAssignment resolveLyrics(const MidiData& data,
                              const std::vector<LyricLine>* external,
                              int mainTrack) {
    if (external && !external->empty()) return assignLyrics(data, *external, mainTrack);
    return assignMidiLyrics(data, mainTrack);
}

} // namespace m2j

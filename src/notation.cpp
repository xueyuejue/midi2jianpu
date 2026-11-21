// notation.cpp - 调性识别与简谱转换实现
#include "midi2jianpu/notation.hpp"

#include "text_encoding.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace m2j {

static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

// 自然大调/小调各级相对主音的半音距离（7个音级）
static const std::array<int, 7> MAJOR = {0, 2, 4, 5, 7, 9, 11};
static const std::array<int, 7> MINOR = {0, 2, 3, 5, 7, 8, 10};

std::string midiNoteName(int pitch) {
    // pitch 在合法 MIDI 范围（0~127）内时 pc 恒非负；负值来自移调折回前
    // 的极端输入或未来的音高改写路径，归一后 (pitch - pc) 是 12 的整倍数，
    // 除法不再受「向零截断」影响（-1/12 = 0 会把 B-2 写成 B-1）。
    int pc = pitch % 12;
    if (pc < 0) pc += 12;
    std::ostringstream os;
    os << NOTE_NAMES[pc] << ((pitch - pc) / 12 - 1);
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
        // 文件里写的升降数就是权威：-6 就是六个降号（Gb），不能因为
        // 「F# 也只要六个升号」就翻到升号侧去。非法值夹到标准范围。
        r.fifthsHint = sf < -7 ? -7 : (sf > 7 ? 7 : sf);
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
        // 移调折回后 pitch 已在 0~127，这里按音级取模仍是防御：负 pitch
        // 的 % 12 是负下标，对栈上数组是越界写，宁可多一次归一。
        int wPc = note.pitch % 12;
        if (wPc < 0) wPc += 12;
        pitchWeights[wPc] += w;
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

// ---- 移调 ----

// 调号升降数随移调平移。
//
// 音高移 N 个半音，等价于在五度圈上走 7N 步（纯五度 = 7 半音，且
// 7 x 7 ≡ 1 (mod 12)，所以「7N mod 12」就是所需步数）。步数取 [-5, 6]
// 中最短的等价写法，跨过零点的移调才不会写出 12 个升号的怪调：
//   C(0) +2 -> +2  D       C(0) +1 -> -5  Db（而不是 7 个升号的 C#）
//   C(0) -2 -> -2  Bb      C(0) +7 -> +1  G（纯五度只差一个升号）
// 结果再归一化到 [-6, 6]：C# 大调(7) +6 会算出 13，等价写法 Db 只差 5 个降号。
//
// 注意「已在 [-6,6] 内就原样返回」这一步不能省：-6 与 +6 在模 12 下同余，
// 先取模再折半会把它变成 +6，六个降号的 Gb 大调就被悄悄写成了六个升号。
// 只有真正越界的值（12、13、-7、-11……）才需要折。
static int normalizeFifths12(int f) {
    if (f >= -6 && f <= 6) return f;
    int m = ((f % 12) + 12) % 12;
    if (m > 6) m -= 12;
    return m;
}

int transposeFifths(int fifths, int semitones) {
    const int n = ((semitones % 12) + 12) % 12;
    int d = (7 * n) % 12;          // 0..11
    if (d > 6) d -= 12;            // -5..6：少写临时记号的等价写法
    return normalizeFifths12(fifths + d);
}

int transposeData(MidiData& data, int semitones) {
    if (semitones == 0) return 0;
    // ±60 半音（v1.96 设的 CLI 上限）内，极端移调仍可能把音符推出 MIDI
    // 音域：30 + (-60) = -30，120 + 60 = 180。旧实现放任越界值流进下游，
    // 两个后果：estimateKey 的 pitchWeights[pitch % 12] 用负下标越界写栈
    // （内存安全），MusicXML 的 <octave> 写出 12 或 -5（越过规范 0~9，
    // 记谱软件可能拒绝）。按整八度折回：音级不变，调性 / 和弦 / 唱名等
    // 一切按音级算的链路都不受影响，页眉如实报告折回数。
    int folded = 0;
    for (auto& n : data.notes) {
        n.pitch += semitones;
        if (n.pitch > 127 || n.pitch < 0) {
            while (n.pitch > 127) n.pitch -= 12;
            while (n.pitch < 0) n.pitch += 12;
            folded++;
        }
    }
    // 调号必须跟着走：estimateKey() 优先采信 MIDI 内置调号，只移音不移调号
    // 会让 --transpose +2 输出的谱面「音符在 D 大调、调号还是 1=C」，
    // 简谱唱名全变成 2 3 #4，MusicXML 的 <fifths> 也留在 0，MuseScore 里
    // 整首曲子挂满临时记号。
    if (data.keySig.valid)
        data.keySig.sharpsFlats = transposeFifths(data.keySig.sharpsFlats, semitones);
    for (auto& p : data.keySigChanges)
        p.sig.sharpsFlats = transposeFifths(p.sig.sharpsFlats, semitones);
    return folded;
}

KeyEstimate transposeKey(const KeyEstimate& key, int semitones) {
    if (semitones == 0) return key;
    // 走调号而不是直接 tonic+半音：后者会把 C+3 写成「D# 大调」（9 个升号），
    // 走五度圈才能落到「Eb 大调」（3 个降号）这种真正好读的写法。
    const int sf = transposeFifths(keyFifths(key), semitones);
    const int majorTonic = ((7 * sf) % 12 + 12) % 12;
    KeyEstimate r = key;
    r.tonicPitchClass = key.isMajor ? majorTonic : (majorTonic - 3 + 12) % 12;
    // 移调后的调号是算出来的确定值，直接当 hint 记下来：否则 Gb(-6) 移一个
    // 八度回到同一个音级，又会退化成查表的 +6。
    r.fifthsHint = sf;
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

static std::string makeScaleLabel(int gong, int finalPc, PentatonicMode mode,
                                  int fifthsHint = kFifthsAuto) {
    KeyEstimate k;
    k.tonicPitchClass = ((gong % 12) + 12) % 12;
    k.isMajor = true;
    // 宫音音级 6 有 Gb 与 F# 两种写法，标注里的升降号必须跟调用方一致，
    // 否则会出现「宫音 Gb」但页眉另一处写 F# 的自相矛盾。
    k.fifthsHint = fifthsHint;
    const bool flat = keyFifths(k) < 0;
    std::ostringstream os;
    os << pitchClassName(finalPc >= 0 ? finalPc : gong, flat)
       << " " << pentatonicModeName(mode) << "调式（宫音 "
       << pitchClassName(gong, flat) << "）";
    return os.str();
}

// 相对宫音的半音数 -> 传统七声里的偏音名。
//
// 只认四个有固定写法的偏音：清角(5)、变徵(6)、闰(10)、变宫(11)。
// 差半音的关系（1 / 3 / 8）在十二律里没有名字——那种情形基本可以断定是
// 宫音选错了半音，硬给个名字反而像在替用户圆场。
static const char* pentatonicBiasName(int rel) {
    switch (rel) {
        case 5:  return "清角";
        case 6:  return "变徵";
        case 10: return "闰";
        case 11: return "变宫";
        default: return nullptr;
    }
}

// --gong 是用户硬指定的宫音，曲子却未必落在这个宫系统上。
//
// pentatonic.mid 的落音是 G；指定 --gong D 时 G 是清角，不在 D 宫五声里。
// 旧实现照旧写成「G 宫调式（宫音 D）」——宫调式的主音必须就是宫音，
// 这不是措辞难看的问题，是两个互相打架的断言：真按宫调式唱，落音该是 D。
// 12 个宫音里有 7 个会撞出这种标签。所以落音是偏音时不再给调式名，
// 直接说清「落音是哪个偏音、不在五声内」，由用户判断是宫音选错了还是
// 这首本来就不是五声。
static std::string makeGongMismatchLabel(int gong, int finalPc, int fifthsHint) {
    KeyEstimate k;
    k.tonicPitchClass = ((gong % 12) + 12) % 12;
    k.isMajor = true;
    k.fifthsHint = fifthsHint;
    const bool flat = keyFifths(k) < 0;
    const int rel = ((finalPc - gong) % 12 + 12) % 12;
    std::ostringstream os;
    os << "宫音 " << pitchClassName(gong, flat) << "（落音 "
       << pitchClassName(finalPc, flat);
    if (const char* bias = pentatonicBiasName(rel))
        os << " 是" << bias;
    os << "，不在五声内）";
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

KeyEstimate keyFromGong(const MidiData& data, int gongPitchClass, int fifthsHint) {
    KeyEstimate k;
    k.tonicPitchClass = ((gongPitchClass % 12) + 12) % 12;
    k.isMajor = true;
    // 宫音本身没有升降号信息，--gong Gb 与 --gong F# 传进来都是音级 6；
    // 由调用方（parseKey 认得 'b' 后缀）把降号侧传下来，否则五声调式
    // 也会把 Gb 宫写成 F# 宫。
    k.fifthsHint = fifthsHint;
    const PitchProfile prof = melodyPitchProfile(data);
    PentatonicMode mode = PentatonicMode::Gong;
    int finalPc = -1;
    // 落音是正音才敢报调式名。没有音符（finalPc 拿不到）时保持宫调式：
    // 那不是「不匹配」，只是没有证据，写「宫音 X（…不在五声内）」反而像报错。
    bool matched = true;
    if (prof.finalPc >= 0) {
        finalPc = prof.finalPc;
        const PentatonicMode m = modeOfFinal(k.tonicPitchClass, finalPc);
        if (m != PentatonicMode::None) mode = m;
        else matched = false;
    }
    k.scaleLabel = matched
                       ? makeScaleLabel(k.tonicPitchClass, finalPc, mode, fifthsHint)
                       : makeGongMismatchLabel(k.tonicPitchClass, finalPc, fifthsHint);
    return k;
}

// 大调主音音级 -> 调号升降数（取绝对值最小的等音写法）
// C=0, C#/Db=-5, D=2, D#/Eb=-3, E=4, F=-1, F#/Gb=6, G=1, G#/Ab=-4, A=3, A#/Bb=-2, B=5
static const int MAJOR_FIFTHS[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};

int keyFifths(const KeyEstimate& key) {
    // 有权威来源（MIDI 调号 / --key 的升降写法 / 转调点）就直接采信：
    // 音级查表分不出 Gb 大调与 F# 大调，只能挑较短的一侧。
    if (key.fifthsHint != kFifthsAuto) return key.fifthsHint;
    int pc = key.tonicPitchClass;
    if (key.isMajor) return MAJOR_FIFTHS[pc];
    // 关系小调的主音比关系大调高小三度
    return MAJOR_FIFTHS[(pc + 3) % 12];
}

int keyFifthsOfFlatSpelling(int pitchClass) {
    const int pc = ((pitchClass % 12) + 12) % 12;
    // 降号写法里只有 Gb 与升号侧打平（6 降 vs 6 升），而表里默认给了 +6；
    // 其余降号（Db -5 / Eb -3 / Ab -4 / Bb -2）本来就是表里那个更短的写法。
    return pc == 6 ? -6 : MAJOR_FIFTHS[pc];
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

// 文本简谱记法（与 HTML 版的视觉记法一一对应）：
//   延长线 "-"  每多一拍加一条
//   下划线 "_" 表示八分，"=" 表示十六分，"%%" 表示三十二分
//   附点在数字右侧用 "." 表示
struct DurEntry { double beats; const char* suffix; };
static const DurEntry DUR_TABLE[] = {
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

// 谱面「写不出来」的空档门槛：不足半拍（八分休止）的空白一律不记休止符。
// 文本谱与 HTML 都按这个门槛补休止符，时值量化也必须用同一个值把音符
// 补到下一个音头（见 quantizeTiming），否则那段时间会被两边各吃一次：
// 谱面少拍，MusicXML 却单独补出 16 分休止。
long restGateTicks(int division) {
    return division > 0 ? std::max<long>(1, division / 2) : 1;
}

std::string durationSuffix(long durationTicks, int division) {
    if (division <= 0) return "";
    double beats = (double)durationTicks / division;

    if (beats > 4.5) {  // 超长音符退化为延长线计数
        int n = (int)std::lround(beats) - 1;
        return std::string(n > 0 ? n : 0, '-');
    }

    const char* best = "";
    double bestDist = 1e9;
    for (const auto& d : DUR_TABLE) {
        double dist = std::abs(beats - d.beats);
        if (dist < bestDist) { bestDist = dist; best = d.suffix; }
    }
    return best;
}

// durationSuffix() 会把表外时值吸附到最近档，落在两档之间时可能向上取整。
// 这里给出「按最近档折算后的实际 tick 数」，口径必须与 durationSuffix 一致，
// 供记谱出口判断有没有发生上取整（原来的 3.75 拍被记成 4 拍就是这么来的）。
static long nearestNotatableTicks(long durationTicks, int division) {
    if (division <= 0) return durationTicks;
    double beats = (double)durationTicks / division;
    if (beats > 4.5)                       // 与上面的「退化为延长线」同口径
        return (long)std::lround(beats) * division;
    double bestDist = 1e9;
    long best = durationTicks;
    for (const auto& d : DUR_TABLE) {
        double dist = std::abs(beats - d.beats);
        if (dist < bestDist) {
            bestDist = dist;
            best = (long)std::lround(d.beats * division);
        }
    }
    return best;
}

// 不超过 ticks 的最大可记谱时值，用来把超长时值拆成多段「宁短不长」的记法。
static long largestNotatableTicks(long ticks, int division) {
    if (division <= 0) return ticks;
    long best = 0;
    for (const auto& d : DUR_TABLE) {
        long len = (long)std::lround(d.beats * division);
        if (len > 0 && len <= ticks && len > best) best = len;
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
    // 长于全音符：这张表是「一个音符符号能表示的时值」，到这里就装不下了。
    //
    // 旧实现在这里 return 4.0，等于把一个 6 拍的长音截成 4 拍。音符的
    // durationTicks 被就地改小，后面的时间就成了空洞，三个输出层只好在
    // 下一小节补一个休止符——`| 3 - - - | 0 - 4 5 |`，唱到一半凭空静音两拍。
    //
    // 长音本来就该靠延音线跨小节分写（文本/HTML 的 `-`、MusicXML 的
    // <tie>），切分逻辑早就写好了。这里只把拍数吸附到 1/4 拍，时值必须保住。
    if (beats >= NOTATABLE_BEATS[n - 1]) return std::round(beats * 4.0) / 4.0;
    double best = NOTATABLE_BEATS[0];
    double bestDist = 1e9;
    for (int i = 0; i < n; i++) {
        double d = std::abs(beats - NOTATABLE_BEATS[i]);
        // 距离相同时取较短的那个：宁可写成八分+休止，也别硬拉成附点四分
        if (d < bestDist - 1e-9) { bestDist = d; best = NOTATABLE_BEATS[i]; }
    }
    return best;
}

// 把同轨音符拆成互不重叠的「声部」（lane）。口径与 splitVoices() 一致：
// 同拍同值算和弦共用一个 lane，其余优先复用已经收尾的 lane。
// 第一遍估门限因子与第二遍补空档都用它，保证两边看到的是同一套声部。
struct LaneLayout {
    std::vector<int> laneOf;                    // 按 data.notes 的全局索引
    std::vector<long> laneNext;                // 本声部的下一个音头（无则 0）
    std::vector<std::vector<long>> laneOnsets; // 按 lane 编号
};

static LaneLayout buildLaneLayout(const MidiData& data,
                                  const std::vector<size_t>& idxs) {
    struct Lane { long start = -1; long end = -1; };
    LaneLayout out;
    out.laneOf.assign(data.notes.size(), 0);
    out.laneNext.assign(data.notes.size(), 0);
    std::vector<Lane> lanes;
    for (size_t i : idxs) {
        const long s = data.notes[i].startTicks;
        const long dd = std::max<long>(data.notes[i].durationTicks, 1);
        int chosen = -1;
        for (size_t v = 0; v < lanes.size(); v++)
            if (lanes[v].start == s && lanes[v].end - lanes[v].start == dd) {
                chosen = (int)v; break;
            }
        if (chosen < 0)
            for (size_t v = 0; v < lanes.size(); v++)
                if (lanes[v].end <= s) { chosen = (int)v; break; }
        if (chosen < 0) { lanes.push_back(Lane()); chosen = (int)lanes.size() - 1; }
        out.laneOf[i] = chosen;
        lanes[chosen].start = s;
        lanes[chosen].end = std::max(lanes[chosen].end, s + dd);
    }
    out.laneOnsets.resize(lanes.size());
    for (size_t i : idxs) {
        auto& o = out.laneOnsets[out.laneOf[i]];
        if (o.empty() || o.back() != data.notes[i].startTicks)
            o.push_back(data.notes[i].startTicks);
    }
    for (size_t i : idxs) {
        const auto& o = out.laneOnsets[out.laneOf[i]];
        const long s = data.notes[i].startTicks;
        for (size_t k = 0; k + 1 < o.size(); k++)
            if (o[k] == s) { out.laneNext[i] = o[k + 1]; break; }
    }
    return out;
}

// 按 0.02 分桶取众数估门限因子；样本不足或众数不显著时返回 1.0（不还原）。
static double estimateGate(const std::vector<double>& ratios) {
    if (ratios.size() < 8) return 1.0;
    std::map<int, int> hist;
    for (double r : ratios) hist[(int)std::lround(r * 50)]++;
    int bestBin = -1, bestCnt = 0;
    for (auto& kv : hist) if (kv.second > bestCnt) { bestCnt = kv.second; bestBin = kv.first; }
    if (bestCnt * 4 < (int)ratios.size()) return 1.0;   // 没有统一门限
    double gate = bestBin / 50.0;
    if (gate < 0.45 || gate > 1.0) return 1.0;
    return gate;
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
    // 整轨口径（旧行为，逐字节保留）之外，额外记下「被别的声部音头挡住」的音。
    // 复调轨里别的声部的音头会插进本声部两个音头之间，整轨 ioi 被切短，
    // 本声部的门限音于是算出 dur/ioi > 1 而被当作无效样本丢掉；更糟的是
    // 连奏声部（dur == ioi，r = 1.0）会把众数锁死在 1.0，门限彻底估不出来。
    // 例如钢琴左手八分连奏 + 右手 0.7 门限四分音符：整轨音头每 240 tick，
    // 右手 336/240 = 1.4 被丢、左手 240/240 = 1.0 灌满样本，gate 落到 1.0，
    // 右手 12 个四分音符全被写成附点八分，MusicXML 补出 12 个 16 分休止。
    // 修法不改全局 gate：只对这批被误丢的音，另按本声部口径估一个 laneGate
    // 并单独还原。其余音符仍走原来的 gate，既有素材因此逐字节不变。
    std::vector<double> ratios, laneRatios;
    std::vector<char> laneOnlyNotes(data.notes.size(), 0);
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
        std::map<long, long> wholeNext;
        for (size_t k = 0; k + 1 < onsets.size(); k++) wholeNext[onsets[k]] = onsets[k + 1];
        const LaneLayout lay = buildLaneLayout(data, idxs);
        for (size_t i : idxs) {
            const long s = data.notes[i].startTicks;
            const long ln = lay.laneNext[i];
            if (ln <= s) continue;
            auto it = wholeNext.find(s);
            const long wn = (it == wholeNext.end()) ? 0 : it->second;
            if (wn > s && wn < ln) {
                laneOnlyNotes[i] = 1;
                // 连奏音（r 接近 1）不携带门限信息，不能参与门限众数。
                double r = (double)data.notes[i].durationTicks / (ln - s);
                if (r > 0.2 && r < 0.98) laneRatios.push_back(r);
            }
        }
    }
    const double gate = estimateGate(ratios);
    const double laneGate = estimateGate(laneRatios);
    // 只有当本声部口径确实估出「更短、更真实」的门限时才启用这条支路。
    const bool laneEffective = laneGate < gate - 0.02;
    rep.gate = gate;
    rep.laneGate = laneEffective ? laneGate : 1.0;

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
        // 本声部（lane）内的下一个音头：复调轨里不能用整轨 onsets 当本音的
        // 收尾，否则左手按住的长音会被右手的音头截断（见 v1.55.0）。
        const LaneLayout lay = buildLaneLayout(data, idxs);

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
                // 门限还原只对「断奏音」成立：门限因子来自 dur/ioi 的众数，
                // 描述的是音符提前收尾、留出气口的那种演奏习惯。
                // 长音（附点二分及以上）是按住不放的，note-off 就是真实收尾，
                // 再按 1/0.86 放大只会让它虚增 16%（4 拍 → 4.65 拍）。
                // 单声部轨还能靠「响到下一音头」兜回来，柱式和弦的钢琴轨
                // （复调，monophonic=false）没有这层兜底，虚增会真的落进谱面：
                // 一个 4 拍的和弦被写成 4 拍 + 0.75 拍延音，整段小节线错位。
                const bool heldNote = oldDur >= 3L * div;
                const long laneIoi = (lay.laneNext[i] > s) ? lay.laneNext[i] - s : 0;
                double est;
                if (heldNote) {
                    est = (double)oldDur;
                } else if (laneEffective && laneOnlyNotes[i]) {
                    // 本声部口径还原必须有「本声部下一个音头」作参照：末音
                    // 没有参照，除一次 gate 会把 480 tick 虚增成 720、谱面
                    // 凭空多一小节；连奏音与长于间隔的复调音同理都不还原。
                    const bool eligible = laneIoi > 0 && oldDur < laneIoi &&
                                         oldDur >= 0.45 * laneIoi;
                    est = eligible ? (double)oldDur / laneGate : (double)oldDur;
                } else {
                    est = (double)oldDur / gate;   // 旧行为，逐字节保留
                }

                long dur;
                if (monophonic && ioi > 0 && est >= 0.9 * ioi) {
                    // 连奏：音符一直响到下一个音头
                    dur = ioi;
                } else {
                    dur = (long)std::llround(snapBeats(est / div) * div);
                }
                if (monophonic && ioi > 0 && dur > ioi) dur = ioi;

                // 门限衰减留下的空档要并进前一个音。
                // 真实演奏的 note-off 普遍早于下一个音头（DAW 门限 85% 是常见
                // 默认值），谱面上不足半拍的空档写不出休止符、一律省略，于是
                // 那段时间必须由音符本身占掉：400/480 的四分音符旋律，一小节
                // 4 拍会被记成「附点八分 + 看不见的十六分休止」，文本谱只剩 3
                // 拍，而 MusicXML 另走一条路补出真的 16 分休止——三端就此分叉。
                // v1.55.0：空档按「本声部下一个音头」算（laneNext），不再被整轨
                // `monophonic` 判定一刀切。补到的是同声部的下一个音头，绝不会截断别的
                // 声部的长音；连奏判定与 dur > ioi 的截短仍走整轨口径，保持既有
                // 复调素材的排版不变。
                //
                // 再加一道相对门槛（空档不到音长三分之一，即门限 >= 3/4）：
                // 一半音长就收尾的那种是刻意的断奏 / 打击乐击打，120 tick 的
                // 鼓点占 240 tick 的格，拉成八分会把十六分音符写丢。
                const long nextInLane = lay.laneNext[i];
                const long gap = (nextInLane > s) ? nextInLane - s - oldDur : 0;
                if (gap > 0 && gap < restGateTicks(div) && gap * 3 <= oldDur &&
                    nextInLane - s > dur)
                    dur = nextInLane - s;

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

// CC11 / note velocity 的取值 -> 六档简谱力度记号。
// ExpressionMap 与三个输出层共用这一张表，避免 HTML 写一套、文本写一套。
static std::string dynamicLevel(int val) {
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

// 见头文件注释：声部主导通道是力度 / 楔形记号的唯一取用口径。
int dominantChannel(const std::vector<NoteEvent>& notes) {
    std::map<int, int> counts;
    for (const auto& n : notes) counts[n.channel]++;
    int best = -1, bestCount = 0;
    for (const auto& n : notes) {
        const int c = counts[n.channel];
        if (c > bestCount) { bestCount = c; best = n.channel; }
    }
    return best;
}

// 同轨复调拆分。见头文件注释：同拍同值算和弦，其余重叠音分到不同声部。
std::vector<int> splitVoices(std::vector<NoteEvent>& notes, int maxVoices,
                             const std::function<bool(const NoteEvent&)>& skip) {
    std::vector<int> voice(notes.size(), 0);
    if (notes.empty()) return voice;
    if (maxVoices < 1) maxVoices = 1;

    // 每个声部的「上一个音」状态：用它判断同拍和弦与是否已经空出来。
    struct Lane { long start = -1; long end = -1; };
    std::vector<Lane> lanes;

    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < notes.size(); i++) order[i] = i;
    // 先按音头排，同拍时高音在前 —— 声部号越小越靠上，谱面读起来才顺。
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (notes[a].startTicks != notes[b].startTicks)
            return notes[a].startTicks < notes[b].startTicks;
        return notes[a].pitch > notes[b].pitch;
    });

    for (size_t idx : order) {
        // 倚音不占独立声部：它只是主音前面的装饰，留在声部 0 里由输出层跳过。
        if (skip && skip(notes[idx])) { voice[idx] = 0; continue; }

        const long start = notes[idx].startTicks;
        const long dur = std::max<long>(notes[idx].durationTicks, 1);
        const long end = start + dur;
        int chosen = -1;

        // 1. 同拍同值：这是一个真和弦，几个音共用一格，写成 <chord/> 才对。
        for (size_t v = 0; v < lanes.size(); v++) {
            if (lanes[v].start == start && lanes[v].end - lanes[v].start == dur) {
                chosen = (int)v;
                break;
            }
        }
        // 2. 上一个音已经收尾的声部可以直接接上。
        if (chosen < 0) {
            for (size_t v = 0; v < lanes.size(); v++) {
                if (lanes[v].end <= start) { chosen = (int)v; break; }
            }
        }
        // 3. 都占着：开新声部；到上限就并进最早结束的那个。
        if (chosen < 0) {
            if ((int)lanes.size() < maxVoices) {
                lanes.push_back(Lane());
                chosen = (int)lanes.size() - 1;
            } else {
                size_t best = 0;
                for (size_t v = 1; v < lanes.size(); v++)
                    if (lanes[v].end < lanes[best].end) best = v;
                chosen = (int)best;
            }
        }

        voice[idx] = chosen;
        lanes[chosen].start = start;
        lanes[chosen].end = std::max(lanes[chosen].end, end);
    }

    // 合并进同一声部的溢出音可能和里面的音重叠。同声部一旦重叠，输出层就会
    // 把长音当和弦写、把小节撑爆，所以这里统一收口：
    //   1. 同拍不同值 -> 全部取最小值，变成时值一致的（真）和弦
    //   2. 还和后面的音头重叠 -> 截短到下一个音头
    // 同拍同值的正常和弦不受影响（这一步对它们是无操作）。
    std::map<int, std::vector<size_t>> byVoice;
    for (size_t i = 0; i < notes.size(); i++) byVoice[voice[i]].push_back(i);
    for (auto& kv : byVoice) {
        auto& v = kv.second;
        std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
            return notes[a].startTicks < notes[b].startTicks;
        });
        // 1. 同拍对齐时值
        for (size_t k = 0; k < v.size();) {
            size_t j = k;
            long minDur = LONG_MAX;
            while (j < v.size() && notes[v[j]].startTicks == notes[v[k]].startTicks) {
                if (!(skip && skip(notes[v[j]])))
                    minDur = std::min(minDur, std::max<long>(notes[v[j]].durationTicks, 1));
                j++;
            }
            if (minDur != LONG_MAX) {
                for (size_t m = k; m < j; m++)
                    if (!(skip && skip(notes[v[m]])) && notes[v[m]].durationTicks > minDur)
                        notes[v[m]].durationTicks = minDur;
            }
            k = j;
        }
        // 2. 截短到下一个（更晚的）音头
        for (size_t k = 0; k < v.size(); k++) {
            const size_t i = v[k];
            if (skip && skip(notes[i])) continue;   // 倚音保持原样
            long nextStart = LONG_MAX;
            for (size_t j = k + 1; j < v.size(); j++) {
                if (notes[v[j]].startTicks > notes[i].startTicks) {
                    nextStart = notes[v[j]].startTicks;
                    break;
                }
            }
            if (nextStart == LONG_MAX) continue;
            const long end = notes[i].startTicks + notes[i].durationTicks;
            if (end > nextStart)
                notes[i].durationTicks = std::max<long>(1, nextStart - notes[i].startTicks);
        }
    }
    return voice;
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

    // 边界表要铺到最后一个音之后再多一节，延长段才不会跨出表尾。
    // v1.89：与解析层共用同一个时间轴上限。只按 songEnd 铺表时，极小的
    // curLen 能把 horizon 里每一节压到 8 tick，撞上 20000 的硬保险就产出
    // 几十 MB 空谱；钳到 cap 之后两端口径一致，正常文件 songEnd 远小于 cap。
    long songEnd = 0;
    for (const auto& n : data.notes)
        songEnd = std::max(songEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    const long horizon = std::min(songEnd + bl, timelineCapTicks(data.division));

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
// 拍数的显示口径：接近整数就写整数，否则保留一位小数。弱起说明与
// --pickup 的报错文案共用，避免出现「弱起 1.0 拍」这种不一致写法。
static std::string formatBeatCount(double beats) {
    std::ostringstream os;
    if (std::fabs(beats - std::round(beats)) < 0.05) {
        os << (long)std::lround(beats);
    } else {
        os.setf(std::ios::fixed);
        os.precision(1);
        os << beats;
    }
    return os.str();
}

std::string pickupLabel(const MidiData& data) {
    BarGrid grid = barGridOf(data);
    if (grid.pickup <= 0) return std::string();
    long beatLen = grid.barLen / std::max(1, grid.beatsPerBar);
    if (beatLen <= 0) return std::string();
    double beats = (double)grid.pickup / (double)beatLen;
    return "弱起 " + formatBeatCount(beats) + " 拍";
}

std::string pickupArgError(double beats, const TimeSignature& sig) {
    // 0 拍是 --no-pickup 的语义，不是「长度为零的弱起」；负数早已被参数
    // 解析拦下，这里仍然兜一次，免得以后有别的调用方绕过 parseBeats。
    if (!(beats > 0.0))
        return "弱起长度必须大于 0 拍（不要弱起请用 --no-pickup）";
    const int num = sig.numerator > 0 ? sig.numerator : 4;
    // 容差取 1e-9：3.9999999 拍在 4/4 里就是整整一小节，不能被浮点误差放过；
    // 而 3.9 拍仍然合法（简谱里确实有 3.9 拍的弱起）。
    if (beats >= (double)num - 1e-9) {
        std::ostringstream os;
        os << "弱起 " << formatBeatCount(beats) << " 拍不小于整小节 "
           << num << " 拍（弱起必须短于小节）";
        return os.str();
    }
    return std::string();
}

// 输出一个音符记号：延长线拆成独立 token，读起来是 "1 - -" 而不是 "1--"
static void emitNoteToken(std::ostringstream& os, const std::string& head,
                          const std::string& dur, const std::string& art = "") {
    os << head;
    bool onlyDashes = !dur.empty() &&
                      dur.find_first_not_of('-') == std::string::npos;
    if (onlyDashes) {
        for (size_t i = 0; i < dur.size(); i++) os << " -";
    } else {
        os << dur;
    }
    // 演奏法记号紧跟数字，不插空格：文本谱里 `5·` 才看得出是这一个音断奏。
    os << art << " ";
}

// 时值记号的统一出口。durationSuffix() 取最近档，表外时值可能向上取整：
// 3.75 拍记成 4 拍（`---`）。只要多出来的那点还没越过小节线，就照最近档记
// （读起来干净，`1.4` 拍记 `1.` 也是这个道理）；一旦会把这一小节撑过拍号，
// 就拆成多段「宁短不长」的记法——`0 - -` 再补一条 `-_.`，总长正好 3.75 拍。
// 撑爆小节不是排版小事：它把后面每一小节的拍位整体推后，读者也没法判断
// 到底空了几拍（pedal_channels.mid 第 2 小节因此读成 4.25 拍）。
//
// exactRest 是休止符专用口径：间隙后面紧跟着一个音头，时间位置是定死的，
// 吸附（无论向上还是向下）都会让谱面拍数与真实时间错位——1.75 拍记成
// 0.(1.5 拍)、1.25 拍记成 0(1 拍)，4/4 的一小节只读出 3.5 拍。休止符
// 只有精确档才照记，表外档一律拆成多段「宁短不长」的记法。
static void emitDurationTokens(std::ostringstream& os, const std::string& head,
                               long ticks, long startTick, long barEnd,
                               int division, const std::string& art = "",
                               bool exactRest = false) {
    if (ticks <= 0) { emitNoteToken(os, head, "", art); return; }
    const long snapped = nearestNotatableTicks(ticks, division);

    // 先试算能不能精确拆完。余量不足最小时值时保留旧写法，绝不擅自改动
    // 时间推进（与 MusicXML 侧 splitRestDuration 的兜底同口径）。
    if (exactRest && snapped != ticks) {
        std::vector<long> parts;
        long left = ticks;
        while (left > 0) {
            long part = largestNotatableTicks(left, division);
            if (part <= 0) break;
            parts.push_back(part);
            left -= part;
        }
        if (left == 0 && parts.size() > 1) {
            for (size_t i = 0; i < parts.size(); i++)
                emitNoteToken(os, i == 0 ? head : "-",
                              durationSuffix(parts[i], division),
                              i == 0 ? art : "");
            return;
        }
    }

    // 音符（以及拆不干净的休止符）沿用旧口径。
    if (snapped <= ticks || startTick + snapped <= barEnd) {
        emitNoteToken(os, head, durationSuffix(ticks, division), art);
        return;
    }
    long left = ticks;
    bool first = true;
    while (left > 0) {
        long part = largestNotatableTicks(left, division);
        if (part <= 0) break;
        // 首段带音头与演奏记号，后续段用 "-" 续写（与跨小节续写同一套记号）
        emitNoteToken(os, first ? head : "-", durationSuffix(part, division),
                      first ? art : "");
        first = false;
        left -= part;
    }
    // 连最小档都放不下（不足 1/32 拍）：退回老写法，别把音头整个丢掉。
    if (first) emitNoteToken(os, head, durationSuffix(ticks, division), art);
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
    // 转调点也该记住自己的升降号侧，否则中间转进 Gb 的那一段照样会被
    // 印成 F#。base 可能被 --key 手工平移过，所以先把「曲首偏移」算出来：
    // 文件里写的 sf 加上这个偏移，才是输出该用的绝对调号。
    const int sfShift = keyFifths(base) - normalizeFifths12(sf0);
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
        k.fifthsHint = normalizeFifths12(p.sig.sharpsFlats + sfShift);
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
    const KeyMap& keys, const MidiData& data, const ExpressionMap& expr,
    const std::map<long, std::string>* chords = nullptr,
    bool barNumbers = false) {

    std::ostringstream os;
    int currentBar = -1;
    bool started = false;
    long prevEnd = 0;
    std::string lastDyn;
    // 力度口径已经印到哪个 tick：小节内换档要接着这个位置往后找，
    // 否则同一处变化会被相邻两个音头各印一遍（或者整条漏掉）。
    long lastDynTick = 0;
    // 这一行谱的通道口径：力度档位与渐强渐弱都按它取，不能随小节首音符变。
    const int partChannel = dominantChannel(notes);

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
        // 小节号：与 HTML 的 .barno 同一口径——弱起小节不编号，第一个
        // 完整小节才是 1，这样两端的「第几小节」永远指同一段音乐。
        if (barNumbers && bar > 0)
            os << "[" << grid.measureNumber(bar) << "] ";
        // 变拍号处把新拍号印在谱面上（只印旋律轨，伴奏轨不重复）。
        // 页眉的摘要说明「什么时候变」，这里说明「从哪一栏开始按新拍子数」。
        if (chords && grid.meterChangedAt(bar))
            os << "[" << grid.sigForBar(bar).toString() << "] ";
        // 转调标记落在起作用的那一小节：页眉的摘要说「什么时候变」，
        // 这里说「从哪一栏起按新调唱」。用「转」字与 [3/4] 拍号、
        // [C] 和弦区分开，纯文本里也不会看混。
        if (chords && keys.keyChangedAtBar(bar, grid))
            os << "[转" << ("1=" + keyName(keys.keyAtBar(bar, grid))) << "] ";
        // 段落名是整份谱的全局标记，只印主声部一次；渐强渐弱是通道级
        // 演奏表情，每个声部都要按自己的主导通道各印一次。旧版把两者一起
        // 塞进 if (chords)，伴奏声部（乃至第二条轨）的 cresc. 全被吞掉。
        for (const auto& m : expr.inRange(grid.barStart(bar), grid.barEnd(bar), channel)) {
            if (chords && !m.section.empty())
                os << "[" << escapeForTextScore(m.section) << "] ";
            if (m.wedge > 0) os << "[cresc.] ";
            else if (m.wedge < 0) os << "[dim.] ";
        }
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
        std::string d = expr.dynamicAt(atTick, channel);
        if (!d.empty() && d != lastDyn) {
            os << "[" << d << "] ";
            lastDyn = d;
        }
        // 小节线这一栏已经按 atTick 的口径标过，游标推到这里；
        // 小节内更晚会由下面的 dynamicStepsInside 接着往后补。
        lastDynTick = atTick;
    };
    // 跨小节时，后段用延长线续写：整拍输出 "-"，不足一拍带时值后缀
    auto emitHeld = [&](long segTicks, long atTick, long barEndTick) {
        if (segTicks <= 0) return;
        if (segTicks % data.division == 0) {
            for (long k = 0; k < segTicks / data.division; ++k) os << "- ";
        } else {
            emitDurationTokens(os, "-", segTicks, atTick, barEndTick, data.division);
        }
    };

    // 同拍的一组音是和弦（splitVoices 已把同拍同值的音放进同一声部）。
    // 旧实现一个音印一个 token 顺着排下去：`sample.mid` 钢琴左手的 C-E-G
    // 三个全音符被印成 `1, - - - 3, - - - 5, - - -`，4/4 的一小节读出 12
    // 拍，后面每一小节的拍位跟着错位。简谱里和弦是竖排的，文本谱没有竖排，
    // 这里用 `+` 把同一拍的音并成一个 token（`1,+3,+5,`），时值只写一次，
    // 拍数才与拍号对得上。`+` 也不会和 [C] 和弦名、3(...) 三连音、
    // (...)5 倚音这三种记号混淆。
    size_t idx = 0;
    while (idx < merged.size()) {
        size_t grpEnd = idx + 1;
        while (grpEnd < merged.size() &&
               merged[grpEnd].startTicks == merged[idx].startTicks)
            grpEnd++;
        const NoteEvent& n = merged[idx];
        // 组的时值取最短：splitVoices 已把同拍音对齐，这里只是防御。
        long groupDur = std::max<long>(n.durationTicks, 1);
        for (size_t k = idx + 1; k < grpEnd; k++)
            groupDur = std::min(groupDur, std::max<long>(merged[k].durationTicks, 1));

        // 右括号必须先于休止输出，否则 3(5 5 5 0 ) 会把休止吞进三连音组
        if (!triplets[idx] && inTriplet) { os << ") "; inTriplet = false; }

        // 休止：按小节切开输出，长前奏才能写成每小节一个 "0 - - -"。
        // 抽成 lambda 是因为倚音也要用：倚音整颗不占格，但它前面的空白
        // （弱起小节开头、小节中段的休止）不能跟着一起被吞掉——旧实现先
        // 跳过倚音再算 gap，那一段空白既没有音头也没有休止，直接消失，
        // parts.mid 的 2 拍弱起小节因此只读出 1 拍。
        auto emitRestUntil = [&](long endTick) {
            long gap = endTick - prevEnd;
            if (gap < restGateTicks(data.division)) return;
            if (!started && endTick <= 0) return;
            long t = prevEnd;
            while (t < endTick) {
                long bar = barOf(t);
                switchToBar(bar, t, partChannel);
                long seg = std::min(endTick, grid.barEnd(bar)) - t;
                if (seg <= 0) break;                    // 防御：避免死循环
                emitDurationTokens(os, "0", seg, t, grid.barEnd(bar), data.division,
                                   "", true);
                t += seg;
            }
        };

        // 倚音（装饰音）：本身不单独占一格，改挂到紧随其后的主音上。这里整颗
        // 跳过，只把结束位置推进到倚音尾部，避免主音的休止计算把它算成空拍。
        if (expr.isGraceNote(n.startTicks, n.channel)) {
            emitRestUntil(n.startTicks);
            started = true;
            prevEnd = n.startTicks + groupDur;
            idx = grpEnd;
            continue;
        }

        emitRestUntil(n.startTicks);

        switchToBar(barOf(n.startTicks), n.startTicks, partChannel);

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

        // 小节内换力度档：与上面的和弦同理，标记必须跟着音头走。
        // 旧实现只在 switchToBar 里取一次 dynamicAt，小节中途的换档既没有
        // 位置也没有出口——第 2 拍 ff→p、第 4 拍 p→f 时，p 整条消失、f 被
        // 推迟到下一小节开头才印出来，谱面与听到的力度差三拍。
        // 渐变段内部的中间档位不在这里印（由 [cresc.]/[dim.] 表达），
        // dynamicStepsInside 已按楔形区间排除，三端（文本谱 / HTML /
        // MusicXML）共用同一份判定。
        for (const auto& m : expr.dynamicStepsInside(lastDynTick, n.startTicks + 1, partChannel)) {
            lastDynTick = std::max(lastDynTick, m.tick);
            if (m.dynamic.empty() || m.dynamic == lastDyn) continue;
            os << "[" << m.dynamic << "] ";
            lastDyn = m.dynamic;
        }

        // 左括号放在小节线之后，否则 "3(" 会留在上一行末
        if (triplets[idx] && !inTriplet) { os << "3("; inTriplet = true; }

        // 三连音组内的音符不标时值后缀（"3(...)" 已表达节奏）
        // 倚音（装饰音）写成 (装饰音数字) 直接贴在主音数字前，例如 (3)5
        // 表示先快速滑/弹一个 3 再落到主音 5；括号把它和正式音区分开，
        // 也避免与和弦 [..]、三连音 3(..) 等记号混淆。
        const NoteExpression ne = expr.noteAt(n.startTicks, n.channel);
        // 和弦音按音高从低到高用 `+` 串起来；每个音各带自己的八度记号，
        // 所以 `1,+3,+5,` 里三个逗号分别管自己那个音。
        std::string head;
        if (ne.hasGrace())
            head = "(" + pitchToJianpu(ne.gracePitch, keys.at(n.startTicks)) + ")";
        bool toneWritten = false;
        for (size_t k = idx; k < grpEnd; k++) {
            const auto& cn = merged[k];
            if (expr.isGraceNote(cn.startTicks, cn.channel)) continue;
            if (toneWritten) head += "+";
            head += pitchToJianpu(cn.pitch, keys.at(n.startTicks));
            toneWritten = true;
        }
        if (!toneWritten) {          // 整组都是倚音：没有可印的音头
            idx = grpEnd;
            continue;
        }
        // 断音 / 重音 / 滑音 / 颤音只挂在真实音头上，延续段的 "-" 不带记号。
        const std::string art = ne.symbol();
        long noteEnd = n.startTicks + groupDur;
        long barEnd = grid.barEnd(barOf(n.startTicks));
        if (triplets[idx] || noteEnd <= barEnd) {
            if (triplets[idx])
                emitNoteToken(os, head, "", art);        // 节奏由 3(...) 表达
            else
                emitDurationTokens(os, head, groupDur, n.startTicks, barEnd,
                                   data.division, art);
        } else {
            // 长音跨小节：先写满本小节，再逐小节用延长线续上
            emitDurationTokens(os, head, barEnd - n.startTicks, n.startTicks,
                               barEnd, data.division, art);
            long t = barEnd;
            while (t < noteEnd) {
                long bar = barOf(t);
                switchToBar(bar, t, partChannel);
                long seg = std::min(noteEnd, grid.barEnd(bar)) - t;
                if (seg <= 0) break;                    // 防御：避免死循环
                emitHeld(seg, t, grid.barEnd(bar));
                t += seg;
            }
        }
        started = true;
        prevEnd = noteEnd;
        idx = grpEnd;
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
//
// 变速点一律走 tempoMarksOf 的折叠结果。旧版直接遍历 tempoChanges 取前 5 个，
// 遇到渐慢（MIDI 里是几十个连续 set_tempo）会写成
// 「♩=120 → 120(第3小节) → 116(第3小节) → 113(第3小节) → 109(第3小节) → …」，
// 五条全指同一小节，读者拿不到任何信息；折叠后同样一段渐慢只写一个
// 「rit. 120→60(第3小节)」。
std::string tempoSummary(const MidiData& data) {
    const std::vector<TempoMark> marks = tempoMarksOf(data);
    if (marks.empty()) return std::string();

    std::ostringstream os;
    os << "♩=" << marks[0].bpm;
    if (marks.size() > 1) {
        // 小节号走网格换算：弱起曲目里 tick/小节长 会整体偏一号
        BarGrid grid = barGridOf(data);
        for (size_t i = 1; i < marks.size() && i <= 4; i++) {
            const TempoMark& m = marks[i];
            const long bar = grid.measureNumber(grid.barIndex(m.tick));
            os << " → ";
            if (m.kind == TempoMarkKind::Ritardando)
                os << "rit. " << m.bpm << "→" << m.toBpm << "(第" << bar << "小节)";
            else if (m.kind == TempoMarkKind::Accelerando)
                os << "accel. " << m.bpm << "→" << m.toBpm << "(第" << bar << "小节)";
            else
                os << m.bpm << "(第" << bar << "小节)";
        }
        if (marks.size() > 5) os << " → …";
    }
    return os.str();
}

// ---- 声部 / 乐器标注 ----
// GM 标准 128 个音色号的英文全名；中文曲 MIDI 常常把民族乐器映射到 Flute /
// Harp / Tremolo Strings 等通用音色，所以这里只给标准 GM 集做中文译名覆盖，
// 不会凭空造出「笛子/二胡」这种名字误导用户。查不到（>=128）回落到空串。
static const char* GM_NAMES[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
    "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2",
    "Harpsichord", "Clavinet", "Celesta", "Glockenspiel",
    "Music Box", "Vibraphone", "Marimba", "Xylophone",
    "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ",
    "Rock Organ", "Church Organ", "Reed Organ", "Accordion",
    "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)",
    "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
    "Electric Guitar (clean)", "Electric Guitar (muted)",
    "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)",
    "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1",
    "Synth Bass 2", "Violin", "Viola", "Cello",
    "Contrabass", "Tremolo Strings", "Pizzicato Strings",
    "Orchestral Harp", "Timpani", "String Ensemble 1",
    "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
    "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)",
    "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)",
    "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)",
    "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)",
    "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)",
    "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
    "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot"
};

// 常用音色的中文译名覆盖（其余回落到上面的英文全名）。
static const char* GM_CN[128] = {
    "钢琴", nullptr, nullptr, nullptr, nullptr, nullptr, "拨弦键琴", "击弦键琴",
    nullptr, "钟琴", "八音盒", "电颤琴", "马林巴", "木琴", "管钟", nullptr,
    "管风琴", "打击风琴", nullptr, "教堂管风琴", "簧风琴", "手风琴", "口琴",
    nullptr, "尼龙弦吉他", "钢弦吉他", "爵士吉他", "清音电吉他", "闷音电吉他",
    "过载吉他", "失真吉他", nullptr, "原声贝斯", "电贝斯", nullptr, "无品贝斯",
    nullptr, nullptr, "合成贝斯", nullptr, "小提琴", "中提琴", "大提琴",
    "低音提琴", "颤音弦乐", "拨奏弦乐", "竖琴", "定音鼓", "弦乐合奏", nullptr,
    "合成弦乐", nullptr, "合唱啊", "人声呜", "合成人声", "乐团击奏", "小号",
    "长号", "大号", "弱音小号", "圆号", "铜管组", nullptr, nullptr, "高音萨克斯",
    "中音萨克斯", "次中音萨克斯", "上低音萨克斯", "双簧管", nullptr, "大管",
    "单簧管", "短笛", "长笛", "竖笛", "排箫", nullptr, "尺八", "哨子", "陶笛",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, "西塔琴", "班卓琴", "三味线", "筝", "卡林巴", "风笛", "民间提琴", "唢呐",
    nullptr, nullptr, "钢鼓", nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};

std::string gmInstrumentName(int prog) {
    if (prog < 0 || prog >= 128) return std::string();
    if (GM_CN[prog]) return std::string(GM_CN[prog]);
    return std::string(GM_NAMES[prog]);
}

// GM Level 1 打击乐键位 35..81 的中文名。同族键位刻意共用一名：诊断要回答的是
// 「这段鼓里有什么」，35/36 两个底鼓分开报只会把清单撑长、把常见鼓件挤到「等」
// 后面。未定义的键位（比如 GM2 扩展的 82 以后）由 gmDrumName 回落成「鼓件 N」。
static const char* GM_DRUM_NAMES[] = {
    /*35*/ "底鼓", "底鼓", "边击", "军鼓", "拍手", "军鼓",
    /*41*/ "低音嗵鼓", "闭镲", "低音嗵鼓", "脚踏镲", "中音嗵鼓", "开镲",
    /*47*/ "中音嗵鼓", "高音嗵鼓", "碎音镲", "高音嗵鼓", "叮叮镲", "中国镲",
    /*53*/ "叮叮铃", "铃鼓", "水镲", "牛铃", "碎音镲", "颤音器",
    /*59*/ "叮叮镲", "高音邦戈", "低音邦戈", "弱音康加", "开放康加", "低音康加",
    /*65*/ "高音小鼓", "低音小鼓", "高音阿哥哥", "低音阿哥哥", "沙锤", "沙球",
    /*71*/ "短哨", "长哨", "短刮器", "长刮器", "响棒", "高音木鱼",
    /*77*/ "低音木鱼", "弱音刮葫芦", "开放刮葫芦", "弱音三角铁", "开放三角铁"
};

std::string gmDrumName(int note) {
    if (note < 35 || note > 81) return "鼓件 " + std::to_string(note);
    return std::string(GM_DRUM_NAMES[note - 35]);
}

std::map<int, int> partPrograms(const MidiData& data) {
    // 统计每条声部轨（跳过打击乐通道 9）的主导通道与首音 tick。
    // 这张表是谱面页眉与 --list-tracks 诊断唯一的乐器名来源，两边都不能
    // 再各自去读 channelProgram，否则中途换音色的曲子会出现「诊断说小提琴、
    // 谱面写钢琴」这种自相矛盾的输出。
    std::map<int, std::map<int, int>> trackChanCount;  // track -> channel -> count
    std::map<int, long> trackFirstTick;                 // track -> 首音 tick
    for (const auto& n : data.notes) {
        // 逻辑通道是 port*16+ch，鼓件看低 4 位；port 0 时仍是原来的 9。
        if ((n.channel & 0x0F) == 9) continue;  // 鼓轨已在解析阶段丢弃，防御性再跳一次
        trackChanCount[n.track][n.channel]++;
        auto fit = trackFirstTick.find(n.track);
        if (fit == trackFirstTick.end() || n.startTicks < fit->second)
            trackFirstTick[n.track] = n.startTicks;
    }
    std::map<int, int> out;
    for (auto& [tr, chans] : trackChanCount) {
        if (chans.empty()) continue;
        // 取音符最多的通道作为该声部的代表通道
        int bestCh = chans.begin()->first, bestN = -1;
        for (auto& [ch, c] : chans)
            if (c > bestN) { bestN = c; bestCh = ch; }
        // 按声部首音那一刻生效的音色取名：一首曲子中途 0xC0 换乐器时，
        // 前半首不该被标成最后那件乐器。没有时间轴记录时 programAt 会
        // 自动回退到 channelProgram，普通 GM 文件行为逐字节不变。
        const long firstTick = trackFirstTick.count(tr) ? trackFirstTick[tr] : 0;
        out[tr] = programAt(data, bestCh, firstTick);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> listParts(const MidiData& data) {
    const auto progs = partPrograms(data);
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [tr, prg] : progs) {
        std::string name;
        auto it = data.trackNames.find(tr);
        if (it != data.trackNames.end() && !it->second.empty()) name = it->second;
        else name = "轨 " + std::to_string(tr);
        std::string inst;
        if (prg >= 0) inst = gmInstrumentName(prg);
        out.emplace_back(std::move(name), std::move(inst));
    }
    return out;
}

// 单行摘要里容不下换行：把任意空白串压成一个空格，并去掉首尾空白。
// HTML 本来就会这么折叠，文本谱不会——两端口径在这里对齐。
static std::string squeezeWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending = false;   // 前面已经有字，遇到空白就先记一笔
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!out.empty()) pending = true;
            continue;
        }
        if (pending) { out += ' '; pending = false; }
        out += c;
    }
    return out;
}

std::string partSummary(const MidiData& data) {
    auto parts = listParts(data);
    if (parts.empty()) return std::string();
    std::ostringstream os;
    os << "声部：";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) os << "，";
        // 轨名是文件里的裸字节：带换行就会把这个单行页眉撕成两半，
        // 带 ESC 就会在终端里执行。文档口径是丢掉，不是转义；再把剩下的
        // 空白压成一个空格，让文本谱和网页对同一个名字给出同样的形状
        // （HTML 本来就会把连续空白折叠成一个空格）。
        os << squeezeWhitespace(dropControlChars(parts[i].first));
        if (!parts[i].second.empty()) os << "·" << parts[i].second;
    }
    return os.str();
}

std::vector<TrackSummary> trackSummaries(const MidiData& data) {
    std::vector<TrackSummary> out;
    const int n = data.trackCount > 0 ? data.trackCount : 0;
    if (n <= 0) return out;

    // 音色表必须与谱面页眉同源（partPrograms 的按轨音色时间轴），
    // 否则诊断说「小提琴」而谱面写「钢琴」，用户照诊断选轨会被误导。
    const auto progs = partPrograms(data);
    out.reserve((size_t)n);
    for (int t = 0; t < n; t++) {
        TrackSummary s;
        s.index = t;
        auto nit = data.trackNames.find(t);
        if (nit != data.trackNames.end()) s.name = nit->second;
        auto dit = data.drumNotesByTrack.find(t);
        if (dit != data.drumNotesByTrack.end()) s.drumNotes = dit->second;
        // 鼓件名在这里统一映射：解析层只认音高，CLI、网页下拉框、空轨提示
        // 若各查一张表，同一份文件会报出三种鼓名。同族键位合并计数后按
        // 次数降序排，让最常听到的鼓件排在清单前面。
        auto dpit = data.drumPitchesByTrack.find(t);
        if (dpit != data.drumPitchesByTrack.end()) {
            std::map<std::string, int> byName;
            for (const auto& kv : dpit->second) {
                if (kv.second <= 0) continue;
                byName[gmDrumName(kv.first)] += kv.second;
            }
            for (const auto& kv : byName)
                s.drumInstruments.emplace_back(kv.first, kv.second);
            std::stable_sort(s.drumInstruments.begin(), s.drumInstruments.end(),
                             [](const auto& a, const auto& b) {
                                 if (a.second != b.second) return a.second > b.second;
                                 return a.first < b.first;
                             });
        }
        auto pit = progs.find(t);
        if (pit != progs.end() && pit->second >= 0) {
            s.program = pit->second;
            s.instrument = gmInstrumentName(pit->second);
        }
        out.push_back(std::move(s));
    }

    // 一次线性扫描把音符与歌词按轨归位：轨数是几十的量级，但文件可能有
    // 上万个音符，逐轨重扫就是 O(轨 × 音)。顺手记下每轨每通道的首音 tick：
    // 通道级乐器名要和 partPrograms 一样按时间轴取，不能读 channelProgram 末值。
    std::map<int, std::map<int, long>> trackChanFirstTick;
    for (const auto& nt : data.notes) {
        if (nt.track >= 0 && nt.track < n) {
            out[(size_t)nt.track].notes++;
            auto& cm = trackChanFirstTick[nt.track];
            auto it = cm.find(nt.channel);
            if (it == cm.end() || nt.startTicks < it->second)
                cm[nt.channel] = nt.startTicks;
        }
    }
    // 有旋律音却没有 0xC0 的轨：programAt 返回 -1 不是「读不出来」，而是
    // 「文件没声明」——按 GM 规范这种通道就是 0 号音色（钢琴），播放器也
    // 确实这么响。诊断里留一整段空白，用户会以为工具解析失败，所以把默认值
    // 写明；program 仍保持 -1，机器照样能区分「声明了」和「没声明」。
    // 这与 channelInstruments 不矛盾：那里答「各通道各自声明了什么」（空数组
    // ＝都没声明），这里答「这条轨听起来是什么」。
    for (auto& s : out) {
        if (s.notes > 0 && s.program < 0) s.instrument = "未指定（GM 默认为钢琴）";
    }
    // 歌词按「可唱音节」计，不数 0x05 事件：一条事件里写「春江花月夜」是 5 个字，
    // 而「@KMIDI」「-」「_」这些元数据 / 延续符一个字都不唱。此前直接数事件，
    // karaoke.mid 的 Vocal 轨有 9 条事件却只有 5 个音节，诊断写着「歌词 9 字」。
    for (const auto& c : lyricSyllablesByTrack(data)) {
        if (c.first >= 0 && c.first < n) out[(size_t)c.first].lyrics = c.second;
    }
    // 通道布局来自解析层（std::map 已按通道号升序）。note-on 数为 0 的通道
    // 理论上不会进表，这里仍挡一道，免得将来改成「记 0xC0 的通道」时冒出空项。
    for (const auto& kv : data.channelNotesByTrack) {
        if (kv.first < 0 || kv.first >= n) continue;
        for (const auto& ch : kv.second) {
            if (ch.second <= 0) continue;
            out[(size_t)kv.first].channels.push_back(ch);
            // 鼓件音也占一个通道位，但打击乐没有 GM 旋律音色名，别把通道 9
            // 标成「钢琴」（默认 program 0）——那比不说还误导。
            if ((ch.first & 0x0F) == 9) continue;
            long firstTick = 0;
            auto tit = trackChanFirstTick.find(kv.first);
            if (tit != trackChanFirstTick.end()) {
                auto cit = tit->second.find(ch.first);
                if (cit != tit->second.end()) firstTick = cit->second;
            }
            const std::string nm =
                gmInstrumentName(programAt(data, ch.first, firstTick));
            if (!nm.empty())
                out[(size_t)kv.first].channelInstruments.emplace_back(ch.first, nm);
        }
    }
    return out;
}

std::string drumInstrumentList(const TrackSummary& s, size_t maxItems,
                               bool withCount) {
    std::string out;
    const size_t n = std::min(maxItems, s.drumInstruments.size());
    for (size_t i = 0; i < n; i++) {
        if (i) out += withCount ? ", " : "、";
        out += s.drumInstruments[i].first;
        if (withCount) out += " " + std::to_string(s.drumInstruments[i].second);
    }
    if (s.drumInstruments.size() > n) out += " 等";
    return out;
}

std::string formatJianpu(const MidiData& data, const KeyEstimate& key,
                           bool barNumbers) {
    const KeyMap keys(data, key);
    const BarGrid grid = barGridOf(data);
    const ExpressionMap expr(data);
    std::ostringstream os;
    os << keys.summary(grid)
       << "    " << meterSummary(data);
    std::string tempo = tempoSummary(data);
    if (!tempo.empty()) os << "    " << tempo;
    std::string parts = partSummary(data);
    if (!parts.empty()) os << "    " << parts;
    std::string pickup = pickupLabel(data);
    if (!pickup.empty()) os << "    " << pickup;
    const std::string exprSum = expr.summary();
    if (!exprSum.empty()) os << "    " << exprSum;
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
                sum += (pb.value - 8192) / 8192.0 * pb.range;  // 该通道该时刻的范围
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

    // 同轨复调：一条轨常常同时装着两个声部（钢琴左右手写在同一个轨里、
    // 伴奏垫在人声轨下面）。简谱一行写不下两个声部，所以按声部拆成独立的
    // 谱行。单声部轨（绝大多数文件）走原样，输出与旧版逐字一致。
    struct PartView {
        int track = 0;
        int voice = 0;
        int voiceCount = 1;
        std::vector<NoteEvent> notes;
    };
    std::vector<PartView> partViews;
    for (auto& [t, ns] : byTrack) {
        const std::vector<int> vo = splitVoices(
            ns, 3, [&](const NoteEvent& n) { return expr.isGraceNote(n.startTicks, n.channel); });
        int voiceCount = 0;
        for (int v : vo) voiceCount = std::max(voiceCount, v + 1);
        if (voiceCount <= 1) {
            partViews.push_back({t, 0, 1, std::move(ns)});
            continue;
        }
        for (int v = 0; v < voiceCount; v++) {
            PartView pv;
            pv.track = t;
            pv.voice = v;
            pv.voiceCount = voiceCount;
            for (size_t i = 0; i < ns.size(); i++)
                if (vo[i] == v) pv.notes.push_back(ns[i]);
            if (!pv.notes.empty()) partViews.push_back(std::move(pv));
        }
    }

    for (const auto& pv : partViews) {
        const int t = pv.track;
        const auto& ns = pv.notes;
        // 旋律声部的「主要那条」才挂和弦与歌词：第二个声部跟着标一遍只是噪音。
        const bool isMain = (t == mainTrack && pv.voice == 0);
        // 轨名按「轨」判定，不按声部：否则主旋律的第二声部会被叫成「伴奏轨0」。
        const bool isMainTrack = (t == mainTrack);
        std::string label;
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) {
            // 文档口径：控制字符丢掉、空白压成单空格。这是文本谱里的小标题，
            // 一行一条声部；轨名里的换行/ESC 会把它拆成两行、或把终端染红。
            label = squeezeWhitespace(dropControlChars(it->second));
        } else {
            label = isMainTrack ? "旋律轨" : "伴奏轨" + std::to_string(t);
        }
        if (pv.voiceCount > 1)
            label += " · 声部" + std::to_string(pv.voice + 1);
        os << "[" << label << "] (" << ns.size() << " 音符)";
        if (isMain && !chordByBar.empty()) {
            os << " · 和弦: " << distinctChordCount(chordByBar) << " 个";
        }
        os << "\n";
        os << formatTrack(ns, keys, data, expr, isMain ? &chordByBar : nullptr,
                          barNumbers);
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

// ---- 外部 JSON 歌词（v1.101.0：有界对象扫描，不再"找字面量"）----
// 宽松数字解析（定义见文件下方）。WASM 版默认关闭 C++ 异常，
// std::stoi / std::stod 遇到畸形输入会直接 abort 整个页面，故统一用它。
static double parseNum(const std::string& s, size_t from, size_t to);

// UTF-8 BOM（EF BB BF）：Windows 记事本存 UTF-8 时会自动加。LRC 逐行解析
// 要求 line[0]=='['，BOM 占掉首字节后整行歌词被静默丢掉；JSON 虽不受影响
// （find('{') 会跳过前缀），两个入口仍统一剥掉，保证 CLI / WASM / 单测同口径。
static void stripUtf8Bom(std::string& s) {
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
}

// 歌词正文里裸 C0 控制字符（除 tab/LF/CR）与 DEL：MusicXML 的 xmlEscape()
// 会丢，JSON 的 \u0000 解码分支也会丢，唯独 HTML 页眉转义不会 —— 同一份
// 歌词三端口径不一致。两个纯文本入口统一过滤；正常文件不含这些字节，
// 输出逐字节零回归。
static void sanitizeLyricControls(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\t' || c == '\n' || c == '\r' ||
            (c >= 0x20u && c != 0x7Fu))
            out.push_back((char)c);
    }
    if (out.size() != s.size()) s.swap(out);
}

static bool isJsonSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}


// 读 4 位十六进制；任何一位非法都返回 false，绝不越界读。
static bool jsonHex4(const std::string& c, size_t p, unsigned& v) {
    if (p + 4 > c.size()) return false;
    unsigned r = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char ch = c[p + k];
        unsigned d;
        if (ch >= '0' && ch <= '9')      d = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a') + 10u;
        else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A') + 10u;
        else return false;
        r = (r << 4) | d;
    }
    v = r;
    return true;
}

static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80u) {
        out.push_back((char)cp);
    } else if (cp < 0x800u) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

// UTF-16 LE/BE → UTF-8。代理对合成 U+10000..U+10FFFF，孤立代理与 C0/DEL
// 一律丢弃（与 skipJsonString 的 \uXXXX 分支同口径），任何分支都不产 U+FFFD。
// start 是首个码元的偏移：带 BOM 时 2（跳过 FF FE / FE FF），无 BOM 时 0。
// 尾部残缺的半个码元（奇数长度）直接忽略，不越界读。
static std::string decodeUtf16(const std::string& raw, size_t start,
                               bool littleEndian) {
    std::string out;
    unsigned high = 0;
    for (size_t i = start; i + 1 < raw.size(); i += 2) {
        const unsigned a = (unsigned char)raw[i];
        const unsigned b = (unsigned char)raw[i + 1];
        unsigned cp = littleEndian ? (a | (b << 8)) : ((a << 8) | b);
        if (high) {
            if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                cp = 0x10000u + ((high - 0xD800u) << 10) + (cp - 0xDC00u);
                appendUtf8(out, cp);
                high = 0;
                continue;
            }
            high = 0;                         // 孤立高代理丢弃
        }
        if (cp >= 0xD800u && cp <= 0xDBFFu) { high = cp; continue; }
        if (cp >= 0xDC00u && cp <= 0xDFFFu) continue;  // 孤立低代理丢弃
        if (cp == 0u ||
            (cp < 0x20u && cp != '\t' && cp != '\n' && cp != '\r') ||
            cp == 0x7Fu) continue;
        appendUtf8(out, cp);
    }
    return out;
}

// 无 BOM UTF-16 的字节签名嗅探。UTF-16 里的 ASCII 段（LRC 时间戳、JSON 键名
// 与标点）形如 `5B 00` / `00 5B`——NUL 全挤在同一个奇偶位上。而合法 UTF-8 /
// GBK / Shift-JIS 歌词一个 NUL 都没有（真含 NUL 已是病态输入），所以「NUL
// 成密度且只在一侧」这个判据零误判，不会像「按字节对打分」那样把 GBK 抢走。
// 纯汉字的无 ASCII 文件（整份只有汉字、没有时间戳或键名）认不出来——那种内容
// 本来就不符合 LRC / JSON 语法，硬猜只会伤到 GBK。
// 样本量门槛不能省：4 字节 2 个 NUL 的碎片一眼看是 50%，按 90% 算却「达标」。
// 小样本只认 100% 单侧，样本够大（>= 8 个 NUL）才放宽到 90%——因为 U+4E00「一」
// 这类低字节为 0 的汉字会在**另一侧**留下 NUL，真实文件往往不是干净的一半一半。
static bool sniffUtf16NoBom(const std::string& raw, bool& littleEndian) {
    if (raw.size() < 6) return false;
    size_t nulEven = 0, nulOdd = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\0') continue;
        if (i & 1u) ++nulOdd; else ++nulEven;
    }
    const size_t total = nulEven + nulOdd;
    if (total < 3 || total * 8 < raw.size()) return false;
    if (nulOdd == total)  { littleEndian = true;  return true; }
    if (nulEven == total) { littleEndian = false; return true; }
    if (total >= 8) {
        if (nulOdd * 10 >= total * 9)  { littleEndian = true;  return true; }
        if (nulEven * 10 >= total * 9) { littleEndian = false; return true; }
    }
    return false;                                  // 两侧都有，当普通字节流
}

// 外部歌词文件的唯一字节归一入口：UTF-16 BOM 先转 UTF-8，其次识别无 BOM 的
// UTF-16，其余交给 textenc::normalize()（严格 UTF-8 原样返回，否则 GBK /
// Shift-JIS 打分，再不行按 CP1252 逐字节折叠）。LRC 的 readTextFile() 与
// JSON 的 parseLyrics() 都走这里，避免「同一份 GBK 歌词另存成 .json 就产出
// 非法 UTF-8 的 HTML 和打不开的 MusicXML」。
// 注意：纯文本入口（WASM set_lyrics 收到的 JS 字符串已是 Unicode）不做
// 字节转码，只走 stripUtf8Bom + sanitizeLyricControls。
std::string normalizeLyricFileBytes(const std::string& raw) {
    if (raw.size() >= 2) {
        const unsigned char a = (unsigned char)raw[0];
        const unsigned char b = (unsigned char)raw[1];
        if (a == 0xFFu && b == 0xFEu) {
            // FF FE 00 00 是 UTF-32LE BOM；按 UTF-16 解会变成满屏 NUL。
            // 不支持就干脆给空文本，让上层明确报「没解析出歌词行」。
            if (raw.size() >= 4 && (unsigned char)raw[2] == 0x00u &&
                (unsigned char)raw[3] == 0x00u) return {};
            return decodeUtf16(raw, 2, true);
        }
        if (a == 0xFEu && b == 0xFFu) return decodeUtf16(raw, 2, false);
        if (a == 0x00u && b == 0x00u && raw.size() >= 4 &&
            (unsigned char)raw[2] == 0xFEu && (unsigned char)raw[3] == 0xFFu)
            return {};                        // UTF-32BE BOM，同上
    }
    // 无 BOM 的 UTF-16（部分工具 / 脚本拼出来的 LRC、JSON）：NUL 是合法
    // UTF-8，不拦就会被 isValidUtf8() 放过，时间戳里全是 NUL、一行都解析不出。
    bool noBomLe = true;
    if (sniffUtf16NoBom(raw, noBomLe)) return decodeUtf16(raw, 0, noBomLe);
    return textenc::normalize(raw);
}

// 读一个 JSON 字符串字面量：q 必须指向起始 '"'，返回结束引号位置；out 非空时
// 写入解码后的内容。没有闭合引号一律返回 npos——旧版在 npos 上继续做 +1，
// 回绕成 0 后 pos 不再前进，形成无限循环并把整份文件逐字塞进谱面。
static size_t skipJsonString(const std::string& c, size_t q, std::string* out) {
    if (out) out->clear();
    if (q >= c.size() || c[q] != '"') return std::string::npos;
    for (size_t i = q + 1; i < c.size(); ++i) {
        const char ch = c[i];
        if (ch == '"') return i;
        if (ch == '\\' && i + 1 < c.size()) {
            const char e = c[++i];
            if (!out) continue;
            switch (e) {
                case '"':  out->push_back('"');  break;
                case '\\': out->push_back('\\'); break;
                case '/':  out->push_back('/');  break;
                case 'n':  out->push_back('\n'); break;
                case 'r':  out->push_back('\r'); break;
                case 't':  out->push_back('\t'); break;
                case 'b':  out->push_back('\b'); break;
                case 'f':  out->push_back('\f'); break;
                case 'u': {
                    // JSON 标准 \uXXXX：很多歌词工具默认把非 ASCII 转成它。
                    // 旧版走 default 只收下 'u'，后面 4 位 hex 当普通字符，
                    // 「你」变成字面 "u4F60" 印进谱面。
                    unsigned cp = 0;
                    if (jsonHex4(c, i + 1, cp)) {
                        i += 4;                 // i 指向第 4 位 hex
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            unsigned lo = 0;
                            if (i + 6 < c.size() && c[i + 1] == '\\' &&
                                c[i + 2] == 'u' && jsonHex4(c, i + 3, lo) &&
                                lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10)
                                     + (lo - 0xDC00u);
                                i += 6;
                            } else {
                                cp = 0;         // 孤立高代理，丢弃
                            }
                        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                            cp = 0;             // 孤立低代理，丢弃
                        }
                        // \u0000 等 C0 控制字符不能变成 NUL 塞进 HTML /
                        // MusicXML；DEL 与孤立代理（cp=0）一并丢弃。绝不产 U+FFFD。
                        if (cp >= 0x20u && cp != 0x7Fu && cp <= 0x10FFFFu)
                            appendUtf8(*out, cp);
                    } else {
                        out->push_back('u');   // 畸形 \u 按字面降级
                    }
                    break;
                }
                default:   out->push_back(e);   break;  // 未知转义按字面收下
            }
        } else if (out) {
            out->push_back(ch);
        }
    }
    return std::string::npos;
}

// 在 [from, to) 内找 JSON key 字面量，跳过所有字符串值。单纯 find() 会把歌词
// 正文里出现的 "start_beat" 当成下一个字段，进而把词挂错位置。
// endOut 返回该 key 的**结束引号**位置：键名可能是 "\u0074ext" 这类转义形式，
// 长度不能按字面量写死，冒号和值引号一律从 endOut+1 开始找。
static size_t findJsonKey(const std::string& c, const char* name,
                          size_t from, size_t to,
                          size_t* endOut = nullptr) {
    if (endOut) *endOut = std::string::npos;
    if (to > c.size()) to = c.size();
    for (size_t i = from; i < to;) {
        if (c[i] != '"') { ++i; continue; }
        std::string s;
        const size_t e = skipJsonString(c, i, &s);
        if (e == std::string::npos) return std::string::npos;
        if (s == name) {
            if (endOut) *endOut = e;
            return i;
        }
        i = e + 1;
        if (i >= to) break;
    }
    return std::string::npos;
}

// 找对象的结束 '}'，跳过字符串里的花括号（歌词 "世}界" 不该切断对象）。
static size_t findObjectEnd(const std::string& c, size_t objStart) {
    for (size_t i = objStart + 1; i < c.size();) {
        if (c[i] == '"') {
            const size_t e = skipJsonString(c, i, nullptr);
            if (e == std::string::npos) return c.size();
            i = e + 1;
            continue;
        }
        if (c[i] == '}') return i;
        ++i;
    }
    return c.size();
}

// 取扩展名并转小写。点必须落在最后一段路径里，否则 "a.dir/b" 会把
// ".dir/b" 当成扩展名（Windows 上还可能是反斜杠分隔）。
static std::string lyricExtLower(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    if (slash != std::string::npos && dot < slash) return {};
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

// 内容嗅探：只读第一个标签/括号，不做全文猜测。
//   [00:12.00] / [ti:…] / [offset:±500]  → LRC（标签里有冒号）
//   [{…}] / ["…"] / [1,2]                → JSON（第一个有效字符不是标签）
//   不以 [ 开头                          → JSON
// 判据只看「第一个 ] 之前有没有冒号」，几十字节就够，不会把长文件读两遍。
static bool lyricLooksLikeLrc(const std::string& text) {
    size_t i = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        i = 3;                                     // 自带 BOM 的文本
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\r' || text[i] == '\n'))
        ++i;                                       // 前导空行
    if (i >= text.size() || text[i] != '[') return false;
    ++i;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) return false;
    const char c = text[i];
    if (c == '{' || c == '"' || c == ']') return false;   // 是 JSON 数组/对象
    const size_t close = text.find(']', i);
    if (close == std::string::npos || close - i > 64) return false;
    return text.find(':', i) < close;
}

LyricFormat detectLyricFormat(const std::string& path, const std::string& content) {
    const std::string ext = lyricExtLower(path);
    if (ext == ".lrc") return LyricFormat::Lrc;
    if (ext == ".json") return LyricFormat::Json;
    // 扩展名不认识（含没有扩展名）：按内容判定，绝不让文件名决定命运。
    return lyricLooksLikeLrc(content) ? LyricFormat::Lrc : LyricFormat::Json;
}

// 解析 JSON 歌词文本本身（纯逻辑，便于单测）。文件入口 parseLyrics 只负责读盘。
std::vector<LyricSyllable> parseLyricsText(const std::string& contentRaw, int division,
                                            size_t* dropped) {
    if (dropped) *dropped = 0;       // 输出参数，入口清零
    std::string content = contentRaw;
    stripUtf8Bom(content);
    sanitizeLyricControls(content);
    std::vector<LyricSyllable> result;

    // start_beat 是用户给的外部数字。判据沿用 v1.100.1 的平台无关 512 小节
    // cap（Linux 的 long 是 64 位，绝不能拿 LONG_MAX 当业务上界）。
    const int safeDiv = std::max(std::abs(division), 1);
    const double unit = (double)safeDiv;
    const double capTicks = (double)timelineCapTicks(safeDiv);
    const double maxBeat = capTicks / unit;

    size_t pos = 0;
    while (pos < content.size()) {
        const size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        const size_t objEnd = findObjectEnd(content, objStart);
        // 每轮严格前进：objEnd 可能是 content.size()（未闭合对象），也不能让
        // 下一轮又命中同一个 '{'。这是 P0「畸形 JSON 挂起」的硬保障。
        size_t nextPos = (objEnd < content.size()) ? objEnd + 1 : content.size();
        if (nextPos <= objStart) nextPos = objStart + 1;

        size_t keyEnd = std::string::npos, textEnd = std::string::npos;
        const size_t key = findJsonKey(content, "start_beat", objStart, objEnd,
                                       &keyEnd);
        const size_t textPos = findJsonKey(content, "text", objStart, objEnd,
                                           &textEnd);
        // 没有这两个字段的对象不是歌词行（例如外层元数据），静默跳过。
        if (key == std::string::npos || textPos == std::string::npos ||
            keyEnd == std::string::npos || textEnd == std::string::npos) {
            pos = nextPos;
            continue;
        }
        auto dropOne = [&]() {
            if (dropped) ++*dropped;
            pos = nextPos;
        };

        // "start_beat" 后面必须是冒号 + 数字，数字只在该对象内取。
        // 从真实结束引号之后找，键名写成 "\u0073tart_beat" 也能正确定位。
        const size_t colon = content.find(':', keyEnd + 1);
        if (colon == std::string::npos || colon >= objEnd) { dropOne(); continue; }
        size_t end = content.find_first_of(",}", colon + 1);
        if (end == std::string::npos || end > objEnd) end = objEnd;
        const double beatNum = parseNum(content, colon + 1, end);

        // "text" 后面只允许「空白 + 恰好一个冒号 + 空白 + 字符串字面量」。
        // 旧版按字面量长度 find('"')，遇到 "text":123 会抓到下一个对象里的
        // 引号，把 JSON 源码字符 s/t/a/r/t 当歌词印进谱面；键名被写成
        // "\u0074ext" 时更会连冒号都找不到，整行被静默丢弃。
        size_t q1 = std::string::npos;
        if (textEnd < objEnd) {
            size_t j = textEnd + 1;
            while (j < objEnd && isJsonSpace(content[j])) ++j;
            if (j < objEnd && content[j] == ':') {
                ++j;
                while (j < objEnd && isJsonSpace(content[j])) ++j;
                if (j < objEnd && content[j] == '"') q1 = j;
            }
        }
        const bool shapeOk = (q1 != std::string::npos);
        std::string line;
        const size_t q2 = shapeOk ? skipJsonString(content, q1, &line)
                                  : std::string::npos;
        if (q2 == std::string::npos || q2 >= objEnd) { dropOne(); continue; }

        if (!std::isfinite(beatNum) || beatNum < -maxBeat || beatNum > maxBeat) {
            dropOne();
            continue;
        }
        const long beat = (long)beatNum;
        double startD = (double)beat * division;
        if (startD > capTicks) startD = capTicks;
        if (startD < -capTicks) startD = -capTicks;

        // 按字拆分，每个字分配一个 tick（均匀分布在该行的 beat 范围内）。
        const long startTick = (long)startD;
        size_t nextKeyEnd = std::string::npos;
        const size_t nextStart = findJsonKey(content, "start_beat",
                                             objEnd, content.size(),
                                             &nextKeyEnd);
        long lineLenTicks = division * 4;  // 默认 4 拍
        if (nextStart != std::string::npos && nextKeyEnd != std::string::npos) {
            const size_t nc = content.find(':', nextKeyEnd + 1);
            if (nc != std::string::npos) {
                size_t ne = content.find_first_of(",}", nc + 1);
                if (ne == std::string::npos) ne = content.size();
                const double nextNum = parseNum(content, nc + 1, ne);
                // 下一行 beat 离谱 / 倒序时退回默认 4 拍，不做溢出减法。
                const double spanD = (nextNum - beatNum) * division;
                if (std::isfinite(spanD) && spanD >= 0 && spanD <= capTicks)
                    lineLenTicks = (long)spanD;
            }
        }
        if (lineLenTicks < 0) lineLenTicks = 0;
        const long maxSpan = LONG_MAX - startTick;
        if (lineLenTicks > maxSpan) lineLenTicks = maxSpan;
        const long perChar = lineLenTicks / std::max((int)line.size(), 1);
        for (size_t i = 0; i < line.size(); i++) {
            // UTF-8 中文字符：每个字 3 字节
            size_t charLen = 1;
            const unsigned char c = (unsigned char)line[i];
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            if (i + charLen > line.size()) break;
            result.push_back({startTick + (long)i * perChar, line.substr(i, charLen)});
            i += charLen - 1;
        }
        pos = nextPos;
    }
    return result;
}

std::vector<LyricSyllable> parseLyrics(const std::string& jsonPath, int division,
                                       size_t* dropped) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        if (dropped) *dropped = 0;
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    // 与 LRC 的 readTextFile() 同口径：GBK / Shift-JIS / CP1252 / UTF-16 BOM
    // 都在这里归一成合法 UTF-8，parseLyricsText 只看纯文本。
    return parseLyricsText(normalizeLyricFileBytes(content), division, dropped);
}

// 解析LRC歌词文件: [mm:ss.xx]歌词
// ---- LRC 解析（秒 -> tick 一律走 TempoTimeline）----
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

static std::string lowerAscii(std::string v) {
    for (char& c : v) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return v;
}

// [offset:±毫秒] 是所有时间戳的整体平移量（正=歌词延后）。多个取最后一个，
// 这与主流播放器的「最后一个生效」一致；毫秒值只在这里换成秒，不碰 tick。
static double lrcOffsetSeconds(const std::string& lrc) {
    double off = 0.0;
    size_t pos = 0;
    while (pos < lrc.size()) {
        size_t nl = lrc.find('\n', pos);
        if (nl == std::string::npos) nl = lrc.size();
        const std::string line = lowerAscii(lrc.substr(pos, nl - pos));
        size_t p = 0;
        while ((p = line.find("[offset:", p)) != std::string::npos) {
            size_t close = line.find(']', p);
            if (close == std::string::npos) break;
            off = parseNum(lrc, pos + p + 8, pos + close) / 1000.0;
            p = close + 1;
        }
        pos = (nl >= lrc.size()) ? lrc.size() : nl + 1;
    }
    return off;
}

// 小数位按「位数」定权重：1 位 = 十分之一、2 位 = 百分之一、3 位 = 毫秒。
// LRC 里 .5 / .50 / .500 都是 0.5 秒，写 4 位以上只取前 3 位。
static double lrcFraction(const std::string& digits) {
    double v = 0.0, scale = 1.0;
    const size_t n = digits.size() > 3 ? 3 : digits.size();
    for (size_t i = 0; i < n; i++) {
        scale /= 10.0;
        v += (digits[i] - '0') * scale;
    }
    return v;
}

static bool lrcAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// 把一段「整数[.小数]」拆成整数值与小数位字符串。整数位允许为空（`.50` = 0.5），
// 这是旧版 parseNum 就接受过的写法，不趁这次收紧。
static bool lrcSplitNumber(const std::string& s, double& whole,
                           std::string& frac) {
    const size_t dot = s.find('.');
    const std::string w = (dot == std::string::npos) ? s : s.substr(0, dot);
    frac = (dot == std::string::npos) ? std::string() : s.substr(dot + 1);
    if (!w.empty() && !lrcAllDigits(w)) return false;
    if (w.empty() && frac.empty()) return false;
    if (!frac.empty() && !lrcAllDigits(frac)) return false;
    // 用 double 累加：畸形文件里可能出现几十位数字，整数累加会溢出成 UB，
    // 而这里最终只要能算出秒数再由 secondsToTickChecked 判上限即可。
    whole = 0.0;
    for (char c : w) whole = whole * 10.0 + (c - '0');
    return true;
}

// 解析一个时间戳标签（已去掉方括号）。返回 false = 不是时间戳（元数据 / 畸形）。
//
// LRC 的规范单元是 mm:ss[.xx]，但真实文件里长出了几种写法。旧版只按「第一个
// 冒号」切一刀，后半段交给 parseNum —— 而 parseNum 见到第二个冒号就停：
//   [00:05:50]    冒号式百分秒（部分播放器 / 导出工具这么写）-> 5.00s，慢半秒
//   [00:00:05.00] 小时级时间戳                                 -> 0.00s，整首堆到曲首
// 两种都是静默失准，谁也不报错。这里按「冒号段数 + 末段有没有小数点」显式分派：
//   2 段  mm:ss[.frac]                    规范写法
//   3 段  · 末段带 '.' → hh:mm:ss[.frac]  超过一小时的录音
//         · 末段纯数字 → mm:ss:cc         冒号式百分秒
// 括号内两侧空白（[ 00:05.00 ]）也一并接受——旧版会连整行一起丢掉。
static bool parseLrcTimestamp(const std::string& tag, double& outSec) {
    size_t a = 0, b = tag.size();
    while (a < b && (tag[a] == ' ' || tag[a] == '\t')) a++;
    while (b > a && (tag[b - 1] == ' ' || tag[b - 1] == '\t')) b--;
    if (a >= b) return false;
    // 必须数字开头：ti: / ar: / offset: 这类元数据在这里一律不是时间戳
    if (tag[a] < '0' || tag[a] > '9') return false;

    std::vector<std::string> seg;
    size_t start = a;
    for (size_t i = a; i < b; i++) {
        if (tag[i] != ':') continue;
        seg.push_back(tag.substr(start, i - start));
        start = i + 1;
        if (seg.size() > 2) return false;   // [a:b:c:d] 没有公认含义
    }
    seg.push_back(tag.substr(start, b - start));
    if (seg.size() < 2) return false;       // 没有冒号就不是时间戳

    double first = 0;
    std::string firstFrac;
    if (!lrcSplitNumber(seg[0], first, firstFrac) || !firstFrac.empty())
        return false;                       // 首段只许整数

    if (seg.size() == 2) {
        double ss = 0;
        std::string frac;
        if (!lrcSplitNumber(seg[1], ss, frac)) return false;
        outSec = first * 60.0 + ss + lrcFraction(frac);
        return true;
    }

    double mid = 0;
    std::string midFrac;
    if (!lrcSplitNumber(seg[1], mid, midFrac) || !midFrac.empty())
        return false;                       // 中段只许整数
    if (seg[2].find('.') != std::string::npos) {
        double ss = 0;
        std::string frac;
        if (!lrcSplitNumber(seg[2], ss, frac)) return false;
        outSec = first * 3600.0 + mid * 60.0 + ss + lrcFraction(frac);
        return true;
    }
    if (!lrcAllDigits(seg[2])) return false;   // 冒号式百分秒，纯数字
    outSec = first * 60.0 + mid + lrcFraction(seg[2]);
    return true;
}

// 增强型 LRC（Enhanced LRC / A2 扩展）在行内用 <mm:ss.xx> 标每个字的时刻：
//   [00:12.00]<00:12.00>你<00:12.40>好<00:12.80>吗
// 这些标签不是歌词字。旧版把行内剩余原串整段当文字，于是 <00:00.00> 被拆成
// 一串 "<" "0" "0" ":" 音节印进谱面（HTML 里能看到 &lt; 0 0 :），还把「共 N
// 字」撑大——实测 8 个字变 36 个字，连带触发「这一段的字比旋律音多」误报。
// 这里只剥掉**能解析成时间戳**的尖括号标签；歌词正文里偶尔出现的其它尖括号
// 原样保留，不擅自吞字符。
static std::string stripEnhancedLrcTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            out.push_back(s[i++]);
            continue;
        }
        const size_t close = s.find('>', i + 1);
        if (close == std::string::npos) {
            // 截断的尾标签（`<00:00.50` 没了右尖括号）：形如「数字打头且带
            // 冒号」就按时间标签丢掉，否则（`<3` 这种正文）原样保留。
            const size_t j = i + 1;
            if (j < s.size() && s[j] >= '0' && s[j] <= '9' &&
                s.find(':', j) != std::string::npos)
                break;
            out.append(s, i, std::string::npos);
            break;
        }
        double sec = 0;
        if (parseLrcTimestamp(s.substr(i + 1, close - i - 1), sec)) {
            i = close + 1;                     // 逐字时间标签：丢掉
            continue;
        }
        out.push_back(s[i++]);                 // 不是时间戳，按普通字符处理
    }
    return out;
}

// 一行里可以有多个 [mm:ss.xx]（同一句词在副歌重复出现），也可以有
// [ti:] / [ar:] / [offset:] 这类元数据。首个标签不是时间戳时整行跳过。
static void lrcTimestamps(const std::string& line,
                          std::vector<double>& secs, std::string& text) {
    size_t p = 0;
    while (p < line.size() && line[p] == '[') {
        size_t close = line.find(']', p);
        if (close == std::string::npos) break;
        const std::string tag = line.substr(p + 1, close - p - 1);
        double sec = 0;
        if (parseLrcTimestamp(tag, sec)) {
            secs.push_back(sec);
        } else if (secs.empty()) {
            return;  // 元数据行 / 畸形行，不当歌词
        }
        p = close + 1;
    }
    text = stripEnhancedLrcTags(line.substr(p));
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
}

// 唯一的时间戳换算点：offset 先叠到秒上，再由 tempo map 反解成 tick。
static std::vector<LyricLine> parseLrcTextImpl(const std::string& lrcRaw,
                                               const TempoTimeline& tl,
                                               size_t* dropped) {
    if (dropped) *dropped = 0;   // 输出参数，见 parseLyrics
    std::string lrc = lrcRaw;
    stripUtf8Bom(lrc);
    sanitizeLyricControls(lrc);
    const double off = lrcOffsetSeconds(lrc);
    std::vector<LyricLine> out;
    size_t pos = 0;
    while (pos < lrc.size()) {
        size_t nl = lrc.find('\n', pos);
        if (nl == std::string::npos) nl = lrc.size();
        std::string line = lrc.substr(pos, nl - pos);
        pos = (nl >= lrc.size()) ? lrc.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '[') continue;

        std::vector<double> secs;
        std::string text;
        lrcTimestamps(line, secs, text);
        if (text.empty()) continue;
        for (double s : secs) {
            long tick = 0;
            if (!tl.secondsToTickChecked(s + off, tick)) {
                // 秒数换算成 tick 会溢出 32 位 long：留着只会让歌词被静默
                // 挂到曲首（或整行消失）。显式丢弃，由调用方提示用户。
                if (dropped) ++*dropped;
                continue;
            }
            out.push_back({tick, text});
        }
    }
    // 多时间戳 + offset 可能让行顺序打乱，下游 assignLyrics 依赖升序
    std::stable_sort(out.begin(), out.end(),
                     [](const LyricLine& a, const LyricLine& b) {
                         return a.startTick < b.startTick;
                     });
    return out;
}

static std::string readTextFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    // 老 LRC 常见 GBK / Shift-JIS / UTF-16 BOM；归一后再交给按字节切行的
    // 解析器，否则中文歌词会以非法 UTF-8 混进 HTML / MusicXML。
    // 与 parseLyrics() 共用同一个入口，JSON / LRC 不再两套口径。
    return normalizeLyricFileBytes(raw);
}

std::vector<LyricLine> parseLrcText(const std::string& lrc, const MidiData& data,
                                    size_t* dropped) {
    return parseLrcTextImpl(lrc, TempoTimeline(data), dropped);
}

std::vector<LyricLine> parseLrcText(const std::string& lrc, int division, int bpm,
                                    size_t* dropped) {
    if (division <= 0 || bpm <= 0) return {};
    // 旧签名没有 tempo map，按曲首单速构造一条等价时间轴
    const std::vector<TempoPoint> one{{0, (double)bpm}};
    return parseLrcTextImpl(lrc, TempoTimeline(one, division), dropped);
}

std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, const MidiData& data,
                                     size_t* dropped) {
    return parseLrcTextImpl(readTextFile(lrcPath), TempoTimeline(data), dropped);
}

std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, int division, int bpm,
                                     size_t* dropped) {
    return parseLrcText(readTextFile(lrcPath), division, bpm, dropped);
}

// 按字拆成音节（旧接口）。行长按「4 秒」换算：用时间轴双向查询，
// 变速段里 4 秒对应的 tick 数会跟着变，不会像固定 secPerBeat 那样失真。
static std::vector<LyricSyllable> parseLrcSyllables(const std::string& lrc,
                                                    const TempoTimeline& tl,
                                                    size_t* dropped) {
    std::vector<LyricSyllable> result;
    const auto lines = parseLrcTextImpl(lrc, tl, dropped);
    for (const auto& ln : lines) {
        int charCount = 0;
        for (size_t i = 0; i < ln.text.size(); ) {
            unsigned char c = ln.text[i];
            int charLen = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                        : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + charLen > ln.text.size()) break;
            charCount++;
            i += charLen;
        }
        if (charCount == 0) continue;
        long endTick = 0;
        if (!tl.secondsToTickChecked(tl.tickToSeconds(ln.startTick) + 4.0, endTick))
            continue;
        const long perChar = std::max<long>(0, endTick - ln.startTick) / charCount;
        long idx = 0;
        for (size_t i = 0; i < ln.text.size(); ) {
            unsigned char c = ln.text[i];
            int charLen = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                        : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + charLen > ln.text.size()) break;
            result.push_back({ln.startTick + idx * perChar, ln.text.substr(i, charLen)});
            i += charLen;
            idx++;
        }
    }
    return result;
}

std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, const MidiData& data,
                                    size_t* dropped) {
    return parseLrcSyllables(readTextFile(lrcPath), TempoTimeline(data), dropped);
}

std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, int division, int bpm,
                                    size_t* dropped) {
    if (division <= 0 || bpm <= 0) return {};
    const std::vector<TempoPoint> one{{0, (double)bpm}};
    return parseLrcSyllables(readTextFile(lrcPath), TempoTimeline(one, division), dropped);
}

int tempoChangeCount(const MidiData& data) {
    // 折叠后的记号数减掉曲首那一个，才是「中途变了几次速」。
    // 一段渐慢从 17 个事件收敛成 1 个记号，提示语里的数字才对得上读者
    // 在谱面上数得到的标记。
    const size_t n = tempoMarksOf(data).size();
    return n > 0 ? (int)n - 1 : 0;
}

// ---- 速度标记折叠 ----
//
// 时间轴（TempoTimeline）必须逐点保留 —— 几十个自动化节点是算准秒数的唯一
// 依据。但谱面只该有 `rit.` / `accel.` 一个记号（见 notation.hpp 的说明）。
// 这里做的是「事件 -> 符号」的收敛，不改 data 里的任何东西。
std::vector<TempoMark> tempoMarksOf(const MidiData& data) {
    std::vector<TempoMark> marks;

    // 先归一化：排序、同 tick 后写覆盖、丢弃非法 BPM。与 TempoTimeline 用
    // 同一套规则，否则「时间轴按 3 个点算、谱面按 4 个点标」这种错位迟早出现。
    std::vector<TempoPoint> pts = data.tempoChanges;
    std::stable_sort(pts.begin(), pts.end(),
                     [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
    std::vector<TempoPoint> uniq;
    for (const auto& p : pts) {
        if (p.bpm <= 0 || p.tick < 0) continue;
        if (!uniq.empty() && uniq.back().tick == p.tick) uniq.back() = p;
        else uniq.push_back(p);
    }
    if (uniq.empty()) return marks;

    // 曲首速度永远是独立记号：不管它后面接的是渐慢还是保持，读者都得先知道
    // 「从多快开始」。这样也保证 marks 的第一个元素 tick 恒为 uniq[0].tick。
    marks.push_back({uniq[0].tick, uniq[0].tick, (int)std::lround(uniq[0].bpm),
                     (int)std::lround(uniq[0].bpm), TempoMarkKind::Step, 1});
    if (uniq.size() < 2) return marks;

    // 聚簇：相邻间隔 <= 1 拍的事件属于同一次速度变化。
    //
    // 为什么阈值是「一拍」而不是拍内的几分之一：DAW 的 tempo 自动化节点再密
    // 也不会密过 1/16 拍，而两次**独立**的阶跃变速不可能挤在一拍之内——那在
    // 音乐上没有意义（拍子还没走完就换两次速度）。所以一拍是个安全的分界：
    // 单点阶跃（120→90）自己成簇，一整段渐变曲线（几十个点）合成一个簇。
    const long gap = data.division > 0 ? (long)data.division : 480L;

    size_t i = 1;
    while (i < uniq.size()) {
        size_t j = i;
        while (j + 1 < uniq.size() && uniq[j + 1].tick - uniq[j].tick <= gap) j++;
        // 簇是 uniq[i..j]，共 j-i+1 个点
        const size_t n = j - i + 1;

        const int firstBpm = (int)std::lround(uniq[i].bpm);
        const int lastBpm  = (int)std::lround(uniq[j].bpm);

        // 单调性：全升 / 全降（允许持平，自动化曲线里常见平台）
        bool nonDecreasing = true, nonIncreasing = true;
        for (size_t k = i; k < j; k++) {
            const int a = (int)std::lround(uniq[k].bpm);
            const int b = (int)std::lround(uniq[k + 1].bpm);
            if (b < a) nonDecreasing = false;
            if (b > a) nonIncreasing = false;
        }

        // 跨度太小（< 5%）不当作渐变：120→118 这种抖动写成 rit. 是误导。
        const int prevBpm = (int)std::lround(uniq[i > 0 ? i - 1 : 0].bpm);
        const double span = prevBpm > 0
                          ? std::fabs((double)(lastBpm - firstBpm)) / (double)prevBpm
                          : 0.0;

        const bool isRamp = (n >= 3) && (nonDecreasing || nonIncreasing) && span >= 0.05;

        if (isRamp) {
            marks.push_back({uniq[i].tick, uniq[j].tick, firstBpm, lastBpm,
                             nonDecreasing ? TempoMarkKind::Accelerando
                                           : TempoMarkKind::Ritardando,
                             (int)n});
        } else {
            // 阶跃 / 抖动：只保留这一簇的最终速度。抖动（自动化曲线来回拉）
            // 在谱面上没有对应记号，取末值至少保证「后面按这个速度唱」是对的。
            marks.push_back({uniq[j].tick, uniq[j].tick, lastBpm, lastBpm,
                             TempoMarkKind::Step, (int)n});
        }
        i = j + 1;
    }
    return marks;
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

    // 每个和声窗既要统计音级，也要记住「最低音」——转位（C/E、G/B）只能由
    // 低音判定：C E G 三个音无论怎么排列，音级集合都一样，只有最低音能说明
    // 作曲家把哪个音放在低音部。低音常常来自独立的贝斯轨，所以这里把所有
    // 非旋律轨的音一起收进来，跟和弦音的统计口径保持一致。
    struct WinInfo {
        std::map<int, int> pitchCount;  // 音级 -> 次数
        int lowestPitch = 1000;         // 窗口内最低的 MIDI 音高
    };
    std::map<long, WinInfo> winInfo;  // 窗口起点 -> 统计
    for (const auto& note : data.notes) {
        if (note.track == mainTrack) continue;
        int pc = note.pitch % 12;
        if (pc < 0) pc += 12;
        WinInfo& w = winInfo[windowOf(note.startTicks)];
        w.pitchCount[pc]++;
        if (note.pitch < w.lowestPitch) w.lowestPitch = note.pitch;
    }

    struct ChordType {
        const char* suffix;
        std::vector<int> intervals;
        const char* kind;   // MusicXML <kind> 的标准取值
    };
    // 顺序有讲究：同分时先命中者胜，所以三和弦排在七和弦之前，
    // 只有在第三个音之外还多出一个「特征音」时才会升格成七和弦。
    const std::vector<ChordType> chordTypes = {
        {"",      {0, 4, 7},     "major"},              // 大三和弦
        {"m",     {0, 3, 7},     "minor"},              // 小三和弦
        {"7",     {0, 4, 7, 10}, "dominant"},           // 属七和弦
        {"m7",    {0, 3, 7, 10}, "minor-seventh"},      // 小七和弦
        {"maj7",  {0, 4, 7, 11}, "major-seventh"},      // 大七和弦
        {"dim",   {0, 3, 6},     "diminished"},         // 减三和弦
        {"aug",   {0, 4, 8},     "augmented"},          // 增三和弦
        {"sus4",  {0, 5, 7},     "suspended-fourth"},   // 挂四和弦
        {"sus2",  {0, 2, 7},     "suspended-second"},   // 挂二和弦
        {"m7b5",  {0, 3, 6, 10}, "half-diminished"},    // 半减七（Bm7b5）
        {"7sus4", {0, 5, 7, 10}, "suspended-fourth"},   // 挂四七（G7sus4）
    };

    static const char* NOTE_NAMES_SHARP[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    static const char* NOTE_NAMES_FLAT[12] = {
        "C", "Db", "D", "Eb", "E", "F",
        "Gb", "G", "Ab", "A", "Bb", "B"
    };

    // 单个窗口里最可能的和弦；证据不足时返回空 Pick（root < 0）。
    // 打分仍是「和弦各音出现次数之和」，4 音和弦只有在三个音都在场、
    // 且多出的那个特征音也真出现时才压过同根音的三和弦。
    struct Pick { int root = -1; int type = -1; int score = 0; };
    auto bestPickOf = [&](const std::map<int, int>& pitchCount) -> Pick {
        if (pitchCount.size() < 2) return Pick{};   // 只有一个音级不构成和弦

        Pick best;
        for (const auto& kv : pitchCount) {
            const int root = kv.first;
            for (size_t ti = 0; ti < chordTypes.size(); ti++) {
                int score = 0;
                bool allPresent = true;
                for (int interval : chordTypes[ti].intervals) {
                    auto f = pitchCount.find((root + interval) % 12);
                    if (f == pitchCount.end()) { allPresent = false; break; }
                    score += f->second;
                }
                if (allPresent && score > best.score) {
                    best = Pick{root, (int)ti, score};
                }
            }
        }
        return best.score >= 3 ? best : Pick{};
    };

    // 相邻同名合并。空窗口不改 prevName，所以「C · 空 · C」不会标两次。
    std::string prevName;
    for (const auto& kv : winInfo) {
        const WinInfo& wi = kv.second;
        const Pick pick = bestPickOf(wi.pitchCount);
        if (pick.root < 0) continue;

        const char* const* names = prefersFlats(keys.at(kv.first))
                                 ? NOTE_NAMES_FLAT : NOTE_NAMES_SHARP;
        const ChordType& ct = chordTypes[pick.type];

        std::string name = std::string(names[pick.root]) + ct.suffix;
        int bassPc = -1;
        // 转位：最低音是「和弦音」但不是根音时写成 C/E。低音落在和弦外
        // （经过音、持续音）时不写斜杠——那不是转位，写出来反而是错的。
        if (wi.lowestPitch < 1000) {
            int bp = wi.lowestPitch % 12;
            if (bp < 0) bp += 12;
            bool isChordTone = false;
            for (int interval : ct.intervals)
                if ((pick.root + interval) % 12 == bp) { isChordTone = true; break; }
            if (isChordTone && bp != pick.root) {
                bassPc = bp;
                name += "/";
                name += names[bp];
            }
        }

        if (name == prevName) continue;
        ChordMark cm;
        cm.startTick = kv.first;
        cm.name = name;
        cm.rootPc = pick.root;
        cm.kind = ct.kind;
        cm.kindText = ct.suffix;
        cm.bassPc = bassPc;
        chords.push_back(std::move(cm));
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

// 把一行文字铺到 onsets 的 [a0, a1) 这段音头上。
//
// tailEnd 是这一行右端的收尾位置（下一组歌词的起点或全曲末音之后）。
static void placeLyricLine(LyricAssignment& out, const std::string& text,
                           const std::vector<long>& onsets,
                           size_t a0, size_t a1, long tailEnd) {
    const auto syls = splitUtf8(text);
    const size_t n = a1 - a0, m = syls.size();
    if (n == 0 || m == 0) return;
    // 分母在铺排开始时就记好：m > n 时循环会 break，剩下的字根本不会进
    // 循环体，靠循环次数是数不出来的。
    out.charsTotal += m;

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
        pl.tick = onsets[a0 + a];
        pl.noteCount = (int)(b - a);
        pl.lastNoteTick = onsets[a0 + b - 1];
        // 右端不能越过下一行歌词的起点，否则连音线会拖进下一句
        pl.spanEnd = (b < n) ? onsets[a0 + b] : tailEnd;
        if (pl.spanEnd <= pl.tick) pl.spanEnd = pl.tick + 1;

        out.at[pl.tick] = pl;
        if (pl.noteCount >= 2) out.melismas.push_back(pl);
        ++out.charsPlaced;
    }
}

LyricAssignment assignLyrics(const MidiData& data,
                             const std::vector<LyricLine>& lines,
                             int mainTrack) {
    LyricAssignment out;
    if (mainTrack < 0 || lines.empty()) return out;

    // 只统计旋律轨的音头。伴奏的和弦音也在同一 tick 上，混进来会让「一字
    // 多音」的组数翻倍——柱式伴奏三个音就是三个 onset（去重后虽仍是一个，
    // 但换轨会让歌词挂到错误的声部上）。
    //
    // 倚音不算音头（v1.139.0）：它是主音前面的装饰，谱面上写成小字上标，
    // 压根不占一列（三个输出层都跳过它）。把它算进音头会让整句词从那里往后
    // 错开一个音——那个字还谁也印不出来，命令行却把它记成「挂上」。
    const ExpressionMap expr(data);
    std::vector<long> onsets;
    long songEnd = 0;
    for (const auto& n : data.notes) {
        if (n.track != mainTrack) continue;
        if (expr.isGraceNote(n.startTicks, n.channel)) continue;
        onsets.push_back(n.startTicks);
        songEnd = std::max(songEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    }
    if (onsets.empty()) return out;
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());

    for (size_t li = 0; li < lines.size(); ) {
        // 同一时间戳的连续行算一组。双语 LRC（原文一行、译文一行共用同一个
        // [mm:ss.xx]）非常普遍；旧版把每行的右端直接取成下一行的起点，同戳时
        // to == from，这一行一个音头都拿不到就 continue，整句文字被静默吞掉，
        // 页面上只剩后一行。
        size_t gEnd = li + 1;
        while (gEnd < lines.size() &&
               lines[gEnd].startTick == lines[li].startTick) gEnd++;
        const long from = lines[li].startTick;
        // 最后一行的右端取「全曲最后一个音之后」，保证末字也能覆盖到尾音
        const long to = (gEnd < lines.size()) ? lines[gEnd].startTick : songEnd + 1;

        std::vector<long> inLine;
        for (long t : onsets) if (t >= from && t < to) inLine.push_back(t);

        const size_t n = inLine.size();
        const size_t k = gEnd - li;
        // 组内音头按行均分：第 j 行拿 [j*n/k, (j+1)*n/k)。k == 1 时与旧版
        // 逐字节等价；k > 1 时每一行都有自己的音头（行数多于音头时后面几行
        // 确实无处可挂，这是谱面容量问题，不是解析丢失）。
        for (size_t j = 0; j < k; j++) {
            const size_t a0 = (size_t)((unsigned long long)j * n / k);
            const size_t a1 = (size_t)((unsigned long long)(j + 1) * n / k);
            if (a1 <= a0) {
                // 这一行一个音头都没分到（组内行数多于音头数，或整行的
                // 时间戳落在全曲最后一个音之后）。整行不出现，但必须记进
                // 统计——否则「歌词已加载: 12 行」与实际印出来的 8 行
                // 对不上，用户只能猜是自己的文件坏了还是程序漏了。
                const size_t missed = splitUtf8(lines[li + j].text).size();
                if (missed > 0) {
                    out.charsTotal += missed;
                    ++out.linesEmpty;
                }
                continue;
            }
            placeLyricLine(out, lines[li + j].text, inLine, a0, a1,
                           (j + 1 < k) ? inLine[a1] : to);
        }
        li = gEnd;
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

// 规整「单条轨」上的 0x05 事件。normalizeMidiLyrics 拿它的结果，
// pickMidiLyricTrack 只借用它数音节——两条通路共用一个规整口径，
// 否则「哪条轨音节多」的判断会和实际印出来的字对不上。
static std::vector<MidiSyllable> normalizeLyricEvents(std::vector<const LyricEvent*> evs) {
    std::vector<MidiSyllable> out;
    if (evs.empty()) return out;
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

std::vector<std::pair<int, int>> lyricSyllablesByTrack(const MidiData& data) {
    std::vector<std::pair<int, int>> out;
    if (data.midiLyrics.empty()) return out;
    std::map<int, std::vector<const LyricEvent*>> byTrack;
    for (const auto& e : data.midiLyrics) byTrack[e.track].push_back(&e);
    // std::map 保证按轨号升序，调用方可以直接依赖这个次序做「并列取轨号小」
    for (const auto& kv : byTrack) {
        const int n = (int)normalizeLyricEvents(kv.second).size();
        if (n > 0) out.push_back({kv.first, n});   // 只有元数据 / 延续符的轨不占位
    }
    return out;
}

LyricTrackPick pickMidiLyricTrack(const MidiData& data) {
    LyricTrackPick pick;
    const std::vector<std::pair<int, int>> counts = lyricSyllablesByTrack(data);
    for (const auto& c : counts) {
        // 用严格大于比较，并列时自然保住轨号小的那条
        if (c.second > pick.syllables) { pick.syllables = c.second; pick.track = c.first; }
    }
    if (pick.track < 0 || pick.syllables == 0) return LyricTrackPick{};
    for (const auto& c : counts) {
        if (c.first == pick.track) continue;
        pick.droppedTracks++;
        pick.droppedSyllables += c.second;
    }
    return pick;
}

std::string midiLyricHint(const MidiData& data, int selectedTrack) {
    const std::vector<std::pair<int, int>> counts = lyricSyllablesByTrack(data);
    if (counts.empty()) return "此文件没有内置歌词";

    const LyricTrackPick pick = pickMidiLyricTrack(data);
    if (selectedTrack < 0) {
        // 自动（全部轨）：只有一条轨的词会被采用，其余整轨忽略——必须说出来，
        // 否则「自带 10 个音节」看起来像全曲只有 10 个字。
        return "此文件自带 " + std::to_string(pick.syllables) + " 个歌词音节（轨 " +
               std::to_string(pick.track) + "）" + droppedLyricNote(data);
    }

    int mine = 0;
    for (const auto& c : counts)
        if (c.first == selectedTrack) mine = c.second;
    if (mine > 0) {
        // 单轨模式只取所选轨：别的轨上的词这次不参与，说清楚免得下一次换轨后
        // 「音节数怎么变了」。
        int others = 0, otherSyllables = 0;
        for (const auto& c : counts) {
            if (c.first == selectedTrack) continue;
            others++;
            otherSyllables += c.second;
        }
        std::string s = "轨 " + std::to_string(selectedTrack) + " 自带 " +
                        std::to_string(mine) + " 个歌词音节";
        if (others > 0)
            s += "，另 " + std::to_string(others) + " 轨的 " +
                 std::to_string(otherSyllables) + " 个音节不参与（只取所选轨）";
        return s;
    }
    // 所选轨一个字都没有，但文件别处有词：这时绝不能说「此文件没有内置歌词」，
    // 用户会以为这份 MIDI 压根不带词，转而去翻一个根本不存在的 LRC。
    return "轨 " + std::to_string(selectedTrack) + " 没有歌词；文件的 " +
           std::to_string(pick.syllables) + " 个音节在轨 " +
           std::to_string(pick.track) + "，把轨道选回「自动」才会用上";
}

std::string droppedLyricNote(const MidiData& data) {
    const LyricTrackPick pick = pickMidiLyricTrack(data);
    if (pick.droppedTracks <= 0) return std::string();
    return "，另 " + std::to_string(pick.droppedTracks) + " 轨的 " +
           std::to_string(pick.droppedSyllables) + " 个音节已忽略";
}

std::string emptyTrackHint(const MidiData& data, int track) {
    const std::vector<TrackSummary> tracks = trackSummaries(data);
    const TrackSummary* mine = nullptr;
    for (const auto& t : tracks)
        if (t.index == track) mine = &t;

    // 这条轨根本不存在：用户照着文件头部那个 16 位轨数敲了 --track 9，
    // 只说「无音符」会让他以为轨存在、只是空的。
    if (!mine) {
        if (tracks.empty()) return "本文件没有解析到任何轨道";
        return "本文件实际只有 " + std::to_string(tracks.size()) + " 轨（0~" +
               std::to_string((int)tracks.size() - 1) + "）";
    }
    if (mine->notes > 0) return std::string();  // 有旋律音符，不该走到这里

    // 这条轨里到底有什么。鼓件音和歌词音节都不是「音符」，但都不是「什么都没有」：
    // 一条只带 10 个字的歌词轨被 --track 选中时，说「无音符」等于什么也没说。
    std::string s;
    if (mine->drumNotes > 0) {
        s = "该轨只有 " + std::to_string(mine->drumNotes) + " 个鼓件音";
        // 光说「有鼓件音」还是不知道丢的是什么；这里把最多的三种鼓件点出来。
        const std::string detail = drumInstrumentList(*mine, 4, false);
        s += "（" + (detail.empty() ? std::string("简谱不支持鼓谱")
                                    : detail + "；简谱不支持鼓谱") + "）";
    }
    if (mine->lyrics > 0) {
        if (s.empty()) s = "该轨只有 ";
        else s += "和 ";
        s += std::to_string(mine->lyrics) + " 个歌词音节";
    }

    // 旋律在哪：音符最多的一条，并列取轨号小的（与 --list-tracks 的列出顺序一致，
    // 也让结果与遍历顺序无关）。
    const TrackSummary* best = nullptr;
    for (const auto& t : tracks) {
        if (t.index == track || t.notes <= 0) continue;
        if (!best || t.notes > best->notes) best = &t;
    }
    if (best) {
        const std::string where = "旋律在轨 " + std::to_string(best->index) + "（" +
                                  std::to_string(best->notes) + " 音）";
        if (s.empty()) s = where;
        else s += "，" + where;
    }
    return s;
}

std::vector<MidiSyllable> normalizeMidiLyrics(const MidiData& data) {
    if (data.midiLyrics.empty()) return std::vector<MidiSyllable>();
    // 多轨歌词只取一条：两段词各占一轨时合并会把两段词交错成乱码
    const LyricTrackPick pick = pickMidiLyricTrack(data);
    if (pick.track < 0) return std::vector<MidiSyllable>();
    std::vector<const LyricEvent*> evs;
    evs.reserve(data.midiLyrics.size());
    for (const auto& e : data.midiLyrics)
        if (e.track == pick.track) evs.push_back(&e);
    return normalizeLyricEvents(std::move(evs));
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
        // 与 assignLyrics 同一口径：先记分母，循环里的 break / continue
        // 会让「想挂几个」和「挂上几个」分道扬镳。
        out.charsTotal += m;

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
                ++out.charsPlaced;
            }
        }
        i = j;
    }
    return out;
}

int midiLyricTrack(const MidiData& data) {
    // 只有元数据 / 延续符的文件（例如拿 0x05 存段落标记）没有可唱音节，
    // 这种「歌词轨」不能拿来给旋律轨加分。
    const LyricTrackPick pick = pickMidiLyricTrack(data);
    if (pick.track < 0) return -1;
    std::set<int> noteTracks;
    for (const auto& n : data.notes) noteTracks.insert(n.track);
    // format 0 只有一条轨，歌词轨号不携带任何信息
    if (noteTracks.size() < 2) return -1;
    // 歌词分散在多轨时以前一律返回 -1（怕认错轨），现在 pickMidiLyricTrack
    // 已经定下「实际唱的是哪条」，只要那条轨真有音符就敢用。
    if (!noteTracks.count(pick.track)) return -1;
    return pick.track;
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
                   std::to_string(pickMidiLyricTrack(data).syllables) + " 音节" +
                   droppedLyricNote(data) + "）";
        default: return std::string();
    }
}

LyricAssignment resolveLyrics(const MidiData& data,
                              const std::vector<LyricLine>* external,
                              int mainTrack) {
    if (external && !external->empty()) return assignLyrics(data, *external, mainTrack);
    return assignMidiLyrics(data, mainTrack);
}

// ---------------------------------------------------------------------------
// 演奏表情分析
// ---------------------------------------------------------------------------

std::string NoteExpression::symbol() const {
    std::string s;
    if (staccatissimo) s += "▼";
    else if (staccato) s += "·";
    if (accent) s += ">";
    switch (bend) {
        case BendMark::SlideUp:   s += "↗"; break;
        case BendMark::SlideDown: s += "↘"; break;
        case BendMark::Trill:     s += "tr"; break;
        default: break;
    }
    return s;
}

int dynamicToMidiVolume(const std::string& dyn) {
    if (dyn == "pp") return 30;
    if (dyn == "p")  return 45;
    if (dyn == "mp") return 60;
    if (dyn == "mf") return 75;
    if (dyn == "f")  return 95;
    if (dyn == "ff") return 100;  // <sound dynamics> 按百分比，上限 100
    return 80;
}

static std::string trimSectionText(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

// 段落名按「字符」截断，绝不在 UTF-8 序列中间下刀。
//
// 旧写法是 `text.substr(0, 40)` —— 按字节切。一个汉字占 3 字节，于是：
//   1) 中文段落名只显示 13 个字（40/3），英文却是 40 个字，两种语言两种待遇；
//   2) 40 不是 3 的倍数，最后一个字被劈成半截，剩下的孤立续字节（0x80..0xBF）
//      让整份 HTML / MusicXML 变成**非法 UTF-8**。XML 尤其致命：解析器在第一
//      个非法字节就报错，MuseScore 拒绝打开整份谱，而不是只丢一个段落名。
// 所以上限取 40 个字符（码点），截断点回退到序列首字节，半个字整个丢掉。
static std::string truncateUtf8Chars(const std::string& s, size_t maxChars) {
    size_t i = 0, chars = 0;
    while (i < s.size() && chars < maxChars) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        // 尾部残缺的序列（上游数据本身被截过）也不留下来。
        if (i + len > s.size()) break;
        i += len;
        chars++;
    }
    return s.substr(0, i);
}

// 见 notation.hpp「自由文本的显示安全」。两条口径刻意分开：
// 文档输出丢字符（不把噪声塞进谱面），诊断输出转义成看得见的写法
// （它要回答「这个名字里到底有什么」）。
std::string dropControlChars(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        // \t \n \r 三种空白在文本谱 / HTML / XML 里都合法（HTML 折叠成空格、
        // XML 原样保留），其余 C0 与 DEL 一律丢掉：XML 1.0 明文禁止 C0，
        // HTML 遇到它们是解析错误，终端则会把 ESC 当命令执行、把 DEL 当退格
        // （0x7F 在 XML 里其实合法，但它在三种载体里都不可见，留它只会让
        // 「名字里到底有什么」更难查，索性一并丢掉）。
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        if (c == 0x7F) continue;
        out += (char)c;
    }
    return out;
}

std::string escapeForTerminal(const std::string& s, size_t maxChars) {
    // 先按字符（不是字节）截断，再转义：转义后的 `\x1b` 是 4 个字符，
    // 若反过来做，截断点会落在转义序列中间，输出就成了新的乱码。
    const std::string src = maxChars ? truncateUtf8Chars(s, maxChars) : s;
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(src.size() + 8);
    for (unsigned char c : src) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    // 不写 snprintf 是为了不被本地化的格式串 / 缓冲区长度
                    // 绊住；这里只有两位十六进制，手写更快也更清楚。
                    out += "\\x";
                    out += HEX[(c >> 4) & 0xF];
                    out += HEX[c & 0xF];
                } else {
                    out += (char)c;
                }
        }
    }
    // 截断要留痕：静默砍掉半个名字，用户会以为文件里就叫这个。
    if (src.size() < s.size()) out += "…";
    return out;
}

// 见 notation.hpp「自由文本的显示安全」：文本谱专用的结构字符折叠。
// 全角形式与半角字形几乎一样，但在文本谱里没有语法含义（不是小节线、
// 不是标记定界符），stripMarks / 小节切分 / 拍数解析都把它当普通文字。
//
// 用字节串而不是直接写全角字面量：源码编码虽有 BOM/UTF-8 判定，但这里是
// 逐字节拼装，写死 UTF-8 字节可避免任何编译期编码歧义。
std::string escapeForTextScore(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '|': out += "\xEF\xBD\x9C"; break;  // ｜ U+FF5C
            case '[': out += "\xEF\xBC\xBB"; break;  // ［ U+FF3B
            case ']': out += "\xEF\xBC\xBD"; break;  // ］ U+FF3D
            case '\n':                                // 换行会把这一行谱撕成两行
            case '\r': out += ' '; break;
            default:  out += (char)c; break;
        }
    }
    return out;
}

// 从某个音的弯音事件序列里判断滑音 / 颤音。
//
// 现有三个输出层都把弯音「取平均后改音高」，那是给没有弯音记号时的兜底；
// 这里不改音高，只判断形状：单调爬升/下降是滑音，来回穿越是颤音。
// 判据故意收紧（>=3 个点、幅度 >=0.4 半音），宁可漏标也不能把普通的
// vibrato 尾巴或校正用的回中事件当装饰音。
static BendMark bendMarkFor(const NoteEvent& n, const MidiData& data,
                            const std::vector<PitchBendPoint>& bends) {
    long raw = n.rawDurationTicks > 0 ? n.rawDurationTicks : n.durationTicks;
    long end = n.startTicks + std::max<long>(raw, 1);
    std::vector<double> offs;
    for (const auto& pb : bends) {
        if (pb.tick > end) break;             // bends 已按 tick 排序
        if (pb.channel == n.channel && pb.tick >= n.startTicks)
            offs.push_back((pb.value - 8192) / 8192.0 * pb.range);
    }
    if (offs.size() < 3) return BendMark::None;

    double lo = offs[0], hi = offs[0];
    for (double v : offs) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const double span = hi - lo;
    if (span < 0.4) return BendMark::None;

    int flips = 0, lastSign = 0;
    for (size_t i = 1; i < offs.size(); i++) {
        double d = offs[i] - offs[i - 1];
        if (std::abs(d) < 0.08) continue;     // 平台期不算方向变化
        int sg = d > 0 ? 1 : -1;
        if (lastSign != 0 && sg != lastSign) flips++;
        lastSign = sg;
    }
    if (flips >= 2) return BendMark::Trill;

    const double net = offs.back() - offs.front();
    if (flips <= 1 && std::abs(net) >= 0.4)
        return net > 0 ? BendMark::SlideUp : BendMark::SlideDown;
    return BendMark::None;
}

ExpressionMap::ExpressionMap(const MidiData& data) {
    if (!data.useDynamics) return;
    enabled_ = true;

    const int div = data.division > 0 ? data.division : 480;

    // 解析层已经按 tick 排过序，但单元测试直接构造 MidiData 时不保证有序，
    // 这里再排一次（stable：同 tick 后写覆盖的语义不变）。
    dyn_ = data.dynamics;
    std::stable_sort(dyn_.begin(), dyn_.end(),
                     [](const DynPoint& a, const DynPoint& b) { return a.tick < b.tick; });
    std::vector<PitchBendPoint> bends = data.pitchBends;
    std::stable_sort(bends.begin(), bends.end(),
                     [](const PitchBendPoint& a, const PitchBendPoint& b) { return a.tick < b.tick; });
    std::vector<MarkerEvent> markers = data.markers;
    std::stable_sort(markers.begin(), markers.end(),
                     [](const MarkerEvent& a, const MarkerEvent& b) { return a.tick < b.tick; });

    // ---- 1. CC11 Expression：档位变化 + 单调渐变段 ----
    // 力度是按通道的：伴奏轨的渐强不能标到旋律谱上。
    std::map<int, std::vector<DynPoint>> byChannel;
    for (const auto& p : dyn_) byChannel[p.channel].push_back(p);
    for (auto& [ch, v] : byChannel) {
        std::vector<DynPoint> u;
        for (const auto& p : v) {
            if (!u.empty() && u.back().tick == p.tick) u.back().value = p.value;
            else u.push_back(p);
        }
        // 跨档才写一次力度记号；同档内的小抖动不产生噪音。
        std::string prevLevel;
        for (const auto& p : u) {
            const std::string lv = dynamicLevel(p.value);
            if (lv != prevLevel) {
                ExpressionMark m;
                m.tick = p.tick;
                m.channel = ch;
                m.dynamic = lv;
                marks_.push_back(m);
                nDynamic_++;
                prevLevel = lv;
            }
        }
        // 连续 >=3 个点、同向、跨度 >=24（约一档）才叫渐强/渐弱。
        size_t i = 0;
        while (i + 2 < u.size()) {
            int dir = 0;
            size_t j = i + 1;
            for (; j < u.size(); j++) {
                int d = u[j].value - u[j - 1].value;
                if (d == 0) continue;
                int sg = d > 0 ? 1 : -1;
                if (dir == 0) dir = sg;
                else if (sg != dir) break;
            }
            size_t last = j - 1;
            if (last - i + 1 >= 3 && std::abs(u[last].value - u[i].value) >= 24 &&
                u[last].tick > u[i].tick) {
                ExpressionMark a;
                a.tick = u[i].tick;
                a.channel = ch;
                a.wedge = dir;
                ExpressionMark b;
                b.tick = u[last].tick;
                b.channel = ch;
                b.wedgeStop = true;
                marks_.push_back(a);
                marks_.push_back(b);
                if (dir > 0) nCresc_++; else nDim_++;
                i = last + 1;
            } else {
                i++;
            }
        }
    }

    // ---- 2. 0x06 marker：段落名 ----
    for (const auto& mk : markers) {
        // 0x06 段落名同样是文件里的裸字节。这里是它进三端之前的唯一入口：
        // 在源头上过滤，文本谱 / HTML / MusicXML 就都干净（HTML 与 XML 各自的
        // 转义层只是第二道保险，不是唯一一道）。
        std::string text = trimSectionText(dropControlChars(mk.text));
        if (text.empty()) continue;
        text = truncateUtf8Chars(text, 40);
        ExpressionMark m;
        m.tick = mk.tick;
        m.section = text;
        marks_.push_back(m);
        nSection_++;
    }

    // 同一 tick 的多个事件并成一个标记行条目，但**只在同一通道内**合并：
    // ch0 的渐弱与 ch1 的渐强合成一条时 wedge 会互相覆盖（探针
    // dynch_both 里旋律力度写 [ff]→[pp]、谱面却印 [cresc.] 就是这么来的）。
    // 全局(-1)的段落名排在同 tick 最前，保持「段落名 → 渐变」的旧输出顺序。
    std::stable_sort(marks_.begin(), marks_.end(),
                     [](const ExpressionMark& a, const ExpressionMark& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         const bool ga = (a.channel < 0), gb = (b.channel < 0);
                         if (ga != gb) return ga;
                         return false;   // 同 tick 其余保持生成顺序
                     });
    std::vector<ExpressionMark> merged;
    for (const auto& m : marks_) {
        if (!merged.empty() && merged.back().tick == m.tick &&
            merged.back().channel == m.channel) {
            ExpressionMark& b = merged.back();
            if (!m.dynamic.empty()) b.dynamic = m.dynamic;
            if (m.wedge != 0) b.wedge = m.wedge;
            b.wedgeStop = b.wedgeStop || m.wedgeStop;
            if (!m.section.empty()) b.section = m.section;
        } else {
            merged.push_back(m);
        }
    }
    marks_.swap(merged);

    // ---- 3. 音头上的演奏法 ----
    // 力度方差太小说明这份 MIDI 是「一个 velocity 写到底」的合成文件，
    // 此时不判重音：否则每个稍微响一点的音都会被印上 `>`。
    int minVel = 127, maxVel = 0;
    for (const auto& n : data.notes) {
        minVel = std::min(minVel, n.velocity);
        maxVel = std::max(maxVel, n.velocity);
    }
    const bool velVar = (maxVel - minVel) >= 24;

    std::map<int, std::vector<const NoteEvent*>> byTrack;
    for (const auto& n : data.notes) byTrack[n.track].push_back(&n);

    for (auto& [tr, ns] : byTrack) {
        (void)tr;
        std::stable_sort(ns.begin(), ns.end(),
                         [](const NoteEvent* a, const NoteEvent* b) {
                             return a->startTicks < b->startTicks;
                         });

        // ---- 断奏判据：相对该轨的「常态门限」----
        // ratio = 原始发声时长 / 到下一个音头的间隔。真实录音 MIDI 里
        // DAW 会有统一门限（常见 0.85 左右），断奏音是明显低于这个常态的
        // 少数派。绝对阈值不行：句尾「八分音符 + 八分休止」的 ratio 也有
        // 0.5，会被整首标满 `▼`；相对中位数则把这类普通短音符排除掉。
        std::vector<long> onsets, rawMax;
        for (const NoteEvent* p : ns) {
            const long r = p->rawDurationTicks > 0
                         ? p->rawDurationTicks : p->durationTicks;
            if (onsets.empty() || onsets.back() != p->startTicks) {
                onsets.push_back(p->startTicks);
                rawMax.push_back(r);
            } else {
                rawMax.back() = std::max(rawMax.back(), r);
            }
        }
        std::vector<double> ratios(onsets.size(), 1.0), samples;
        std::map<long, int> gapHist;
        for (size_t k = 0; k < onsets.size(); k++) {
            const long ioi = (k + 1 < onsets.size())
                           ? onsets[k + 1] - onsets[k]
                           : std::max<long>(rawMax[k], div);
            if (k + 1 < onsets.size()) gapHist[ioi]++;
            if (ioi >= div / 4) {
                ratios[k] = (double)rawMax[k] / (double)ioi;
                samples.push_back(ratios[k]);
            }
        }
        double base = 1.0;
        if (samples.size() >= 6) {
            std::sort(samples.begin(), samples.end());
            base = samples[samples.size() / 2];
            if (base < 0.35 || base > 1.1) base = 1.0;  // 样本异常时退回绝对判据
        }
        // 最后一道、也是最关键的一道闸门：只在「等间隔音符流」里判断奏。
        //
        // MIDI 没有任何显式的 staccato 事件，短 note-off 既可能是断奏，
        // 也可能只是「八分音符 + 休止」——信息上无法区分。但整轨音头
        // 间隔几乎一致时，谱面上不存在休止，短 gate 就只剩断奏一种解释。
        // 真实人声 MIDI 的间隔是 0.5/1/1.5/2 拍混合，这里会整轨拒绝，
        // 于是不会像早期版本那样每小节冒出一个假的 `▼`。
        bool uniform = false;
        if (!gapHist.empty() && onsets.size() >= 6) {
            int best = 0;
            for (const auto& [gap, cnt] : gapHist) {
                (void)gap;
                best = std::max(best, cnt);
            }
            uniform = best * 10 >= (int)(onsets.size() - 1) * 9;  // >= 90%
        }

        for (size_t k = 0; k < ns.size(); k++) {
            const NoteEvent& n = *ns[k];
            auto oit = std::lower_bound(onsets.begin(), onsets.end(), n.startTicks);
            const size_t oi = (size_t)(oit - onsets.begin());
            const double ratio = (oi < ratios.size()) ? ratios[oi] : 1.0;
            const double rel = base > 0 ? ratio / base : 1.0;

            NoteExpression e;
            // 三道闸门：等间隔流 + 相对常态足够短 + 绝对门限也确实短。
            if (uniform && rel <= 0.30 && ratio <= 0.40) e.staccatissimo = true;
            else if (uniform && rel <= 0.55 && ratio <= 0.60) e.staccato = true;

            if (velVar && n.velocity >= 90) {
                double sum = 0; int cnt = 0;
                const size_t lo = k > 4 ? k - 4 : 0;
                const size_t hi = std::min(ns.size(), k + 5);
                for (size_t q = lo; q < hi; q++) {
                    if (q == k) continue;
                    sum += ns[q]->velocity;
                    cnt++;
                }
                if (cnt > 0 && n.velocity - sum / cnt >= 24.0) e.accent = true;
            }
            e.bend = bendMarkFor(n, data, bends);

            if (e.any()) {
                NoteExpression& dst = notes_[{n.startTicks, n.channel}];
                dst.staccato = dst.staccato || e.staccato;
                dst.staccatissimo = dst.staccatissimo || e.staccatissimo;
                dst.accent = dst.accent || e.accent;
                if (dst.bend == BendMark::None) dst.bend = e.bend;
            }
        }

        // ---- 4. 倚音（装饰音）检测 ----
        // MIDI 没有显式的 grace note 事件，只能从「极短的前导音 + 紧邻的长音」
        // 推断。判据刻意收紧，宁可漏标也不把快速经过音、十六分音符流误判成倚音：
        //   - 前导音原始时长 < 1/8 拍（比十六分音符还短，真正的快速经过音极少这么短）
        //   - 主音原始时长 >= 1/4 拍，且至少是前导音的 4 倍长（短前长后）
        //   - 前导音的尾部紧贴主音头部（间隔在 [-1/8, 1/16] 拍内，容错重叠/量化）
        //   - 前导音与主音不同音高（同音高是倚音无意义，也会和同音合并逻辑打架）
        // 按通道判定：旋律和伴奏各自独立，跨通道误连不会出现。
        std::map<int, std::vector<const NoteEvent*>> byCh;
        for (const auto* p : ns) byCh[p->channel].push_back(p);
        for (auto& [ch, chv] : byCh) {
            (void)ch;
            std::stable_sort(chv.begin(), chv.end(),
                             [](const NoteEvent* a, const NoteEvent* b) {
                                 return a->startTicks < b->startTicks;
                             });
            for (size_t i = 1; i < chv.size(); i++) {
                const NoteEvent* prev = chv[i - 1];
                const NoteEvent* cur = chv[i];
                if (cur->pitch == prev->pitch) continue;
                const long prevRaw = prev->rawDurationTicks > 0
                                   ? prev->rawDurationTicks : prev->durationTicks;
                const long curRaw = cur->rawDurationTicks > 0
                                  ? cur->rawDurationTicks : cur->durationTicks;
                if (prevRaw <= 0 || prevRaw >= div / 8) continue;   // 前导音必须极短
                if (curRaw < div / 4) continue;                    // 主音必须够长
                if (curRaw < 4 * prevRaw) continue;                // 短前长后
                const long gap = cur->startTicks - (prev->startTicks + prevRaw);
                if (gap < -div / 8 || gap > div / 16) continue;    // 紧贴主音头部
                graceNotes_.insert({prev->startTicks, prev->channel});
                NoteExpression& dst = notes_[{cur->startTicks, cur->channel}];
                dst.gracePitch = prev->pitch;
            }
        }
    }
    for (const auto& kv : notes_) {
        const NoteExpression& e = kv.second;
        if (e.staccato || e.staccatissimo || e.accent || e.hasGrace()) nArtic_++;
        if (e.bend != BendMark::None) nBend_++;
    }
}

std::string ExpressionMap::dynamicAt(long tick, int channel) const {
    if (!enabled_) return "";
    int val = -1;
    for (const auto& dp : dyn_) {
        if (dp.tick > tick) break;
        if (dp.channel == channel) val = dp.value;
    }
    return dynamicLevel(val);
}

NoteExpression ExpressionMap::noteAt(long tick, int channel) const {
    auto it = notes_.find({tick, channel});
    return it == notes_.end() ? NoteExpression{} : it->second;
}

bool ExpressionMap::isGraceNote(long tick, int channel) const {
    return graceNotes_.find({tick, channel}) != graceNotes_.end();
}

std::vector<ExpressionMark> ExpressionMap::inRange(long from, long to,
                                                  int channel) const {
    std::vector<ExpressionMark> out;
    for (const auto& m : marks_) {
        if (m.tick < from || m.tick >= to) continue;
        // 全局标记（0x06 段落名）对每一行谱都生效；通道级标记只给所属行。
        if (channel >= 0 && m.channel >= 0 && m.channel != channel) continue;
        out.push_back(m);
    }
    return out;
}

std::vector<ExpressionMark> ExpressionMap::dynamicStepsInside(long from, long to,
                                                             int channel) const {
    std::vector<ExpressionMark> out;
    if (!enabled_) return out;

    // 先按通道收出全部渐变段（wedge 起点 -> 对应 wedgeStop）。marks_ 已按
    // tick 升序，顺序扫一遍即可配对；同一段内的中间档位由楔形表达，不再
    // 逐个印力度记号，否则每个 CC11 采样点都会多一条 <direction>。
    std::vector<std::pair<long, long>> spans;
    long openTick = -1;
    for (const auto& m : marks_) {
        if (m.channel < 0) continue;                 // 全局标记（段落名）与力度无关
        if (channel >= 0 && m.channel != channel) continue;
        if (m.wedge != 0 && openTick < 0) {
            openTick = m.tick;
        } else if (m.wedgeStop && openTick >= 0) {
            spans.push_back({openTick, m.tick});
            openTick = -1;
        }
    }

    for (const auto& m : marks_) {
        if (m.dynamic.empty() || m.channel != channel) continue;
        if (m.tick <= from || m.tick >= to) continue;   // 小节线上的换档另有口径
        bool inRamp = false;
        for (const auto& s : spans) {
            // 区间取闭：渐变起点的换档由楔形本身表达，不重复印档位。
            if (m.tick >= s.first && m.tick <= s.second) { inRamp = true; break; }
        }
        if (!inRamp) out.push_back(m);
    }
    return out;
}

std::string ExpressionMap::summary() const {
    if (!enabled_ || totalCount() == 0) return "";
    std::vector<std::string> parts;
    auto add = [&](int n, const char* label) {
        if (n > 0) parts.push_back(std::string(label) + " " + std::to_string(n));
    };
    add(nDynamic_, "力度");
    add(nCresc_, "渐强");
    add(nDim_, "渐弱");
    add(nArtic_, "断音/重音");
    add(nBend_, "滑音/颤音");
    add(nSection_, "段落");
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += " · ";
        out += parts[i];
    }
    return out;
}

} // namespace m2j

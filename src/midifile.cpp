// midifile.cpp - SMF 二进制解析实现
#include "midi2jianpu/midifile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace m2j {

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | p[1];
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}

// 可变长度值解码，pos 会被推进
static uint32_t readVarLen(const uint8_t* data, size_t size, size_t& pos) {
    uint32_t value = 0;
    while (pos < size) {
        uint8_t b = data[pos++];
        value = (value << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) break;
    }
    return value;
}

// ---- TempoTimeline ----
// 构造时把 set_tempo 序列编译成分段表。输入允许多轨重复声明 / 乱序，
// 这里统一排序、同 tick 后写覆盖、丢弃非法 BPM，下游拿到的表一定是干净的。
TempoTimeline::TempoTimeline(const std::vector<TempoPoint>& changes, int division) {
    division_ = division > 0 ? division : 480;
    std::vector<TempoPoint> v = changes;
    std::sort(v.begin(), v.end(),
              [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
    std::vector<TempoPoint> uniq;
    for (const auto& p : v) {
        if (p.bpm <= 0 || p.tick < 0) continue;
        if (!uniq.empty() && uniq.back().tick == p.tick) uniq.back() = p;
        else uniq.push_back(p);
    }

    segments_.clear();
    segments_.push_back({0, 0.0, 500000.0});  // MIDI 规范默认 120 BPM
    double sec = 0.0;
    long cur = 0;
    for (const auto& p : uniq) {
        sec += (double)(p.tick - cur) * segments_.back().usPerQuarter / 1e6 / division_;
        cur = p.tick;
        const double us = 60000000.0 / p.bpm;
        if (segments_.back().tick == cur) {
            // tick 0 的 set_tempo：改曲首段的速度，秒数仍从 0 起
            segments_.back().usPerQuarter = us;
        } else {
            segments_.push_back({cur, sec, us});
        }
    }
}

TempoTimeline::TempoTimeline(const MidiData& data)
    : TempoTimeline(data.tempoChanges, data.division) {}

double TempoTimeline::tickToSeconds(long tick) const {
    const TempoSegment& head = segments_.front();
    if (tick <= head.tick)  // 负 tick 按曲首速度线性外推
        return head.seconds + (double)(tick - head.tick) * head.usPerQuarter / 1e6 / division_;
    size_t lo = 0, hi = segments_.size();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (segments_[mid].tick <= tick) lo = mid; else hi = mid;
    }
    const TempoSegment& s = segments_[lo];
    return s.seconds + (double)(tick - s.tick) * s.usPerQuarter / 1e6 / division_;
}

long TempoTimeline::secondsToTick(double seconds) const {
    size_t lo = 0;
    // 线性扫就够：一个曲子的变速点最多几十个，二分反而容易在相等秒数上出错
    for (size_t i = 1; i < segments_.size(); i++) {
        if (segments_[i].seconds <= seconds) lo = i; else break;
    }
    const TempoSegment& s = segments_[lo];
    const double t = (double)s.tick
                   + (seconds - s.seconds) * 1e6 * division_ / s.usPerQuarter;
    return (long)std::llround(t);
}

// SMPTE 时间基准的折算。
//
// SMPTE 文件里 tick 就是绝对时间：tick / (fps × ticksPerFrame) = 秒，跟
// set_tempo 没有关系。但简谱的一切都按「拍」算（每拍 tick 数 = division），
// 所以必须挑一个速度把绝对时间换算成拍——取曲首 set_tempo（没有就 120BPM）。
// 折算完 division 就是「每拍多少 tick」，下游 tick/division 直接得到拍数，
// 三个输出层和量化逻辑一行都不用改。
//
// 随之必须做的一处归一：SMPTE 下中途的 tempo 元事件并不改变绝对时间，若把
// 那些变速点留给 TempoTimeline，秒数会被二次缩放（音符与 LRC 歌词都会错位）。
// 所以只保留曲首那一个点。
static void applySmpteTimeBase(MidiData& out) {
    if (!out.smpte) return;
    // 帧率 29 表示 29.97 丢帧；按 29.97 算才能和视频对上。
    double fps = (out.smpteFps == 29) ? 29.97 : (double)out.smpteFps;
    double ticksPerSec = fps * (double)out.smpteTicksPerFrame;
    if (!(ticksPerSec > 0.5)) {   // 折算不出来就退回普通 PPQ，别让下游除零
        out.smpte = false;
        out.division = 480;
        return;
    }

    std::stable_sort(out.tempoChanges.begin(), out.tempoChanges.end(),
                     [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
    double bpm = out.tempoChanges.empty() ? 120.0 : out.tempoChanges.front().bpm;
    if (!(bpm > 0)) bpm = 120.0;

    const double ppq = (60.0 / bpm) * ticksPerSec;   // 一拍多少 tick
    long d = std::lround(ppq);
    out.division = (d > 0) ? (int)d : 1;

    out.tempoChanges.assign(1, TempoPoint{0, bpm});
}

// format 2：每条轨是一个**独立序列**（pattern），不是同一首曲子的各声部。
//
// 旧实现不分 format，把所有轨的音符并到同一条时间轴上：两段各 4 小节的音乐
// 同时起算，小节线对不齐，两个序列里同拍发声的音还会被判成「和弦」。这里
// 只保留文件顺序里第一条含音符的轨，其余按轨（轨名 / 歌词）与通道
// （力度 / 弯音 / 音色）一并过滤，避免残留数据串味。
static void normalizeFormat2(MidiData& out, bool keepAll) {
    if (out.format != 2 || out.notes.empty()) return;

    std::map<int, size_t> perTrack;
    for (const auto& n : out.notes) perTrack[n.track]++;
    out.format2SequenceCount = (int)perTrack.size();
    if (perTrack.empty()) return;

    const int keep = perTrack.begin()->first;   // std::map 按轨号升序，即文件顺序
    out.format2UsedTrack = keep;
    if (perTrack.size() <= 1) return;           // 只有一段，没什么可丢的
    if (keepAll) return;                        // --track 要自己挑段落，先别丢

    std::set<int> channels;
    std::vector<NoteEvent> kept;
    kept.reserve(perTrack[keep]);
    for (const auto& n : out.notes) {
        if (n.track != keep) continue;
        kept.push_back(n);
        channels.insert(n.channel);
    }
    out.notes.swap(kept);

    auto dropOtherChannel = [&](auto& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const auto& p) { return channels.count(p.channel) == 0; }),
                v.end());
    };
    dropOtherChannel(out.dynamics);
    dropOtherChannel(out.pitchBends);
    dropOtherChannel(out.bendRanges);

    for (auto it = out.trackNames.begin(); it != out.trackNames.end();) {
        if (it->first != keep) it = out.trackNames.erase(it);
        else ++it;
    }
    for (auto it = out.channelProgram.begin(); it != out.channelProgram.end();) {
        if (channels.count(it->first) == 0) it = out.channelProgram.erase(it);
        else ++it;
    }
    out.midiLyrics.erase(std::remove_if(out.midiLyrics.begin(), out.midiLyrics.end(),
                                        [&](const LyricEvent& e) { return e.track != keep; }),
                         out.midiLyrics.end());
}

// RPN 变更表 → 每个弯音事件自己的范围。
//
// 必须在**所有轨都读完**、且 format 2 丢掉别的序列之后才做：RPN 常被写在
// 文件末尾的 setup / conductor 轨上，弯音事件却在文件开头的旋律轨里；
// 按解析顺序即时套用会把「后写但 tick 更早」的设定整个漏掉。
static void resolveBendRanges(MidiData& out) {
    auto& pts = out.bendRanges;
    std::stable_sort(pts.begin(), pts.end(),
                     [](const BendRangePoint& a, const BendRangePoint& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });
    out.bendRangeEvents = (int)pts.size();
    std::set<int> chans;
    for (const auto& p : pts) chans.insert(p.channel);
    out.bendRangeChannels = (int)chans.size();

    for (auto& pb : out.pitchBends) {
        double range = out.bendRange;
        for (const auto& p : pts) {
            if (p.tick > pb.tick) break;      // pts 已按 tick 升序
            if (p.channel == pb.channel) range = p.semitones;
        }
        // 坏文件里出现过 0 / 127 这种离谱值：落回默认，别把音高甩到天上
        if (!(range > 0.0) || range > 48.0) range = out.bendRange;
        pb.range = range;
    }
}

// 把 CC64 / CC66 / CC67 这类「踩下 - 抬起」踏板事件按 (tick, 文件顺序) 配成每通道的
// [踩下, 抬起) 区间。迟滞（hysteresis）：踩下判 value >= 64，抬起判 <= 32，
// 中间 33..63 保持前态；真实的连续踏板包络不会在半踩处提前断开。
// 两种踏板共用这一份实现，判定方式才不会哪天悄悄分叉——它们的差别只在
// 「区间怎么作用到音符」，不在「区间怎么算出来」。
using PedalRangeMap = std::map<int, std::vector<std::pair<long, long>>>;
static PedalRangeMap buildPedalRanges(const std::vector<PedalPoint>& src,
                                      const std::set<int>& liveChannels,
                                      long maxTick) {
    std::vector<PedalPoint> ps;
    for (const auto& p : src) if (liveChannels.count(p.channel)) ps.push_back(p);
    std::stable_sort(ps.begin(), ps.end(),
                     [](const PedalPoint& a, const PedalPoint& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });
    PedalRangeMap ranges;
    std::map<int, bool> isDown;
    std::map<int, long> onTick;
    for (const auto& p : ps) {
        if (p.value >= 64) {
            if (!isDown[p.channel]) { isDown[p.channel] = true; onTick[p.channel] = p.tick; }
        } else if (p.value <= 32) {
            if (isDown[p.channel]) {
                // 同 tick 的 127→0→127 是「重新踩踏板」，零长区间直接丢掉。
                if (p.tick > onTick[p.channel])
                    ranges[p.channel].push_back({onTick[p.channel], p.tick});
                isDown[p.channel] = false;
            }
        }
    }
    for (const auto& kv : isDown) {
        // 到曲尾都没抬踏板：用最后一个音符的结尾收束，不能让音无限拖下去。
        if (kv.second && maxTick > onTick[kv.first])
            ranges[kv.first].push_back({onTick[kv.first], maxTick});
    }
    return ranges;
}

// 控制器语义不能「跳过去就算了」。0xB0 里有四件事直接决定谱面：
//   CC64 延音踏板：琴键抬手不等于消音，音要响到踏板抬起或同键重新击打；
//   CC66 持音踏板：只保持踩下那一刻正按着的键，之后新弹的音不受影响；
//   CC67 软踏板：踩下期间起音的音符力度衰减，不动时值、不动 CC11/CC7 力度表；
//   CC120/123：强制清音，某些导出器拿它代替 note-off；
//   CC7：主音量，与 CC11 一样是渐强渐弱的来源。
// 全部轨解析完之后才做：踏板事件可能写在另一条轨上，按轨顺序处理会读成
// 「先处理轨 0 的所有未来事件，再处理轨 1 的过去事件」。
static void applyControllerSemantics(MidiData& out,
                                     const std::vector<PedalPoint>& pedal,
                                     const std::vector<PedalPoint>& sostenuto,
                                     const std::vector<PedalPoint>& softPedal,
                                     const std::vector<AllNotesOffPoint>& allOff,
                                     const std::vector<DynPoint>& cc7) {
    out.pedalEvents = (int)pedal.size();
    out.sostenutoEvents = (int)sostenuto.size();
    out.softPedalEvents = (int)softPedal.size();
    out.allNotesOffEvents = (int)allOff.size();

    // format 2 可能已经把别的段落整轨丢掉，只认还活着的通道。
    std::set<int> liveChannels;
    for (const auto& n : out.notes) liveChannels.insert(n.channel);

    // ---- 1) CC7 主音量并入力度表 ----
    // 同一 (tick, 通道) 若 CC11 也在，以表情为准：DAW 常把 CC7 当静态混音值，
    // 而 CC11 才是演奏者真正的强弱意图，两个都收会凭空多出一档力度。
    for (const auto& p : cc7) {
        if (!liveChannels.count(p.channel)) continue;
        bool covered = false;
        for (const auto& d : out.dynamics)
            if (d.tick == p.tick && d.channel == p.channel) { covered = true; break; }
        if (covered) continue;
        out.dynamics.push_back(p);
        out.cc7Dynamics++;
    }
    if (out.cc7Dynamics > 0)
        std::stable_sort(out.dynamics.begin(), out.dynamics.end(),
                         [](const DynPoint& a, const DynPoint& b) { return a.tick < b.tick; });
    if (out.notes.empty()) return;

    // ---- 2) 踏板事件按 (tick, 文件顺序) 配成每通道的 [踩下, 抬起) 区间 ----
    std::vector<AllNotesOffPoint> as;
    for (const auto& a : allOff) if (liveChannels.count(a.channel)) as.push_back(a);
    std::stable_sort(as.begin(), as.end(),
                     [](const AllNotesOffPoint& a, const AllNotesOffPoint& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });

    long maxTick = 0;
    for (const auto& n : out.notes)
        maxTick = std::max(maxTick, n.startTicks + n.durationTicks);

    const PedalRangeMap ranges = buildPedalRanges(pedal, liveChannels, maxTick);
    const PedalRangeMap sostRanges = buildPedalRanges(sostenuto, liveChannels, maxTick);
    const PedalRangeMap softRanges = buildPedalRanges(softPedal, liveChannels, maxTick);

    // ---- 3) 同键重新击打：前一个音的音响到此为止 ----
    auto keyOf = [](const NoteEvent& n) -> uint64_t {
        return ((uint64_t)(n.channel & 0xFFFF) << 24) |
               ((uint64_t)(n.pitch & 0xFF) << 8) |
               (uint64_t)(n.track & 0xFF);
    };
    std::map<uint64_t, std::vector<long>> startsByKey;
    for (const auto& n : out.notes) startsByKey[keyOf(n)].push_back(n.startTicks);
    for (auto& kv : startsByKey)
        std::sort(kv.second.begin(), kv.second.end());

    // ---- 4) 逐个音符改时值（顺带处理 CC67 的力度） ----
    for (auto& n : out.notes) {
        // CC67 软踏板：真钢琴踩左踏板是击弦变轻 / 音色变柔，按键时长不变。
        // 所以这里只衰减起音落在踩下区间内的音符 velocity——重音记号、
        // 力度变化分析和网页端导出的音符力度都读这个值。CC11 / CC7 构成的
        // 力度表属于另一条控制链，不能在这里改写，否则会凭空多一档力度。
        auto spit = softRanges.find(n.channel);
        if (spit != softRanges.end()) {
            for (const auto& rg : spit->second) {
                if (n.startTicks >= rg.first && n.startTicks < rg.second) {
                    const int softened = (n.velocity * 7 + 5) / 10;  // ×0.7，四舍五入
                    n.velocity = std::max(1, std::min(127, softened));
                    out.softPedalNotes++;
                    break;
                }
            }
        }
        const long start = n.startTicks;
        const long origEnd = start + n.durationTicks;
        long newEnd = origEnd;

        // 只有在「抬手那一刻踏板正踩着」才延长；踏板抬起之后才松键不算。
        bool sustainExtended = false;
        auto rit = ranges.find(n.channel);
        if (rit != ranges.end()) {
            for (const auto& rg : rit->second) {
                if (origEnd >= rg.first && origEnd < rg.second) {
                    newEnd = rg.second;
                    sustainExtended = true;
                    break;
                }
            }
        }
        // ---- CC66 持音踏板：只保持「踩下那一刻正按着的键」----
        // 判定方向与 CC64 正好相反：CC64 看抬手时是否踩着，CC66 看踩下时
        // 是否已按着。所以踩下之后新弹的音（start > onTick）绝不能被延长，
        // 把两种踏板合并进一条时间轴是错的。两条区间在这里取更晚的终点，
        // 后面同键重击 / 强制清音的截尾对两者一视同仁。
        bool sostHeld = false;
        auto srit = sostRanges.find(n.channel);
        if (srit != sostRanges.end()) {
            for (const auto& rg : srit->second) {
                if (start <= rg.first && origEnd > rg.first && rg.second > origEnd) {
                    sostHeld = true;
                    if (rg.second > newEnd) newEnd = rg.second;
                }
            }
        }
        if (sostHeld) out.sostenutoExtendedNotes++;
        if (newEnd > origEnd) {
            auto sit = startsByKey.find(keyOf(n));
            if (sit != startsByKey.end()) {
                for (long st : sit->second)
                    if (st > start && st < newEnd) { newEnd = st; break; }
            }
        }
        // 强制清音：只要落在音符持续区间内，不管踏板状态都截到这里。
        for (const auto& a : as) {
            if (a.channel != n.channel) continue;
            if (a.tick > start && a.tick < newEnd) { newEnd = a.tick; break; }
        }

        if (newEnd > origEnd) {
            n.durationTicks = newEnd - start;
            if (sustainExtended) out.pedalExtendedNotes++;
            // rawDurationTicks 故意不动：它是「量化前的按键时长」，断奏 / 倚音
            // 判据靠它。踏板带来的共鸣不是按键时长，写回去会把整首判成连奏。
        } else if (newEnd < origEnd) {
            n.durationTicks = std::max<long>(newEnd - start, 1);
            if (n.rawDurationTicks > n.durationTicks)
                n.rawDurationTicks = n.durationTicks;
            out.allNotesOffTrimmed++;
        }
    }
}

// Roland GS / Yamaha XG 允许把**任意** part 指定为节奏声部，也可以发
// GM/GS/XG Reset 把整台音源恢复成默认（只有通道 10 是鼓）。SMF 里这些设定
// 都是 0xF0 SysEx；旧实现「按长度整体跳过」，于是 GS/XG 文件里被设成鼓的
// 通道被当成旋律：36/38/42 这些鼓件音高转成一串 `1,, 2,, #4,,`，还会因为
// 音符最多抢走旋律轨身份，调性识别也跟着跑偏。
//
// 鼓模式本质是「(逻辑通道, 时间) → 是否鼓」的函数，而 SysEx 常被写在文件
// 末尾的 setup / conductor 轨上（tick 却是 0）。逐轨即时套用会把写在后面的
// 设定整个漏掉，也会读成「先处理轨 0 的所有未来事件，再处理轨 1 的过去
// 事件」。所以这里先收齐全部事件，按 (tick, 文件顺序) 排好，再回头删音符。
struct DrumModeEvent {
    long tick;
    int channel;     // 逻辑通道；reset 时为 -1
    bool drum;       // true=鼓 / false=普通乐器；reset 时无意义
    bool reset;
    bool fromBank;   // true=CC0/CC32 鼓组切换，false=GS/XG SysEx part mode
    int seq;
    int track;
};

// 返回 true 表示这条 SysEx 会改变鼓模式。不校验 Roland 校验和：损坏或变体
// 文件的校验和经常对不上，但地址与数据字节仍然完整可用。
static bool parseSysExDrum(const uint8_t* p, size_t len,
                           int& channel, bool& drum, bool& reset) {
    reset = false;
    drum = false;
    channel = 0;
    if (len < 5 || p[0] != 0xF0u) return false;
    // GM / GM2 System On / Off: F0 7E 7F 09 0x F7
    if (p[1] == 0x7Eu && p[3] == 0x09u &&
        (p[4] == 0x01u || p[4] == 0x02u || p[4] == 0x03u)) {
        reset = true;
        return true;
    }
    // Roland GS DT1「Use for Rhythm Part」: F0 41 1d 42 12 40 1n 15 mm cs F7
    if (p[1] == 0x41u && len >= 9 && p[3] == 0x42u &&
        p[4] == 0x12u && p[5] == 0x40u) {
        if (p[6] == 0x00u && p[7] == 0x7Fu) {   // GS Reset
            reset = true;
            return true;
        }
        if ((p[6] & 0xF0u) == 0x10u && p[7] == 0x15u) {
            channel = (int)(p[6] & 0x0Fu);
            drum = p[8] != 0u;
            return true;
        }
    }
    // Yamaha XG「Part Mode」: F0 43 1d 4C 08 pp 07 mm F7
    if (p[1] == 0x43u && len >= 8 && p[3] == 0x4Cu) {
        if (p[4] == 0x00u && p[5] == 0x00u && p[6] == 0x7Eu) {
            reset = true;                        // XG System On
            return true;
        }
        if (p[4] == 0x08u && p[6] == 0x07u) {
            channel = (int)(p[5] & 0x0Fu);
            drum = p[7] != 0u;
            return true;
        }
    }
    return false;
}

// Bank Select / Program Change 的**原始事件**。鼓组判定与音色名都必须在
// 所有轨读完后按 (tick, 文件顺序) 重放：CC0 常写在 setup 轨、0xC0 写在
// 音符轨，setup 轨甚至排在文件末尾而 tick 仍是 0；逐轨即时套用会读乱时间。
enum class RawProgramKind { BankMsb, BankLsb, Program, Reset };
struct RawProgramEvent {
    long tick;
    int channel;
    RawProgramKind kind;
    int value;
    int seq;
    int track;
};

static void resolveProgramTimeline(MidiData& out,
                                   std::vector<RawProgramEvent>& raw,
                                   std::vector<DrumModeEvent>& ev) {
    std::stable_sort(raw.begin(), raw.end(),
                     [](const RawProgramEvent& a, const RawProgramEvent& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });
    const int kMax = 4096;   // 与解析层的 kMaxLogicalChannels 一致
    std::vector<int> msb(kMax, 0), lsb(kMax, 0), progCount(kMax, 0);
    std::vector<char> drumBank(kMax, 0);
    auto pushMode = [&](const RawProgramEvent& r, bool drum) {
        DrumModeEvent e;
        e.tick = r.tick;
        e.channel = r.channel;
        e.drum = drum;
        e.reset = false;
        e.fromBank = true;
        e.seq = r.seq;
        e.track = r.track;
        ev.push_back(e);
    };
    for (const auto& r : raw) {
        if (r.channel < 0 || r.channel >= kMax) continue;
        switch (r.kind) {
        case RawProgramKind::BankMsb: msb[r.channel] = r.value; break;
        case RawProgramKind::BankLsb: lsb[r.channel] = r.value; break;
        case RawProgramKind::Reset:
            // GM/GS/XG Reset 把整台音源恢复默认：Bank 归 0、鼓组设定撤销。
            // SysEx 分支已往 ev 里放了 reset 事件，这里只补 bank 造成的
            // 状态回退；通道 10 仍是默认鼓通道。
            for (int ch = 0; ch < kMax; ++ch) {
                if (drumBank[ch]) {
                    RawProgramEvent x = r;
                    x.channel = ch;
                    pushMode(x, (ch & 0x0F) == 9);
                    drumBank[ch] = 0;
                }
                msb[ch] = 0;
                lsb[ch] = 0;
            }
            break;
        case RawProgramKind::Program: {
            const bool drum = (msb[r.channel] == 127);
            // 只有 XG 鼓组状态真的翻转才写鼓模式事件：普通 GM 文件 bank
            // 恒为 0，任何 0xC0 都不该把通道 10 从鼓改回旋律。
            if (drum != (drumBank[r.channel] != 0)) {
                pushMode(r, drum);
                drumBank[r.channel] = drum ? 1 : 0;
            }
            ProgramPoint p;
            p.tick = r.tick;
            p.channel = r.channel;
            p.program = r.value;
            p.bankMsb = msb[r.channel];
            p.bankLsb = lsb[r.channel];
            p.drum = drum;
            p.seq = r.seq;
            out.programTimeline.push_back(p);
            progCount[r.channel]++;
            break;
        }
        }
    }
    out.programChangeEvents = (int)out.programTimeline.size();
    out.programSwitchChannels = 0;
    for (int n : progCount) if (n > 1) out.programSwitchChannels++;
    std::stable_sort(out.programTimeline.begin(), out.programTimeline.end(),
                     [](const ProgramPoint& a, const ProgramPoint& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });
}

static void applySysExDrumMode(MidiData& out,
                               const std::vector<DrumModeEvent>& raw,
                               long defaultDrumNoteOn,
                               bool keepAllFormat2) {
    std::vector<DrumModeEvent> ev = raw;
    // format 2 的其它轨属于别的曲子：normalizeFormat2 会只保留第一条含音符
    // 的轨，鼓设定也必须跟着只认那一条，否则 B 段的 GS Reset 会去改 A 段
    // 的通道。本函数在 normalizeFormat2 **之前**跑（见下方调用顺序的注释），
    // 那时还没有 format2UsedTrack，于是按「含非默认鼓音符的轨」自己算一次。
    // --track 手动挑段落时不丢（调用方随后按轨过滤音符）。
    if (!keepAllFormat2 && out.format == 2 && !out.notes.empty()) {
        std::set<int> liveTracks;
        for (const auto& n : out.notes)
            if ((n.channel & 0x0F) != 9) liveTracks.insert(n.track);
        if (liveTracks.size() > 1) {
            const int keep = *liveTracks.begin();   // std::set 按轨号升序
            ev.erase(std::remove_if(ev.begin(), ev.end(),
                                    [&](const DrumModeEvent& e) {
                                        return e.track != keep;
                                    }), ev.end());
        }
    }
    std::stable_sort(ev.begin(), ev.end(),
                     [](const DrumModeEvent& a, const DrumModeEvent& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });

    out.sysExModeEvents = 0;
    out.sysExResetEvents = 0;
    out.bankModeEvents = 0;
    std::set<int> drumChannels, normalChannels, bankDrumChannels;
    for (const auto& e : ev) {
        if (e.reset) { out.sysExResetEvents++; continue; }
        if (e.fromBank) {
            out.bankModeEvents++;
            if (e.drum) bankDrumChannels.insert(e.channel);
        } else {
            out.sysExModeEvents++;
            (e.drum ? drumChannels : normalChannels).insert(e.channel);
        }
    }
    out.sysExDrumChannels = (int)drumChannels.size();
    out.sysExNormalChannels = (int)normalChannels.size();
    out.bankDrumChannels = (int)bankDrumChannels.size();

    // 时间轴：每个逻辑通道一条「(tick, 状态)」序列。默认状态就是 GM 的
    // 「通道 10 是鼓」；Reset 把已知通道一律写回各自的默认状态。
    std::map<int, std::vector<std::pair<long, bool>>> timeline;
    std::set<int> known;
    for (const auto& e : ev) {
        if (e.reset) {
            for (int ch : known)
                timeline[ch].push_back({e.tick, (ch & 0x0F) == 9});
        } else {
            known.insert(e.channel);
            timeline[e.channel].push_back({e.tick, e.drum});
        }
    }
    auto drumAt = [&](int channel, long tick) {
        bool state = (channel & 0x0F) == 9;
        auto it = timeline.find(channel);
        if (it != timeline.end()) {
            for (const auto& p : it->second) {
                if (p.first > tick) break;
                state = p.second;
            }
        }
        return state;
    };

    long removedExtra = 0, keptDefault = 0;
    out.notes.erase(
        std::remove_if(out.notes.begin(), out.notes.end(),
                       [&](const NoteEvent& n) {
                           const bool isDefaultDrum = (n.channel & 0x0F) == 9;
                           if (!drumAt(n.channel, n.startTicks)) {
                               if (isDefaultDrum) keptDefault++;
                               return false;
                           }
                           if (!isDefaultDrum) {
                               removedExtra++;
                               // GS/XG（SysEx Part Mode 或 Bank MSB 127）把非默认
                               // 鼓通道切成鼓组时，这些鼓件音在解析阶段没被计进
                               // drumNotesByTrack（那里只认通道 9 的 note-on），
                               // 于是 --list-tracks 会把整条鼓轨显示成「旋律 0 音」。
                               // 这里按轨补记；该字段只被诊断读取，转换输出不碰它。
                               // 与 removedExtra 一样依赖 remove_if 对每个元素
                               // 恰好调用一次谓词的标准保证，不会重复累加。
                               out.drumNotesByTrack[n.track]++;
                           }
                           return true;
                       }),
        out.notes.end());
    out.sysExDrumNotesSkipped = (int)removedExtra;
    // 旧实现的计数是「通道 9 的 note-on 个数」，重复触键 / 悬挂音都算在内；
    // 照抄这个数才不会让既有曲目的谱面页眉发生任何变化。通道 10 被 SysEx
    // 改回普通乐器时，扣掉那些实际留在谱里的音。
    out.drumNotesSkipped = (int)std::max<long>(0, defaultDrumNoteOn - keptDefault)
                           + (int)removedExtra;

    // 只有「本来不是鼓、却被 SysEx 设成鼓」的通道才需要清残留事件表：
    // 默认鼓通道的力度/弯音在旧实现里就是留着的，动它会破坏零回归。
    if (out.sysExModeEvents > 0 || out.bankModeEvents > 0) {
        auto dropDrumEvents = [&](auto& v) {
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const auto& p) {
                                       if ((p.channel & 0x0F) == 9) return false;
                                       return drumAt(p.channel, p.tick);
                                   }), v.end());
        };
        dropDrumEvents(out.dynamics);
        dropDrumEvents(out.pitchBends);
        dropDrumEvents(out.bendRanges);
    }
}

// 按 set_tempo 分段，把 tick 精确映射为秒。
//
// 旧实现是在解析过程中用一个可变的 tempo 变量即时换算（tick2sec 闭包），
// 有两个致命问题：
//   1) 变速曲目里，早期的音符会用到最后一次 set_tempo 的速度；
//   2) format 1 文件的 set_tempo 通常在独立的 conductor 轨（track 0），
//      如果它排在音符轨之后被解析，整份谱子的秒数全部错位。
// 现在与 LRC 歌词共用同一张 TempoTimeline，不会再出现「音符对、歌词错」。
static void recomputeSeconds(MidiData& out) {
    std::sort(out.tempoChanges.begin(), out.tempoChanges.end(),
              [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });

    const TempoTimeline tl(out.tempoChanges, out.division);
    for (auto& n : out.notes) {
        double s = tl.tickToSeconds(n.startTicks);
        n.startSeconds = s;
        n.durationSeconds = tl.tickToSeconds(n.startTicks + n.durationTicks) - s;
        if (n.durationSeconds < 0.02) n.durationSeconds = 0.02;
    }

    // out.tempo 只用来推导曲首 BPM（兼容旧接口），不是时间轴的权威来源
    out.tempo = 500000.0;
    for (const auto& p : out.tempoChanges) {
        if (p.bpm > 0) { out.tempo = 60000000.0 / p.bpm; break; }
    }
}

// 拍号事件收齐后统一整理。
//
// format 1 文件里 conductor 轨和各乐器轨常常重复声明同一个 0x58（同一个 tick、
// 同一个拍号），必须先排序去重；否则「同一时刻两个 4/4」会被下游当成变拍号。
// 另外 timeSig 固定取曲首拍号——旧实现取最后一次，是变拍号曲目全盘错位的根源。
static void normalizeMeterMap(MidiData& out) {
    auto& v = out.timeSigChanges;
    std::sort(v.begin(), v.end(),
              [](const TimeSigPoint& a, const TimeSigPoint& b) { return a.tick < b.tick; });
    std::vector<TimeSigPoint> uniq;
    uniq.reserve(v.size());
    for (const auto& p : v) {
        if (!uniq.empty() && uniq.back().tick == p.tick) uniq.back() = p;  // 同 tick 后写覆盖
        else uniq.push_back(p);
    }
    v.swap(uniq);
    if (!v.empty()) out.timeSig = v.front().sig;
    if (!out.timeSig.valid) out.timeSig = TimeSignature{4, 2, true};
}

// 调号事件收齐后统一整理。和拍号一样，format 1 的 conductor 轨与乐器轨
// 常在 tick 0 各自声明一遍同样的 0x59；同 tick 去重后，keySig 固定取曲首。
// 注意：不同 tick 上「与当前生效调号相同」的重复声明不算转调，由
// notation.cpp 的 keyChangesOf() 过滤，这里保留原始序列供调试与测试。
static void normalizeKeyMap(MidiData& out) {
    auto& v = out.keySigChanges;
    std::stable_sort(v.begin(), v.end(),
                     [](const KeySigPoint& a, const KeySigPoint& b) { return a.tick < b.tick; });
    std::vector<KeySigPoint> uniq;
    uniq.reserve(v.size());
    for (const auto& p : v) {
        if (!uniq.empty() && uniq.back().tick == p.tick) uniq.back() = p;  // 同 tick 后写覆盖
        else uniq.push_back(p);
    }
    v.swap(uniq);
    if (!v.empty()) out.keySig = v.front().sig;
}

// 在 [from, limit) 里找下一个「可信」的 MTrk 轨头。
//
// 真实的损坏 MIDI 有两种常见形态：某条 MTrk 的长度字段少算了几个字节
// （下一轮循环在轨数据中间读不到 "MTrk"，旧实现直接停掉整个 while，
// 后面的旋律轨一个音都提取不到）；或多算了下一轨的字节（把下一轨的
// 轨头当成本轨的事件流，凭空读出幻影音符）。此外 DAW 会在轨间塞私有
// 扩展 chunk。这里把标准 End of Track 当作锚点重新定位轨边界：
//   * 前 3 字节必须是 FF 2F 00（EOT，delta 任意）；
//   * 紧跟的 4 字节长度必须 >= 4 且整块落在 buffer 内。
// 两个条件把歌词 / SysEx 里恰好出现的字面 "MTrk" 排除掉，避免误切轨。
static size_t findTrackAnchor(const uint8_t* d, size_t size,
                             size_t from, size_t limit) {
    const size_t kNoAnchor = (size_t)-1;
    if (limit > size) limit = size;
    if (from >= limit || limit < 8) return kNoAnchor;
    for (size_t p = from; p + 8 <= limit; ++p) {
        if (std::memcmp(d + p, "MTrk", 4) != 0) continue;
        if (p < 3 || d[p - 3] != 0xFFu || d[p - 2] != 0x2Fu ||
            d[p - 1] != 0x00u) continue;
        const uint32_t len = rd32(d + p + 4);
        if (len < 4) continue;
        if ((size_t)len > size - (p + 8)) continue;
        return p;
    }
    return kNoAnchor;
}

// 判断 p 处是否像一个可信的 SMF chunk 头：4 字节 id 必须是可打印 ASCII
// （MTrk / XFIH / MThd / 各类 DAW 私有块都是这样），且声明长度整块落在
// buffer 内。少了 id 这一条，长度字段写短的轨尾 EOT（00 FF 2F 00）会被
// 当成私有块头，按下一轨的长度整段跳过去，旋律轨照样读不到。
static bool looksLikeChunkHeader(const uint8_t* d, size_t size, size_t p) {
    if (p + 8 > size) return false;
    for (int i = 0; i < 4; ++i)
        if (d[p + i] < 0x20 || d[p + i] > 0x7E) return false;
    return (size_t)rd32(d + p + 4) <= size - (p + 8);
}

// 核心解析：从内存buffer解析MIDI
static bool parseBuffer(const uint8_t* d, size_t size, MidiData& out,
                        bool keepAllFormat2, int trackFilter) {
    if (size < 14) return false;
    size_t pos = 0;

    // ---- Header MThd ----
    if (std::memcmp(d, "MThd", 4) != 0) return false;
    pos = 4;
    uint32_t hdrLen = rd32(d + pos); pos += 4;
    if (hdrLen < 6) return false;   // 先判长度，否则后面 pos += (hdrLen-6) 会下溢
    out.format = (int)rd16(d + pos); pos += 2;
    out.trackCount = (int)rd16(d + pos); pos += 2;
    {
        uint32_t divWord = rd16(d + pos);
        out.rawDivision = (int)divWord;
        if (divWord & 0x8000u) {
            // SMPTE 时间基准：高字节是「负帧率」的补码（0xE7 = -25），
            // 低字节是每帧 tick 数。这种文件里 tick 直接就是绝对时间，
            // 按无符号读会得到 59176 这种荒谬的 PPQ——一首 16 拍的曲子
            // 被压进 1 小节，还会因量化错乱被拆成好几个假声部。
            int fps = 256 - (int)((divWord >> 8) & 0xFFu);
            int tpf = (int)(divWord & 0xFFu);
            if (fps >= 1 && fps <= 127 && tpf > 0) {
                out.smpte = true;
                out.smpteFps = fps;
                out.smpteTicksPerFrame = tpf;
            }
        } else {
            out.division = (int)divWord;
        }
    }
    pos += 2;
    if (out.division == 0) out.division = 480;   // 防除零：非法 division 回退到 480
    pos += (hdrLen - 6);

    int trackIdx = 0;
    // value = (note-on tick, velocity)。力度要跟着音符走，演奏法分析要用。
    std::unordered_map<uint64_t, std::pair<long, int>> active;

    // 控制器事件先攒着，等所有轨都读完再统一处理：踏板 / 清音常常写在
    // conductor 轨上，而逐轨解析是按「文件顺序」不是「时间顺序」推进的。
    std::vector<PedalPoint> pedalEvents;
    std::vector<PedalPoint> sostenutoEvents;   // CC66 与 CC64 共用 ctrlSeq 定序
    std::vector<PedalPoint> softPedalEvents;   // CC67 软踏板，同样并入 ctrlSeq 定序
    std::vector<AllNotesOffPoint> allOffEvents;
    std::vector<DynPoint> volumeCc7;
    int ctrlSeq = 0;

    // SysEx 里的 GS/XG 鼓模式：先收齐，所有轨读完后按时间轴统一套用。
    std::vector<DrumModeEvent> drumModeEvents;
    // SysEx part mode、Bank Select、Program Change 共用一套序号：按
    // (tick, seq) 排序时，「先设鼓组再 Reset」「Bank 写在后面的轨」才能
    // 按文件里的真实顺序正确重放。
    std::vector<RawProgramEvent> rawProgramEvents;
    int modeSeq = 0;
    // 旧实现在解析阶段直接丢掉通道 9 的音符并累加 drumNotesSkipped。现在鼓
    // 判定统一延后（GS/XG 可以把通道 10 改回普通乐器），这里只照抄「通道 9
    // 的 note-on 个数」，让既有曲目的谱面页眉计数逐字节不变。
    long defaultDrumNoteOn = 0;

    // RPN 状态是**通道级**的，并且跨轨沿用（format 1 常把 RPN 写在 setup 轨、
    // 音符写在另一条轨）。旧实现是「每轨一对局部 int」：同一条轨上两个通道
    // 交错发 CC101/CC100/CC6 就会串味；也不能用 static，否则连续解析两份
    // 文件会互相污染。这里是 parseBuffer 的局部数组，随一次解析一起生灭。
    // 下标是「逻辑通道」port*16+ch（端口号 1 字节，最大 4095），不再是原始
    // 0-15 通道号：两台设备上都叫 ch0 时，RPN 状态不能互相覆盖。
    const int kMaxLogicalChannels = 4096;
    std::vector<int> rpnMSB(kMaxLogicalChannels, -1),
                     rpnLSB(kMaxLogicalChannels, -1),
                     rpnDataMSB(kMaxLogicalChannels, -1),
                     rpnLastSeq(kMaxLogicalChannels, -1);
    int rpnSeq = 0;
    std::vector<BendRangePoint> bendRanges;
    // 实际用到的端口集合。轨首默认 0，FF 21 追加新端口。
    std::set<int> portsUsed;

    // 把一个按下的音在 endTick 收尾。key 里已经带了 通道/音高/轨号
    // （见下面的拼法），所以这里不必再传一遍。
    auto flushNote = [&](uint64_t key, long endTick) {
        auto it = active.find(key);
        if (it == active.end()) return;
        NoteEvent ne;
        ne.startTicks = it->second.first;
        ne.durationTicks = endTick - it->second.first;
        // 轨尾悬挂音的起点可能正好落在最后一个事件上，给一个最小的十六分音符，
        // 免得时值 0 被下游量化当成「不存在的音」再丢一次。
        const long floorDur = (out.division > 0 ? out.division / 4 : 1);
        if (ne.durationTicks < 1) ne.durationTicks = floorDur;
        ne.velocity = it->second.second;
        ne.rawDurationTicks = ne.durationTicks;
        ne.pitch = (int)((key >> 8) & 0xFFu);
        ne.channel = (int)((key >> 24) & 0xFFFFu);
        ne.track = (int)(key & 0xFFu);
        out.notes.push_back(ne);
        active.erase(it);
    };

    // ---- 逐轨（按 chunk 遍历）----
    // SMF 允许 MThd 之后出现非 MTrk 的扩展 chunk（DAW 私有信息、XFIH、
    // 版权块等）。旧实现要求每个 chunk 都必须是 MTrk，只要夹一个私有块
    // 就停掉整个循环，后面的旋律轨整条丢失。这里按 chunk 通用规则跳过；
    // 长度字段本身也不可信时，退回 EOT 锚点重同步。
    while (pos + 8 <= size) {
        if (std::memcmp(d + pos, "MTrk", 4) != 0) {
            if (looksLikeChunkHeader(d, size, pos)) {
                const uint32_t otherLen = rd32(d + pos + 4);
                out.skippedChunks++;
                pos += 8 + otherLen;
                continue;
            }
            const size_t anchor = findTrackAnchor(d, size, pos, size);
            if (anchor == (size_t)-1) break;
            out.resyncedTracks++;
            pos = anchor;
            continue;
        }
        pos += 4;
        // --track / 网页段落选择：只收集指定轨的**演奏事件**。全局音源设定
        // （set_tempo、拍号、调号、SysEx 鼓模式、Bank / Program、RPN、
        // marker）仍要按整份文件读取——GS 设定和速度表几乎总写在
        // conductor / setup 轨上，按轨丢掉会把选中轨的音色与速度算错。
        // 别的轨照样完整解析，只是不往结果里塞演奏数据。
        const bool keepTrack = (trackFilter < 0 || trackIdx == trackFilter);
        uint32_t trkLen = rd32(d + pos); pos += 4;
        size_t trkEnd = pos + trkLen;
        // 长度字段写错同样是真实的损坏形态：少写会把轨尾几个字节甩到
        // 下一轮循环（memcmp 失败 → 整份文件报废），多写会把下一轨的
        // MTrk 头吞进本轨事件流。用下一个 EOT 锚定的轨头把边界校正回来。
        // 声明结尾越过文件末尾时，只有找到锚点才算「长度字段写坏」；
        // 找不到就仍是 v1.38 处理的真·截断文件，按 buffer 末尾硬边界解析。
        if (trkEnd <= size) {
            const size_t anchor = findTrackAnchor(d, size, pos, size);
            // 偏长：锚点落在声明范围内 → 本轨实际到此为止。
            // 偏短：锚点只在声明结尾后一小段内才认，且当前位置看起来
            //       不像一个带合法长度的正常 chunk，避免把整份文件
            //       都当成这一轨的残片。
            const bool boundaryLooksLikeChunk = looksLikeChunkHeader(d, size, trkEnd);
            const bool tooLong = anchor != (size_t)-1 && anchor < trkEnd;
            const bool tooShort = anchor != (size_t)-1 && anchor > trkEnd &&
                                  anchor - trkEnd <= 256 &&
                                  !boundaryLooksLikeChunk;
            if (tooLong || tooShort) {
                trkEnd = anchor;
                out.resyncedTracks++;
            }
        } else {
            // 声明结尾越过文件末尾，但后面仍可能有一整条完好的 MTrk：
            // 长度字段写坏（如 0x00010000）不等于文件被截断。旧实现在
            // 这里直接跳过重同步，把整份文件当成这一轨的残片一路解析，
            // 两轨被并成一轨、--track 1 拿不到旋律、还误报数据不完整。
            // 只要找得到 EOT 锚定的合法轨头，就按真实边界切开；找不到
            // 锚点才是 v1.38 处理的「下载到一半」真截断文件。
            const size_t anchor = findTrackAnchor(d, size, pos, size);
            if (anchor != (size_t)-1) {
                trkEnd = anchor;
                out.resyncedTracks++;
            }
        }
        // 文件可能在下载 / 拷贝途中被截断，MTrk 的长度字段却仍写着原值。
        // 解析边界一律夹到 buffer 末尾：越过 d+size 读到的不是 MIDI 数据，
        // 而是相邻内存，会被当成 delta / 状态字节继续解析，凭空多出幻影
        // 音符和乱码歌词（探针 trunc_tail.mid 曾读出 10 个音，实际只有 2 个）。
        const size_t hardEnd = std::min<size_t>(trkEnd, size);
        if (trkEnd > size) out.truncatedTracks++;

        long absTick = 0;
        uint8_t runningStatus = 0;
        // FF 21 的端口是轨内上下文，每条轨开头重置为 0：SMF 不跨轨继承端口
        // （老导出器常在 A 轨写 port 1，B 轨不写，B 仍是默认端口 0）。
        int currentPort = 0;
        portsUsed.insert(0);
        // running status 只由通道消息（0x80-0xEF）建立。旧实现把每个状态字节
        // 都记成 runningStatus，于是 0xFF（meta）/ 0xF0（SysEx）之后遇到省略
        // 状态字节的音符时，会把音符的音高字节当成 meta 类型、力度字节当成
        // 长度，事件流从那里整体错位——一句歌词之后半首曲子的音符全丢。
        // SMF 规范：meta / SysEx / System Common 取消 running status；
        // System Real-Time（0xF8-0xFE）不改变它。lastChannelStatus 只作宽容
        // 兜底：不少导出器在系统事件之后仍省略状态字节，严格跳过就整轨报废。
        uint8_t lastChannelStatus = 0;

        while (pos < hardEnd) {
            uint32_t delta = readVarLen(d, hardEnd, pos);
            absTick += (long)delta;
            // VLQ 本身可能一路吃到文件末尾；不挡住的话 b = d[pos] 就越界。
            if (pos >= hardEnd) { out.truncatedEvents++; break; }

            uint8_t b = d[pos];
            uint8_t status;
            if (b & 0x80u) {
                status = b;
                pos++;
                if (status < 0xF0u) {
                    runningStatus = status;
                    lastChannelStatus = status;
                } else if (status == 0xFFu || status == 0xF0u || status == 0xF7u ||
                           (status >= 0xF1u && status <= 0xF6u)) {
                    // 0xF1-0xF6 是 System Common，与 meta / SysEx 一样取消。
                    runningStatus = 0;
                }
                // 0xF8-0xFE：系统实时消息，保持 runningStatus 原样。
            } else {
                status = runningStatus;
                if (status == 0 && lastChannelStatus != 0) {
                    status = lastChannelStatus;
                    runningStatus = status;
                    out.runningStatusRecovered++;
                }
            }

            uint8_t type = status & 0xF0u;
            const uint8_t rawChannel = status & 0x0Fu;
            // 逻辑通道 = 端口 × 16 + 通道号。port 0 时与旧值完全相同，所以
            // 没有 FF 21 声明的文件逐字节零回归；只有声明了端口的文件才会把
            // 不同端口上复用的通道号拆成彼此独立的乐器。
            const int channel = currentPort * 16 + (int)rawChannel;

            // 读数据字节之前先确认还剩得下。通道消息固定 1~2 字节；
            // meta / SysEx 的长度写在变长字段里，由各自分支再夹一次。
            // 不够就停在本轨、保留此前读到的完整事件，绝不越过 hardEnd。
            size_t need = 0;
            if (status < 0xF0u) {
                need = (type == 0xC0u || type == 0xD0u) ? 1u : 2u;
            } else if (status == 0xFFu || status == 0xF0u || status == 0xF7u) {
                need = 1;   // meta 至少要有 type，SysEx 至少要有长度字节
            } else if (status == 0xF1u || status == 0xF3u) {
                need = 1;
            } else if (status == 0xF2u) {
                need = 2;
            }
            // 0xF6 Tune Request 与 0xF8-0xFE 系统实时消息没有数据字节。
            if (pos + need > hardEnd) { out.truncatedEvents++; break; }

            if (type == 0x90u || type == 0x80u) {
                uint8_t note = d[pos++];
                uint8_t vel = d[pos++];
                // 通道 10（index 9）默认就是打击乐：音高是鼓件编号，不是音级。
                // 简谱没有鼓谱记法，留着只会污染调性识别、和弦检测和旋律轨判定。
                // 但 GS/XG 可以把它改回普通乐器，所以这里只记 note-on 个数，
                // 真正丢不丢留到 applySysExDrumMode 按时间轴决定——那时所有轨
                // 的 SysEx 都读齐了，设定写在文件末尾也能生效。
                if (rawChannel == 9 && type == 0x90u && vel > 0) {
                    // defaultDrumNoteOn 仍只统计当前选中轨（keepTrack），以保住
                    // --track 模式下 drumNotesSkipped 的旧语义；鼓轨列表用的
                    // drumNotesByTrack 则按所有轨统计，方便 --list-tracks 把每条
                    // 鼓轨都标出来。
                    if (keepTrack) defaultDrumNoteOn++;
                    out.drumNotesByTrack[trackIdx]++;
                }
                {
                    uint64_t key = ((uint64_t)channel << 24) |
                                   ((uint64_t)note << 8) | (uint64_t)trackIdx;
                    if (type == 0x90u && vel > 0) {
                        // 同一个键还没抬手就又按下（重复触键、转录软件、踏板踩法
                        // 都会这样）：旧实现用 active[key] = ... 直接覆盖，前一个
                        // 音凭空消失，调性判断也跟着少一个音。现在把它收到此刻，
                        // 再以新力度重新起音。
                        auto it = active.find(key);
                        if (it != active.end()) {
                            // 同一 tick 重复按下只是重复通知，前一个音时长为 0，
                            // 直接换掉；凭空补一个十六分音符反而会多出一个音。
                            if (absTick > it->second.first) flushNote(key, absTick);
                            else active.erase(it);
                        }
                        active[key] = {absTick, (int)vel};
                    } else {
                        flushNote(key, absTick);
                    }
                }
            } else if (status == 0xFFu) {
                if (pos >= hardEnd) break;
                uint8_t metaType = d[pos++];
                uint32_t metaLen = readVarLen(d, hardEnd, pos);
                // 越界保护：损坏文件里 meta 长度可能超出轨道范围。
                // 整份文件已被截断时（trkEnd > size）这条事件本身就是残骸：
                // 夹紧后把残字节当歌词读会多出乱码音节，直接丢弃并停在本轨。
                if (pos + metaLen > hardEnd) {
                    out.truncatedEvents++;
                    if (trkEnd > size) break;
                    metaLen = (hardEnd > pos) ? (uint32_t)(hardEnd - pos) : 0;
                }
                if (metaType == 0x21u && metaLen >= 1) {  // MIDI Port / Cable Number
                    // 从本条 meta 起，本轨的通道消息都送到这个端口。
                    currentPort = (int)d[pos];
                    portsUsed.insert(currentPort);
                    out.midiPortEvents++;
                } else if (metaType == 0x20u && metaLen >= 1) {  // Channel Prefix
                    // SMF 规范：它只决定**其后 meta / SysEx** 关联到哪个通道，
                    // 不重定向通道消息。当前 meta 一律按 trackIdx 归属，所以
                    // 这里只记数并在页眉如实说明，不能拿它改写 channel——
                    // 那会把 port 0 ch0 的旋律错记成 ch2 的乐器。
                    out.channelPrefixEvents++;
                } else if (metaType == 0x51u && metaLen >= 3) {  // set_tempo
                    // 只记下 tick 与 BPM，秒数在全部事件解析完后统一换算
                    long us = (long)((d[pos] << 16) | (d[pos+1] << 8) | d[pos+2]);
                    if (us > 0) out.tempoChanges.push_back({absTick, 60000000.0 / (double)us});
                } else if (metaType == 0x58u && metaLen >= 4) {  // time signature
                    TimeSignature ts;
                    ts.numerator = d[pos];
                    ts.denomPow = d[pos+1];
                    ts.valid = true;
                    // 分母指数 0..8（二分音符到 256 分音符），超范围多半是坏数据
                    if (ts.numerator > 0 && ts.denomPow >= 0 && ts.denomPow <= 8) {
                        out.timeSigChanges.push_back({absTick, ts});
                        if (!out.timeSig.valid) out.timeSig = ts;
                    }
                } else if (metaType == 0x59u && metaLen >= 2) {  // key signature
                    KeySignature ks;
                    ks.sharpsFlats = (int8_t)d[pos];
                    ks.isMinor = d[pos+1] != 0;
                    ks.valid = true;
                    // 记全序列而不是覆盖：一首歌中途可以转调（0x59 出现在
                    // 副歌/桥段的小节线上），曲首之外的点由 KeyMap 消费。
                    if (ks.sharpsFlats >= -7 && ks.sharpsFlats <= 7)
                        out.keySigChanges.push_back({absTick, ks});
                } else if (metaType == 0x03u) {  // track name
                    std::string name((const char*)&d[pos], metaLen);
                    out.trackNames[trackIdx] = name;
                } else if (metaType == 0x04u) {  // instrument name
                    std::string name((const char*)&d[pos], metaLen);
                    // 存到轨道名称中，如果轨道名为空
                    if (out.trackNames[trackIdx].empty()) {
                        out.trackNames[trackIdx] = name;
                    }
                } else if (metaType == 0x05u) {  // lyric event
                    std::string lyric((const char*)&d[pos], metaLen);
                    // 保留空事件：很多文件用空 lyric 表示换行/段落，规整阶段再决定
                    if (keepTrack) out.midiLyrics.push_back({absTick, lyric, trackIdx});
                } else if (metaType == 0x01u) {  // text event
                    std::string text((const char*)&d[pos], metaLen);
                    // 只存非空的文本
                    if (!text.empty() && text.length() < 100) {
                        out.textEvents.push_back({absTick, text});
                    }
                } else if (metaType == 0x06u) {  // marker
                    std::string marker((const char*)&d[pos], metaLen);
                    if (!marker.empty()) {
                        out.markers.push_back({absTick, marker});
                    }
                }
                pos += metaLen;
            } else if (type == 0xB0u) {
                uint8_t ctrl = d[pos++];
                uint8_t val = d[pos++];
                // 演奏事件按轨隔离；下面的 Bank Select 与 RPN 是音源状态，
                // 常写在别的 setup 轨上，必须继续按整份文件收集。
                if (keepTrack) {
                    if (ctrl == 0x0Bu) {  // Expression
                        out.dynamics.push_back({absTick, (int)val, (int)channel});
                    } else if (ctrl == 0x07u) {  // 主音量：和 CC11 一样承载渐强渐弱
                        volumeCc7.push_back({absTick, (int)val, (int)channel});
                    } else if (ctrl == 0x40u) {  // CC64 延音踏板，存原始值，迟滞判定后做
                        pedalEvents.push_back({absTick, (int)channel, (int)val, ctrlSeq++});
                    } else if (ctrl == 0x42u) {  // CC66 持音踏板（Sostenuto）
                        sostenutoEvents.push_back({absTick, (int)channel, (int)val, ctrlSeq++});
                    } else if (ctrl == 0x43u) {  // CC67 软踏板（Soft Pedal）
                        softPedalEvents.push_back({absTick, (int)channel, (int)val, ctrlSeq++});
                    } else if (ctrl == 0x78u || ctrl == 0x7Bu) {  // All Sound/Notes Off
                        allOffEvents.push_back({absTick, (int)channel, ctrlSeq++});
                    }
                }
                // CC0 Bank MSB / CC32 Bank LSB：XG 用 MSB 127 指定鼓组，
                // GS/XG 也靠它选变体音色。旧实现把这两个控制器当普通两字节
                // 跳过，鼓件音高于是全被当成旋律。先攒着，全部轨读完后按
                // 时间轴重放（setup 轨常排在文件末尾而 tick 是 0）。
                if (ctrl == 0x00u || ctrl == 0x20u) {
                    out.bankSelectEvents++;
                    RawProgramEvent r;
                    r.tick = absTick;
                    r.channel = (int)channel;
                    r.kind = (ctrl == 0x00u) ? RawProgramKind::BankMsb
                                             : RawProgramKind::BankLsb;
                    r.value = (int)val;
                    r.seq = modeSeq++;
                    r.track = trackIdx;
                    rawProgramEvents.push_back(r);
                }
                // ---- RPN 0x0000：弯音灵敏度（每通道、可中途改）----
                // 标准序列 CC101(0x65)=0、CC100(0x64)=0、CC6(0x06)=半音数，
                // 可选 CC38(0x26)=音分微调；很多导出器随后用 127/127 退出 RPN，
                // 这里照收即可（MSB/LSB 变成 127 后 CC6 自然不再匹配）。
                if (ctrl == 0x65u) rpnMSB[channel] = (int)val;
                else if (ctrl == 0x64u) rpnLSB[channel] = (int)val;
                else if (ctrl == 0x06u && rpnMSB[channel] == 0 && rpnLSB[channel] == 0) {
                    rpnDataMSB[channel] = (int)val;
                    rpnLastSeq[channel] = rpnSeq;
                    bendRanges.push_back({absTick, (int)channel, (double)val, rpnSeq++});
                } else if (ctrl == 0x26u && rpnMSB[channel] == 0 && rpnLSB[channel] == 0 &&
                           rpnDataMSB[channel] >= 0 && rpnLastSeq[channel] >= 0) {
                    // CC38 把上一次 CC6 精修到音分（100 音分 = 1 半音）。
                    for (auto& p : bendRanges) {
                        if (p.seq == rpnLastSeq[channel]) {
                            p.semitones = (double)rpnDataMSB[channel] + (double)val / 100.0;
                            break;
                        }
                    }
                }
            } else if (type == 0xE0u) {
                uint8_t lsb = d[pos++];
                uint8_t msb = d[pos++];
                int bend = (msb << 7) | lsb;
                if (keepTrack) out.pitchBends.push_back({absTick, bend, (int)channel});
            } else if (status == 0xF0u || status == 0xF7u) {
                // SysEx：按长度整体跳过，否则会把后续事件错位解析。
                // 但 GS/XG 的「Use for Rhythm Part / Part Mode」与 GM/GS/XG
                // Reset 就藏在这些字节里，先看一眼再跳。
                uint32_t sxLen = readVarLen(d, hardEnd, pos);
                // hardEnd 已提到轨级（原先只有 SysEx 分支自己夹了一次），
                // meta / 通道消息现在同样不会越过 buffer 末尾。
                if (pos + sxLen > hardEnd) {
                    out.truncatedEvents++;
                    if (trkEnd > size) break;
                    sxLen = (uint32_t)(hardEnd > pos ? hardEnd - pos : 0);
                }
                if (status == 0xF0u && sxLen > 0) {
                    out.sysExEvents++;
                    int sxCh = 0;
                    bool sxDrum = false, sxReset = false;
                    if (parseSysExDrum(d + pos, sxLen, sxCh, sxDrum, sxReset)) {
                        DrumModeEvent e;
                        e.tick = absTick;
                        e.channel = sxReset ? -1 : currentPort * 16 + sxCh;
                        e.drum = sxDrum;
                        e.reset = sxReset;
                        e.seq = modeSeq++;
                        e.track = trackIdx;
                        e.fromBank = false;
                        drumModeEvents.push_back(e);
                        if (sxReset) {
                            // Reset 同时把各通道的 Bank 状态清回默认，
                            // 否则撤销鼓组后下一个 0xC0 还会读成鼓。
                            RawProgramEvent r;
                            r.tick = absTick;
                            r.channel = 0;
                            r.kind = RawProgramKind::Reset;
                            r.value = 0;
                            r.seq = e.seq;
                            r.track = trackIdx;
                            rawProgramEvents.push_back(r);
                        }
                    }
                }
                pos += sxLen;
            } else if (type == 0xC0u) {
                // 音色号（GM program change）。旧实现直接跳过，于是「这条轨是
                // 笛子/二胡/钢琴」这种对读谱极有用的信息被丢掉了；现在按通道
                // 记下来，输出层据此给每条声部标出乐器名。
                const int progValue = (int)d[pos++];
                out.channelProgram[channel] = progValue;
                RawProgramEvent r;
                r.tick = absTick;
                r.channel = (int)channel;
                r.kind = RawProgramKind::Program;
                r.value = progValue;
                r.seq = modeSeq++;
                r.track = trackIdx;
                rawProgramEvents.push_back(r);
            } else if (type == 0xA0u) {
                // 复音触后：状态 + 音高 + 压力 = 3 字节。旧实现落到末尾的
                // 兜底 pos += 1，每遇到一个就少读一个字节，事件流从那里开始
                // 整体错位——一条 16 个音的轨最后连一个音都提取不出来。
                // 简谱没有触后记号，正确跳过即可。
                pos += 2;
            } else if (type == 0xD0u) {   // 通道触后：只有 1 个数据字节
                pos += 1;
            } else if (status == 0xF1u || status == 0xF3u) {
                pos += 1;                 // MTC quarter frame / song select
            } else if (status == 0xF2u) {
                pos += 2;                 // song position pointer
            } else if (status >= 0xF6u) {
                // 0xF6 起是系统实时消息，没有数据字节；按 1 字节跳过会把后面
                // 的 delta 时间一起吃掉。
            } else {
                pos += 1;
            }
        }
        pos = trkEnd;
        // 轨结束了还按着没抬手的音：旧实现整条丢掉（开头那个探针里少掉的
        // 音甚至把调性从 C 大调带偏成 F / D 小调）。按本轨最后一个事件的
        // 位置收尾。
        while (!active.empty()) flushNote(active.begin()->first, absTick);
        trackIdx++;
    }

    std::sort(out.notes.begin(), out.notes.end(),
              [](const NoteEvent& a, const NoteEvent& b) {
                  if (a.startTicks != b.startTicks) return a.startTicks < b.startTicks;
                  return a.pitch < b.pitch;
              });
    // format 1 里各轨是按文件顺序解析的：conductor 轨若排在最后，它携带的
    // 力度 / 弯音事件就会全部堆在数组尾部。下游按 tick 推进游标时要求升序，
    // 所以这三张表都要像 tempo / 拍号 / 调号一样在收齐后统一排序。
    // 用 stable_sort：同 tick 保持「文件里后写覆盖先写」的解析顺序。
    auto byTickStable = [](const auto& a, const auto& b) { return a.tick < b.tick; };
    std::stable_sort(out.dynamics.begin(), out.dynamics.end(), byTickStable);
    std::stable_sort(out.pitchBends.begin(), out.pitchBends.end(), byTickStable);
    std::stable_sort(out.markers.begin(), out.markers.end(), byTickStable);
    out.bendRanges = std::move(bendRanges);
    normalizeMeterMap(out);
    normalizeKeyMap(out);

    // ---- 按轨隔离（--track / 网页段落选择）----
    // 必须排在踏板 / 力度 / 清音的语义修正**之前**：那些处理会按「还活着
    // 的通道」把别的轨的踏板、CC11 并进谱面，等 parseBuffer 返回再丢音符，
    // 谱面时值早就被别轨的控制器改过了。演奏事件表已在解析循环里按轨
    // 门控，这里只丢音符，并记下 format 2 的**全文件**段数。
    int f2SeqBefore = 0;
    if (out.format == 2 && trackFilter >= 0) {
        std::set<int> seqTracks;
        for (const auto& n : out.notes) seqTracks.insert(n.track);
        f2SeqBefore = (int)seqTracks.size();
    }
    if (trackFilter >= 0) {
        out.selectedTrack = trackFilter;
        std::vector<NoteEvent> kept;
        kept.reserve(out.notes.size());
        for (auto& n : out.notes) {
            if (n.track == trackFilter) kept.push_back(n);
            else out.trackFilteredNotes++;
        }
        out.notes.swap(kept);
    }

    // 鼓过滤必须排在 normalizeFormat2 **之前**：通道 9 的音符现在会先收进
    // notes 再统一删，若让 format 2 先按轨统计，一条只有鼓的轨会被当成
    // 「第一段」，整首曲子被判成只有打击乐。先删鼓音，段判定看到的音符
    // 集合就与旧实现完全一致。
    // 先重放 Bank/Program（会往 drumModeEvents 里补 XG 鼓组切换），
    // 再统一套用鼓判定；两者共用 (tick, seq) 时间轴。
    resolveProgramTimeline(out, rawProgramEvents, drumModeEvents);
    applySysExDrumMode(out, drumModeEvents, defaultDrumNoteOn, keepAllFormat2);
    if (trackFilter >= 0) {
        // format 2 的段数要按**整份文件**报，不能按隔离后只剩一轨的
        // notes 重算，否则「共 N 段」的提示会凭空消失。
        if (out.format == 2) {
            out.format2SequenceCount = f2SeqBefore;
            out.format2UsedTrack = trackFilter;
        }
    } else {
        normalizeFormat2(out, keepAllFormat2);
    }
    resolveBendRanges(out);
    applyControllerSemantics(out, pedalEvents, sostenutoEvents, softPedalEvents,
                             allOffEvents, volumeCc7);
    applySmpteTimeBase(out);
    out.midiPortCount = (int)portsUsed.size();
    out.midiPortMax = portsUsed.empty() ? 0 : *portsUsed.rbegin();
    recomputeSeconds(out);
    return true;
}

bool loadMidiFile(const std::string& path, MidiData& out,
                  bool keepAllFormat2, int trackFilter) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return parseBuffer(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), out,
                       keepAllFormat2, trackFilter);
}

bool loadMidiFromMemory(const uint8_t* data, size_t size, MidiData& out,
                        bool keepAllFormat2, int trackFilter) {
    return parseBuffer(data, size, out, keepAllFormat2, trackFilter);
}

// 这两个说明文本放在解析层而不是输出层：它们是「文件头怎么读的」这一事实，
// CLI 提示、HTML 页眉、WASM 三处都要用，写在一处才不会各说各话。
std::string timeBaseLabel(const MidiData& data) {
    if (!data.smpte) return "";
    const std::string fps = (data.smpteFps == 29) ? "29.97"
                                                  : std::to_string(data.smpteFps);
    const double bpm = (data.tempo > 0) ? 60000000.0 / data.tempo : 120.0;
    std::ostringstream os;
    os << "时间基准 SMPTE " << fps << " fps × " << data.smpteTicksPerFrame
       << " tick → " << data.division << " tick/拍（按 ♩="
       << std::lround(bpm) << " 折算）";
    return os.str();
}

std::string controllerLabel(const MidiData& data) {
    std::vector<std::string> parts;
    if (data.pedalExtendedNotes > 0)
        parts.push_back("延音踏板：补长 " + std::to_string(data.pedalExtendedNotes) +
                        " 个音（" + std::to_string(data.pedalEvents) + " 个踏板事件）");
    if (data.sostenutoExtendedNotes > 0)
        parts.push_back("持音踏板：保持 " + std::to_string(data.sostenutoExtendedNotes) +
                        " 个音（" + std::to_string(data.sostenutoEvents) + " 个事件）");
    if (data.softPedalNotes > 0)
        parts.push_back("软踏板：衰减 " + std::to_string(data.softPedalNotes) +
                        " 个音的力度（" + std::to_string(data.softPedalEvents) + " 个事件）");
    if (data.allNotesOffTrimmed > 0)
        parts.push_back("强制清音：截尾 " + std::to_string(data.allNotesOffTrimmed) +
                        " 个音（" + std::to_string(data.allNotesOffEvents) + " 个事件）");
    if (data.cc7Dynamics > 0)
        parts.push_back("CC7 主音量：补入 " + std::to_string(data.cc7Dynamics) +
                        " 个力度点");
    if (parts.empty()) return "";
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += " ｜ ";
        out += parts[i];
    }
    return out;
}

std::string bendRangeLabel(const MidiData& data) {
    if (data.bendRangeEvents <= 0) return "";
    std::vector<double> vals;
    for (const auto& p : data.bendRanges) {
        const double v = std::round(p.semitones * 100.0) / 100.0;
        if (std::find(vals.begin(), vals.end(), v) == vals.end()) vals.push_back(v);
    }
    std::sort(vals.begin(), vals.end());
    std::ostringstream os;
    os << "弯音范围：";
    for (size_t i = 0; i < vals.size(); i++) {
        if (i) os << " / ";
        const long tenths = std::lround(vals[i] * 10.0);
        if (tenths % 10 == 0) os << "±" << (tenths / 10);
        else os << "±" << (tenths / 10) << "." << (tenths % 10);
    }
    os << " 半音，按通道与时间生效（" << data.bendRangeEvents << " 处设定，"
       << data.bendRangeChannels << " 条通道）";
    return os.str();
}

std::string runningStatusLabel(const MidiData& data) {
    if (data.runningStatusRecovered <= 0) return "";
    return "省略状态字节：系统事件之后按上一条通道状态补回 " +
           std::to_string(data.runningStatusRecovered) + " 处（否则音符会整段丢失）";
}

std::string portLabel(const MidiData& data) {
    if (data.midiPortEvents <= 0 && data.channelPrefixEvents <= 0) return "";
    std::vector<std::string> parts;
    if (data.midiPortEvents > 0) {
        std::ostringstream os;
        os << "MIDI Port：" << data.midiPortEvents << " 处声明，共 "
           << data.midiPortCount << " 个端口（0～" << data.midiPortMax
           << "），同通道号跨端口已隔离";
        parts.push_back(os.str());
    }
    if (data.channelPrefixEvents > 0)
        parts.push_back("通道前缀：" + std::to_string(data.channelPrefixEvents) +
                        " 处（按规范不影响通道消息）");
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += " ｜ ";
        out += parts[i];
    }
    return out;
}

std::string sysExLabel(const MidiData& data) {
    if (data.sysExModeEvents <= 0) return "";
    std::ostringstream os;
    os << "SysEx 鼓模式：" << data.sysExModeEvents << " 处 GS/XG 设定";
    if (data.sysExDrumChannels > 0)
        os << "，" << data.sysExDrumChannels << " 条鼓通道";
    if (data.sysExNormalChannels > 0)
        os << "，" << data.sysExNormalChannels << " 条恢复普通乐器";
    if (data.sysExResetEvents > 0)
        os << "，" << data.sysExResetEvents << " 处 GM/GS/XG Reset 恢复默认";
    if (data.sysExDrumNotesSkipped > 0)
        os << "，已跳过 " << data.sysExDrumNotesSkipped << " 个鼓件音";
    return os.str();
}

int programAt(const MidiData& data, int channel, long tick) {
    int result = -1;
    for (const auto& p : data.programTimeline) {
        if (p.tick > tick) break;      // 全表按 tick 升序
        if (p.channel == channel) result = p.program;
    }
    if (result >= 0) return result;
    auto it = data.channelProgram.find(channel);
    return it == data.channelProgram.end() ? -1 : it->second;
}

std::string bankLabel(const MidiData& data) {
    // 普通 GM 文件只有 0xC0、没有 CC0/CC32：不显示这一行，既有谱面页眉
    // 才能逐字节不变。
    if (data.bankSelectEvents <= 0 && data.bankModeEvents <= 0) return "";
    std::ostringstream os;
    os << "音色库/音色设定：" << data.bankSelectEvents << " 处 Bank Select，"
       << data.programChangeEvents << " 次 Program Change";
    if (data.bankDrumChannels > 0)
        os << "，" << data.bankDrumChannels << " 条 XG 鼓组通道";
    if (data.programSwitchChannels > 0)
        os << "，" << data.programSwitchChannels << " 条通道中途换音色（乐器名按时间取）";
    return os.str();
}

std::string trackLabel(const MidiData& data) {
    if (data.selectedTrack < 0) return "";
    std::ostringstream os;
    os << "按轨隔离：只转换轨 " << data.selectedTrack;
    auto it = data.trackNames.find(data.selectedTrack);
    if (it != data.trackNames.end() && !it->second.empty())
        os << "「" << it->second << "」";
    os << "，该轨以外的歌词 / 力度 / 踏板 / 弯音事件已忽略";
    if (data.trackFilteredNotes > 0)
        os << "（另丢弃 " << data.trackFilteredNotes << " 个音符）";
    return os.str();
}

// 文件级结构提示：数据被截断（v1.38）与 chunk 边界被修复（v1.42）合在
// 一处输出，CLI / HTML 页眉 / 网页文件面板三端自动同步。合法文件返回空串。
std::string truncationLabel(const MidiData& data) {
    const bool damaged = data.truncatedTracks > 0 || data.truncatedEvents > 0;
    const bool repaired = data.skippedChunks > 0 || data.resyncedTracks > 0;
    if (!damaged && !repaired) return "";
    std::ostringstream os;
    if (damaged) {
        os << "文件数据不完整：";
        if (data.truncatedTracks > 0)
            os << data.truncatedTracks << " 条轨道在声明结尾前被截断";
        else
            os << "轨尾有未写完的事件";
        os << "，已按实际字节生成谱面";
        if (data.truncatedEvents > 0)
            os << "（丢弃 " << data.truncatedEvents << " 个不完整事件）";
    }
    if (repaired) {
        if (damaged) os << "；";
        os << "文件结构提示：";
        if (data.skippedChunks > 0)
            os << "跳过 " << data.skippedChunks << " 个非 MTrk 扩展块";
        if (data.skippedChunks > 0 && data.resyncedTracks > 0) os << "、";
        if (data.resyncedTracks > 0)
            os << "重同步 " << data.resyncedTracks << " 条轨道边界（长度字段与实际不符）";
        os << "，谱面按真实轨数据生成";
    }
    return os.str();
}

std::string formatLabel(const MidiData& data) {
    if (data.format != 2) return "";
    if (data.format2SequenceCount <= 1) return "";
    std::string s = "format 2（多个独立序列）：共 " +
                    std::to_string(data.format2SequenceCount) + " 段，只转换轨 " +
                    std::to_string(data.format2UsedTrack);
    auto it = data.trackNames.find(data.format2UsedTrack);
    if (it != data.trackNames.end() && !it->second.empty())
        s += "「" + it->second + "」";
    s += "，其余段落属别的曲子";
    return s;
}

} // namespace m2j

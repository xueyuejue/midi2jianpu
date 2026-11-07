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

// 控制器语义不能「跳过去就算了」。0xB0 里有三件事直接决定谱面：
//   CC64 踏板：琴键抬手不等于消音，音要响到踏板抬起或同键重新击打；
//   CC120/123：强制清音，某些导出器拿它代替 note-off；
//   CC7：主音量，与 CC11 一样是渐强渐弱的来源。
// 全部轨解析完之后才做：踏板事件可能写在另一条轨上，按轨顺序处理会读成
// 「先处理轨 0 的所有未来事件，再处理轨 1 的过去事件」。
static void applyControllerSemantics(MidiData& out,
                                     const std::vector<PedalPoint>& pedal,
                                     const std::vector<AllNotesOffPoint>& allOff,
                                     const std::vector<DynPoint>& cc7) {
    out.pedalEvents = (int)pedal.size();
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
    std::vector<PedalPoint> ps;
    for (const auto& p : pedal) if (liveChannels.count(p.channel)) ps.push_back(p);
    std::stable_sort(ps.begin(), ps.end(),
                     [](const PedalPoint& a, const PedalPoint& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return a.seq < b.seq;
                     });
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

    std::map<int, std::vector<std::pair<long, long>>> ranges;  // 通道 -> 踏板区间
    std::map<int, bool> isDown;
    std::map<int, long> onTick;
    for (const auto& p : ps) {
        if (p.down) {
            if (!isDown[p.channel]) { isDown[p.channel] = true; onTick[p.channel] = p.tick; }
        } else if (isDown[p.channel]) {
            // 同 tick 的 127→0→127 是「重新踩踏板」，零长区间直接丢掉。
            if (p.tick > onTick[p.channel])
                ranges[p.channel].push_back({onTick[p.channel], p.tick});
            isDown[p.channel] = false;
        }
    }
    for (const auto& kv : isDown) {
        // 到曲尾都没抬踏板：用最后一个音符的结尾收束，不能让音无限拖下去。
        if (kv.second && maxTick > onTick[kv.first])
            ranges[kv.first].push_back({onTick[kv.first], maxTick});
    }

    // ---- 3) 同键重新击打：前一个音的音响到此为止 ----
    auto keyOf = [](const NoteEvent& n) -> uint64_t {
        return ((uint64_t)(n.channel & 0x0F) << 24) |
               ((uint64_t)(n.pitch & 0xFF) << 8) |
               (uint64_t)(n.track & 0xFF);
    };
    std::map<uint64_t, std::vector<long>> startsByKey;
    for (const auto& n : out.notes) startsByKey[keyOf(n)].push_back(n.startTicks);
    for (auto& kv : startsByKey)
        std::sort(kv.second.begin(), kv.second.end());

    // ---- 4) 逐个音符改时值 ----
    for (auto& n : out.notes) {
        const long start = n.startTicks;
        const long origEnd = start + n.durationTicks;
        long newEnd = origEnd;

        // 只有在「抬手那一刻踏板正踩着」才延长；踏板抬起之后才松键不算。
        auto rit = ranges.find(n.channel);
        if (rit != ranges.end()) {
            for (const auto& rg : rit->second) {
                if (origEnd >= rg.first && origEnd < rg.second) {
                    newEnd = rg.second;
                    break;
                }
            }
        }
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
            out.pedalExtendedNotes++;
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

// 核心解析：从内存buffer解析MIDI
static bool parseBuffer(const uint8_t* d, size_t size, MidiData& out,
                        bool keepAllFormat2) {
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
    std::vector<AllNotesOffPoint> allOffEvents;
    std::vector<DynPoint> volumeCc7;
    int ctrlSeq = 0;

    // RPN 状态是**通道级**的，并且跨轨沿用（format 1 常把 RPN 写在 setup 轨、
    // 音符写在另一条轨）。旧实现是「每轨一对局部 int」：同一条轨上两个通道
    // 交错发 CC101/CC100/CC6 就会串味；也不能用 static，否则连续解析两份
    // 文件会互相污染。这里是 parseBuffer 的局部数组，随一次解析一起生灭。
    int rpnMSB[16], rpnLSB[16], rpnDataMSB[16], rpnLastSeq[16];
    for (int i = 0; i < 16; i++) {
        rpnMSB[i] = -1; rpnLSB[i] = -1; rpnDataMSB[i] = -1; rpnLastSeq[i] = -1;
    }
    int rpnSeq = 0;
    std::vector<BendRangePoint> bendRanges;

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
        ne.channel = (int)((key >> 24) & 0x0Fu);
        ne.track = (int)(key & 0xFFu);
        out.notes.push_back(ne);
        active.erase(it);
    };

    // ---- 逐轨 ----
    while (pos + 8 <= size && std::memcmp(d + pos, "MTrk", 4) == 0) {
        pos += 4;
        uint32_t trkLen = rd32(d + pos); pos += 4;
        size_t trkEnd = pos + trkLen;

        long absTick = 0;
        uint8_t runningStatus = 0;

        while (pos < trkEnd) {
            uint32_t delta = readVarLen(d, size, pos);
            absTick += (long)delta;

            uint8_t b = d[pos];
            uint8_t status;
            if (b & 0x80u) {
                status = b;
                runningStatus = b;
                pos++;
            } else {
                status = runningStatus;
            }

            uint8_t type = status & 0xF0u;
            uint8_t channel = status & 0x0Fu;

            if (type == 0x90u || type == 0x80u) {
                uint8_t note = d[pos++];
                uint8_t vel = d[pos++];
                // 通道 10（index 9）是打击乐：音高是鼓件编号，不是音级。
                // 简谱没有鼓谱记法，留着只会污染调性识别、和弦检测和旋律轨判定。
                if (channel == 9) {
                    if (type == 0x90u && vel > 0) out.drumNotesSkipped++;
                } else {
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
                if (pos >= trkEnd) break;
                uint8_t metaType = d[pos++];
                uint32_t metaLen = readVarLen(d, size, pos);
                // 越界保护：损坏文件里 meta 长度可能超出轨道范围
                if (pos + metaLen > trkEnd) {
                    metaLen = (trkEnd > pos) ? (uint32_t)(trkEnd - pos) : 0;
                }
                if (metaType == 0x51u && metaLen >= 3) {  // set_tempo
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
                    out.midiLyrics.push_back({absTick, lyric, trackIdx});
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
                if (ctrl == 0x0Bu) {  // Expression
                    out.dynamics.push_back({absTick, (int)val, (int)channel});
                } else if (ctrl == 0x07u) {  // 主音量：和 CC11 一样承载渐强渐弱
                    volumeCc7.push_back({absTick, (int)val, (int)channel});
                } else if (ctrl == 0x40u) {  // CC64 延音踏板，>=64 算踩下
                    pedalEvents.push_back({absTick, (int)channel, val >= 64, ctrlSeq++});
                } else if (ctrl == 0x78u || ctrl == 0x7Bu) {  // All Sound/Notes Off
                    allOffEvents.push_back({absTick, (int)channel, ctrlSeq++});
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
                out.pitchBends.push_back({absTick, bend, (int)channel});
            } else if (status == 0xF0u || status == 0xF7u) {
                // SysEx：按长度整体跳过，否则会把后续事件错位解析
                uint32_t sxLen = readVarLen(d, size, pos);
                pos += sxLen;
            } else if (type == 0xC0u) {
                // 音色号（GM program change）。旧实现直接跳过，于是「这条轨是
                // 笛子/二胡/钢琴」这种对读谱极有用的信息被丢掉了；现在按通道
                // 记下来，输出层据此给每条声部标出乐器名。
                if (channel < 16) out.channelProgram[(int)channel] = (int)d[pos++];
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
    normalizeFormat2(out, keepAllFormat2);
    resolveBendRanges(out);
    applyControllerSemantics(out, pedalEvents, allOffEvents, volumeCc7);
    applySmpteTimeBase(out);
    recomputeSeconds(out);
    return true;
}

bool loadMidiFile(const std::string& path, MidiData& out, bool keepAllFormat2) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return parseBuffer(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), out,
                       keepAllFormat2);
}

bool loadMidiFromMemory(const uint8_t* data, size_t size, MidiData& out,
                        bool keepAllFormat2) {
    return parseBuffer(data, size, out, keepAllFormat2);
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

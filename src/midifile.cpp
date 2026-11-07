// midifile.cpp - SMF 二进制解析实现
#include "midi2jianpu/midifile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

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

    // ---- 逐轨 ----
    while (pos + 8 <= size && std::memcmp(d + pos, "MTrk", 4) == 0) {
        pos += 4;
        uint32_t trkLen = rd32(d + pos); pos += 4;
        size_t trkEnd = pos + trkLen;

        long absTick = 0;
        uint8_t runningStatus = 0;
        // RPN 状态必须是"每轨"的局部变量：曾用 static，导致跨轨/跨文件串味
        int rpnMSB = -1, rpnLSB = -1;

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
                        active[key] = {absTick, (int)vel};
                    } else {
                        auto it = active.find(key);
                        if (it != active.end()) {
                            NoteEvent ne;
                            ne.startTicks = it->second.first;
                            ne.durationTicks = absTick - it->second.first;
                            ne.velocity = it->second.second;
                            ne.rawDurationTicks = ne.durationTicks;
                            ne.pitch = note;
                            ne.channel = channel;
                            ne.track = trackIdx;
                            out.notes.push_back(ne);
                            active.erase(it);
                        }
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
                }
                // RPN跟踪: 0x65=RPN MSB, 0x64=RPN LSB, 0x06=Data Entry MSB
                if (ctrl == 0x65u) rpnMSB = val;
                else if (ctrl == 0x64u) rpnLSB = val;
                else if (ctrl == 0x06u && rpnMSB == 0 && rpnLSB == 0) {
                    // RPN 0x00 0x00 = 弯音范围(半音数)
                    out.bendRange = (double)val;
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
            } else if (type == 0xD0u) {  // 通道触后（无数据字节外的 1 字节）
                pos += 1;
            } else {
                pos += 1;
            }
        }
        pos = trkEnd;
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
    normalizeMeterMap(out);
    normalizeKeyMap(out);
    normalizeFormat2(out, keepAllFormat2);
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

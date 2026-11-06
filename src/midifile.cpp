// midifile.cpp - SMF 二进制解析实现
#include "midi2jianpu/midifile.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
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

// 按 set_tempo 分段，把 tick 精确映射为秒。
//
// 旧实现是在解析过程中用一个可变的 tempo 变量即时换算（tick2sec 闭包），
// 有两个致命问题：
//   1) 变速曲目里，早期的音符会用到最后一次 set_tempo 的速度；
//   2) format 1 文件的 set_tempo 通常在独立的 conductor 轨（track 0），
//      如果它排在音符轨之后被解析，整份谱子的秒数全部错位。
// 现在解析阶段只记录 tick，等所有事件收齐后统一重算。
static void recomputeSeconds(MidiData& out) {
    const double div = out.division > 0 ? (double)out.division : 480.0;

    // 按 tick 排序；同一 tick 上的多次 set_tempo 以最后一个为准
    std::sort(out.tempoChanges.begin(), out.tempoChanges.end(),
              [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });

    struct Seg { long tick; double sec; double usPerQn; };
    std::vector<Seg> segs;
    segs.push_back({0, 0.0, 500000.0});   // MIDI 规范的默认速度：120 BPM

    {
        double us = 500000.0;
        double sec = 0.0;
        long curTick = 0;
        for (const auto& p : out.tempoChanges) {
            if (p.bpm <= 0) continue;
            sec += (p.tick - curTick) * us / 1e6 / div;
            curTick = p.tick;
            us = 60000000.0 / p.bpm;
            if (segs.back().tick == curTick) segs.back().usPerQn = us;
            else segs.push_back({curTick, sec, us});
        }
    }

    auto tick2sec = [&](long t) -> double {
        size_t lo = 0, hi = segs.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (segs[mid].tick <= t) lo = mid; else hi = mid;
        }
        const Seg& s = segs[lo];
        return s.sec + (double)(t - s.tick) * s.usPerQn / 1e6 / div;
    };

    for (auto& n : out.notes) {
        double s = tick2sec(n.startTicks);
        n.startSeconds = s;
        n.durationSeconds = tick2sec(n.startTicks + n.durationTicks) - s;
        if (n.durationSeconds < 0.02) n.durationSeconds = 0.02;
    }

    // out.tempo 只用来推导 BPM（歌词 LRC 的时间轴换算），取曲首速度而不是最后一个
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

// 核心解析：从内存buffer解析MIDI
static bool parseBuffer(const uint8_t* d, size_t size, MidiData& out) {
    if (size < 14) return false;
    size_t pos = 0;

    // ---- Header MThd ----
    if (std::memcmp(d, "MThd", 4) != 0) return false;
    pos = 4;
    uint32_t hdrLen = rd32(d + pos); pos += 4;
    pos += 2;                       // format
    pos += 2;                       // ntrks
    out.division = rd16(d + pos); pos += 2;
    if (out.division == 0) out.division = 480;   // 防除零：非法 division 回退到 480
    if (hdrLen < 6) return false;
    pos += (hdrLen - 6);

    int trackIdx = 0;
    std::unordered_map<uint64_t, long> active;

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
                        active[key] = absTick;
                    } else {
                        auto it = active.find(key);
                        if (it != active.end()) {
                            NoteEvent ne;
                            ne.startTicks = it->second;
                            ne.durationTicks = absTick - it->second;
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
                    out.keySig.sharpsFlats = (int8_t)d[pos];
                    out.keySig.isMinor = d[pos+1] != 0;
                    out.keySig.valid = true;
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
            } else if (type == 0xC0u || type == 0xD0u) {
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
    normalizeMeterMap(out);
    recomputeSeconds(out);
    return true;
}

bool loadMidiFile(const std::string& path, MidiData& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return parseBuffer(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), out);
}

bool loadMidiFromMemory(const uint8_t* data, size_t size, MidiData& out) {
    return parseBuffer(data, size, out);
}

} // namespace m2j

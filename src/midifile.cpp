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

bool loadMidiFile(const std::string& path, MidiData& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (buf.size() < 14) return false;

    const uint8_t* d = reinterpret_cast<const uint8_t*>(buf.data());
    size_t size = buf.size();
    size_t pos = 0;

    // ---- Header MThd ----
    if (std::memcmp(buf.data(), "MThd", 4) != 0) return false;
    pos = 4;
    uint32_t hdrLen = rd32(d + pos); pos += 4;
    pos += 2;                       // format
    pos += 2;                       // ntrks
    out.division = rd16(d + pos); pos += 2;
    pos += (hdrLen - 6);

    double tempo = out.tempo;
    int trackIdx = 0;
    std::unordered_map<uint64_t, long> active;

    auto tick2sec = [&](long tick) {
        return tick * tempo / 1000000.0 / out.division;
    };

    // ---- 逐轨 ----
    while (pos + 8 <= size && std::memcmp(buf.data() + pos, "MTrk", 4) == 0) {
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
                uint64_t key = ((uint64_t)channel << 24) |
                               ((uint64_t)note << 8) | (uint64_t)trackIdx;
                if (type == 0x90u && vel > 0) {
                    active[key] = absTick;
                } else {
                    auto it = active.find(key);
                    if (it != active.end()) {
                        double start = tick2sec(it->second);
                        double end = tick2sec(absTick);
                        double dur = end - start;
                        if (dur < 0.02) dur = 0.02;
                        NoteEvent ne;
                        ne.startSeconds = start;
                        ne.durationSeconds = dur;
                        ne.startTicks = it->second;
                        ne.durationTicks = absTick - it->second;
                        ne.pitch = note;
                        ne.channel = channel;
                        ne.track = trackIdx;
                        out.notes.push_back(ne);
                        active.erase(it);
                    }
                }
            } else if (status == 0xFFu) {
                uint8_t metaType = d[pos++];
                uint32_t metaLen = readVarLen(d, size, pos);
                if (metaType == 0x51u && metaLen >= 3) {  // set_tempo
                    tempo = (double)((d[pos] << 16) | (d[pos+1] << 8) | d[pos+2]);
                } else if (metaType == 0x58u && metaLen >= 4) {  // time signature
                    out.timeSig.numerator = d[pos];
                    out.timeSig.denomPow = d[pos+1];
                    out.timeSig.valid = true;
                } else if (metaType == 0x59u && metaLen >= 2) {  // key signature
                    out.keySig.sharpsFlats = (int8_t)d[pos];
                    out.keySig.isMinor = d[pos+1] != 0;
                    out.keySig.valid = true;
                } else if (metaType == 0x03u) {  // track name
                    std::string name((const char*)&d[pos], metaLen);
                    out.trackNames[trackIdx] = name;
                }
                pos += metaLen;
            } else if (type == 0xB0u) {
                uint8_t ctrl = d[pos++];
                uint8_t val = d[pos++];
                if (ctrl == 0x0Bu) {  // Expression
                    out.dynamics.push_back({absTick, (int)val});
                }
            } else if (type == 0xE0u) {
                pos += 2;
            } else if (type == 0xC0u || type == 0xDu) {
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
                  return a.startSeconds < b.startSeconds;
              });
    out.tempo = tempo;
    return true;
}

} // namespace m2j

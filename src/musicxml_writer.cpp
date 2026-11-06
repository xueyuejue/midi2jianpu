// musicxml_writer.cpp - MIDI 数据 -> MusicXML 3.1
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/notation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace m2j {

// 升号写法（默认）
static const char* SHARP_STEP[12] = {
    "C", "C", "D", "D", "E", "F",
    "F", "G", "G", "A", "A", "B"
};
static const int SHARP_ALTER[12] = {
    0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0
};

// 降号写法（调号含降号时使用）
static const char* FLAT_STEP[12] = {
    "C", "D", "D", "E", "E", "F",
    "G", "G", "A", "A", "B", "B"
};
static const int FLAT_ALTER[12] = {
    0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0
};

// 时值 -> MusicXML type + 是否附点
static std::pair<std::string, bool> durTypeDot(double beats) {
    static const struct { double base; const char* type; } stds[] = {
        {4.0, "whole"}, {2.0, "half"}, {1.0, "quarter"},
        {0.5, "eighth"}, {0.25, "16th"}, {0.125, "32nd"}
    };
    for (auto& s : stds) {
        if (std::abs(beats - s.base) / s.base < 0.15) return {s.type, false};
        if (std::abs(beats - s.base * 1.5) / (s.base * 1.5) < 0.15) return {s.type, true};
    }
    // 三连音等非常规时值：按最接近的基础时值输出，配合 time-modification
    double best = 1e9; std::string bestType = "eighth";
    for (auto& s : stds) {
        double d = std::abs(beats - s.base);
        if (d < best) { best = d; bestType = s.type; }
    }
    return {bestType, false};
}

std::string renderMusicXml(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<score-partwise version=\"3.1\">\n";

    // 弯音修正（只统计同通道）
    auto bendOffset = [&](long start, long end, int channel) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.channel == channel && pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * data.bendRange;
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    // 按轨分组
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks, n.channel);
        if (std::abs(off) >= 0.5) n.pitch += (int)std::round(off);
        byTrack[n.track].push_back(n);
    }

    // part-list
    xml << "  <part-list>\n";
    int partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        std::string name;
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) name = it->second;
        else name = "Part " + std::to_string(t + 1);
        xml << "    <score-part id=\"P" << partIdx << "\">"
            << "<part-name>" << name << "</part-name></score-part>\n";
        partIdx++;
    }
    xml << "  </part-list>\n";

    double barLen = data.timeSig.barLengthTicks(data.division);
    const bool flat = prefersFlats(key);
    const int fifths = keyFifths(key);

    partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        xml << "  <part id=\"P" << partIdx << "\">\n";
        int curBar = -1;
        long prevEnd = 0;
        long lastStart = -1;                  // 用于识别同拍和弦
        bool measureHasContent = false;
        auto triplets = detectTriplets(ns, data.division);

        auto closeMeasure = [&]() {
            if (curBar >= 0) {
                // 小节内音符总长不足时补休止符，保证 MuseScore 导入后小节完整
                double filled = (prevEnd - curBar * barLen) / (double)data.division;
                if (filled < 0) filled = 0;
                double need = barLen / (double)data.division - filled;
                if (need >= 0.25) {
                    auto [rt, rd] = durTypeDot(need);
                    xml << "      <note><rest/>"
                        << "<duration>" << (long)std::llround(need * data.division) << "</duration>"
                        << "<type>" << rt << "</type>";
                    if (rd) xml << "<dot/>";
                    xml << "</note>\n";
                }
                xml << "    </measure>\n";
            }
        };

        for (size_t idx = 0; idx < ns.size(); idx++) {
            const auto& n = ns[idx];
            int bar = (int)(n.startTicks / barLen);
            bool isChordMember = (n.startTicks == lastStart);

            if (bar != curBar) {
                if (curBar >= 0) {
                    closeMeasure();
                    prevEnd = (long)(bar * barLen);
                }
                curBar = bar;
                lastStart = -1;
                isChordMember = false;
                xml << "    <measure number=\"" << (bar + 1) << "\">\n";
                if (bar == 0) {
                    xml << "      <attributes>\n"
                        // divisions 用文件自身的时值分辨率，保证 duration 是整数
                        << "        <divisions>" << data.division << "</divisions>\n"
                        << "        <key><fifths>" << fifths << "</fifths>"
                        << "<mode>" << (key.isMajor ? "major" : "minor") << "</mode></key>\n"
                        << "        <time><beats>" << data.timeSig.numerator
                        << "</beats><beat-type>" << (1 << data.timeSig.denomPow)
                        << "</beat-type></time>\n"
                        << "      </attributes>\n";
                }
            }

            // 间隙 → 休止符（和弦成员不重复插入）
            if (!isChordMember) {
                long gap = n.startTicks - prevEnd;
                if (gap >= data.division / 2) {
                    double restBeats = gap / (double)data.division;
                    auto [rtype, rdot] = durTypeDot(restBeats);
                    xml << "      <note><rest/>"
                        << "<duration>" << gap << "</duration>"
                        << "<type>" << rtype << "</type>";
                    if (rdot) xml << "<dot/>";
                    xml << "</note>\n";
                }
            }

            long durTicks = std::max<long>(n.durationTicks, 1);
            double beats = durTicks / (double)data.division;
            int pc = n.pitch % 12;
            int octave = n.pitch / 12 - 1;
            const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
            int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];

            xml << "      <note>";
            if (isChordMember) xml << "<chord/>";
            xml << "<pitch><step>" << step << "</step>";
            if (alter != 0) xml << "<alter>" << alter << "</alter>";
            xml << "<octave>" << octave << "</octave></pitch>"
                << "<duration>" << durTicks << "</duration>";
            if (triplets[idx]) {
                xml << "<time-modification>"
                    << "<actual-notes>3</actual-notes>"
                    << "<normal-notes>2</normal-notes>"
                    << "</time-modification>";
            }
            auto [ntype, ndot] = durTypeDot(beats);
            xml << "<type>" << ntype << "</type>";
            if (ndot) xml << "<dot/>";
            xml << "</note>\n";

            lastStart = n.startTicks;
            prevEnd = std::max(prevEnd, n.startTicks + durTicks);
            measureHasContent = true;
        }
        (void)measureHasContent;
        closeMeasure();
        xml << "  </part>\n";
        partIdx++;
    }

    xml << "</score-partwise>\n";
    return xml.str();
}

} // namespace m2j

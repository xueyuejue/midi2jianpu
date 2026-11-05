// musicxml_writer.cpp - MIDI 数据 -> MusicXML 3.1
#include "midi2jianpu/musicxml_writer.hpp"
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace m2j {

static const char* STEP_NAMES[12] = {
    "C", "C", "D", "D", "E", "F",
    "F", "G", "G", "A", "A", "B"
};
static const int ALTER[12] = {
    0, 1, 0, 1, 0, 0,
    1, 0, 1, 0, 1, 0
};

// 时值 -> MusicXML type
static std::string durType(double beats) {
    if (beats >= 3.5) return "whole";
    if (beats >= 1.75) return "half";
    if (beats >= 0.875) return "quarter";
    if (beats >= 0.4375) return "eighth";
    if (beats >= 0.21875) return "16th";
    return "32nd";
}

std::string renderMusicXml(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<score-partwise version=\"3.1\">\n";

    // 弯音修正（与文本/HTML版一致）
    auto bendOffset = [&](long start, long end) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * 2.0;
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    // 按轨分组
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks);
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

    partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        xml << "  <part id=\"P" << partIdx << "\">\n";
        int curBar = -1;
        long barStart = 0;

        for (const auto& n : ns) {
            int bar = (int)(n.startTicks / barLen);
            if (bar != curBar) {
                if (curBar >= 0) xml << "    </measure>\n";
                curBar = bar;
                barStart = n.startTicks;
                xml << "    <measure number=\"" << (bar + 1) << "\">\n";
                if (bar == 0) {
                    // 第一个小节：attributes
                    int fifths = 0;
                    if (data.keySig.valid) fifths = data.keySig.sharpsFlats;
                    else {
                        // 从key估计推导
                        int pc = key.tonicPitchClass;
                        static const int majFifths[12] = {0,5,7,2,10,4,9,11,3,6,8,1};
                        fifths = majFifths[pc];
                        if (!key.isMajor) fifths = (fifths - 3 + 12) % 12 - 6;
                    }
                    xml << "      <attributes>\n"
                        << "        <divisions>1</divisions>\n"
                        << "        <key><fifths>" << fifths << "</fifths>"
                        << "<mode>" << (key.isMajor ? "major" : "minor") << "</mode></key>\n"
                        << "        <time><beats>" << data.timeSig.numerator
                        << "</beats><beat-type>" << (1 << data.timeSig.denomPow)
                        << "</beat-type></time>\n"
                        << "      </attributes>\n";
                }
            }

            double beats = n.durationTicks / (double)data.division;
            int pc = n.pitch % 12;
            int octave = n.pitch / 12 - 1;

            xml << "      <note><pitch>"
                << "<step>" << STEP_NAMES[pc] << "</step>";
            if (ALTER[pc] != 0) {
                xml << "<alter>" << ALTER[pc] << "</alter>";
            }
            xml << "<octave>" << octave << "</octave></pitch>"
                << "<duration>" << std::fixed << std::setprecision(2) << beats << "</duration>"
                << "<type>" << durType(beats) << "</type>"
                << "</note>\n";
        }
        if (curBar >= 0) xml << "    </measure>\n";
        xml << "  </part>\n";
        partIdx++;
    }

    xml << "</score-partwise>\n";
    return xml.str();
}

} // namespace m2j

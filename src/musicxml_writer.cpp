// musicxml_writer.cpp - MIDI 数据 -> MusicXML 3.1
//
// 输出目标是「MuseScore 打开就能用」，因此比纯文本输出严格得多：
//   - duration 必须是整数，且 divisions 取自源文件，不做二次换算
//   - 同时发声的音符要标 <chord/>，否则会被当成旋律音顺次排开
//   - 跨小节的音符必须切开，段间用 <tie> 连接，否则小节时长会溢出
//   - 三连音的 <type> 要写名义时值（乘 3/2），不能写实际时长
//   - 每个小节的总时长必须等于拍号规定值，不足要补休止符
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/notation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <vector>

namespace m2j {

// 升号写法（调号不含降号时使用）
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

// 拍数 -> MusicXML <type> + 是否附点
//
// 传入的必须是**名义时值**：三连音八分音符实际占 1/3 拍，但它的 <type>
// 应写 eighth（名义 0.5 拍）并配 <time-modification>。若直接把 0.333 传进来，
// 会落到 16th 上，MuseScore 里三连音就画错了。
static std::pair<std::string, bool> durTypeDot(double beats) {
    static const struct { double base; const char* type; } stds[] = {
        {4.0, "whole"}, {2.0, "half"}, {1.0, "quarter"},
        {0.5, "eighth"}, {0.25, "16th"}, {0.125, "32nd"}, {0.0625, "64th"}
    };
    // 先精确匹配（含附点），容差按比例收紧到 8%，
    // 原来 15% 会把 5 拍误判成附点全音符。
    for (auto& s : stds) {
        if (std::abs(beats - s.base) / s.base < 0.08) return {s.type, false};
        double dotted = s.base * 1.5;
        if (std::abs(beats - dotted) / dotted < 0.08) return {s.type, true};
    }
    // 兜底：取最接近的基础时值
    double best = 1e9; std::string bestType = "quarter";
    for (auto& s : stds) {
        double d = std::abs(beats - s.base);
        if (d < best) { best = d; bestType = s.type; }
    }
    return {bestType, false};
}

// 一个待输出的音符片段。
// 跨小节的音符会被切成多段，段与段之间用 tie 连接——这是 MuseScore 正确
// 渲染延音的前提，也是小节时长不溢出的前提。
struct Segment {
    long start = 0;        // 绝对 tick
    long dur = 0;          // 本段持续 tick
    int bar = 0;           // 所属小节（从 0 计）
    int pitch = 0;
    bool triplet = false;  // 属于三连音组，需要 <time-modification>
    bool tieStart = false; // 后面还有延续段 -> <tie type="start"/>
    bool tieEnd = false;   // 前面有被切开的段 -> <tie type="stop"/>
};

std::string renderMusicXml(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<score-partwise version=\"3.1\">\n";

    const int division = (data.division > 0) ? data.division : 480;

    // 弯音修正（只统计同通道，避免不同乐器互相干扰）
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

    // 小节长度（tick）。拍号缺失或非法时回退到 4/4，
    // 否则后面所有除法都会炸。弱起曲目的整套换算走 BarGrid。
    const BarGrid grid = barGridOf(data);

    const bool flat = prefersFlats(key);
    const int fifths = keyFifths(key);

    // 速度：MIDI 里 tempo 是「每四分音符的微秒数」，取首个非零值
    double bpm = 120.0;
    if (data.tempo > 0) bpm = 60000000.0 / data.tempo;
    else if (!data.tempoChanges.empty() && data.tempoChanges[0].bpm > 0) bpm = data.tempoChanges[0].bpm;

    partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        if (ns.empty()) { partIdx++; continue; }

        // ---- 1. 展开成片段：跨小节的音符在此切开 ----
        auto triplets = detectTriplets(ns, division);
        std::vector<Segment> segs;
        segs.reserve(ns.size() * 2);

        for (size_t i = 0; i < ns.size(); i++) {
            const auto& n = ns[i];
            long start = n.startTicks;
            long end = start + std::max<long>(n.durationTicks, 1);
            bool firstPart = true;

            while (start < end) {
                int bar = (int)grid.barIndex(start);
                long barEnd = grid.barEnd(bar);
                long segEnd = std::min(end, barEnd);
                // 防御：若换算出的小节末尾没有推进，直接吃到音符结束，避免死循环
                if (segEnd <= start) segEnd = end;

                Segment s;
                s.start = start;
                s.dur = segEnd - start;
                s.bar = bar;
                s.pitch = n.pitch;
                s.triplet = (i < triplets.size()) ? triplets[i] : false;
                s.tieEnd = !firstPart;      // 前面被切过
                s.tieStart = (segEnd < end); // 后面还有
                segs.push_back(s);

                start = segEnd;
                firstPart = false;
            }
        }

        // ---- 2. 按 (小节, 起始 tick) 稳定排序，保持和弦内各音次序 ----
        std::stable_sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) {
            if (a.bar != b.bar) return a.bar < b.bar;
            return a.start < b.start;
        });

        // ---- 3. 谱号：音域偏低时用低音谱号，否则高音谱号 ----
        // 不判断的话钢琴左手会用高音谱号写，满屏加线。
        double pitchSum = 0;
        for (const auto& n : ns) pitchSum += n.pitch;
        double avgPitch = pitchSum / (double)ns.size();
        const bool bassClef = avgPitch < 58.0;  // 约 A#3 以下

        // ---- 4. 输出 ----
        xml << "  <part id=\"P" << partIdx << "\">\n";

        int curBar = -1;
        long writtenUpTo = 0;   // 当前小节内已写到的绝对 tick
        long lastSegStart = -1; // 识别同拍和弦

        auto emitRest = [&](long from, long to) {
            if (to <= from) return;
            long len = to - from;
            auto [rt, rd] = durTypeDot(len / (double)division);
            xml << "      <note><rest/><duration>" << len
                << "</duration><voice>1</voice><type>" << rt << "</type>";
            if (rd) xml << "<dot/>";
            xml << "</note>\n";
        };

        // <attributes> 只能在第一个真正写出来的小节里出现一次。
        // 原来写死成 `bar == 0`，一旦乐曲开头有整拍休止（前几个小节由下面的
        // 空小节补齐逻辑生成），attributes 就整个丢失，divisions / 拍号 / 调号 /
        // 谱号全没了，导出的文件 MuseScore 打不开。
        bool wroteAttrs = false;
        auto emitAttributes = [&]() {
            if (wroteAttrs) return;
            wroteAttrs = true;
            xml << "      <attributes>\n"
                // divisions 用文件自身的时值分辨率，保证 duration 全为整数
                << "        <divisions>" << division << "</divisions>\n"
                << "        <key><fifths>" << fifths << "</fifths>"
                << "<mode>" << (key.isMajor ? "major" : "minor") << "</mode></key>\n"
                << "        <time><beats>" << data.timeSig.numerator
                << "</beats><beat-type>" << (1 << data.timeSig.denomPow)
                << "</beat-type></time>\n"
                << "        <clef><sign>" << (bassClef ? "F" : "G") << "</sign><line>"
                << (bassClef ? 4 : 2) << "</line></clef>\n"
                << "      </attributes>\n";
            // 速度标记只写一遍，放在第一个声部
            if (partIdx == 1) {
                xml << "      <direction placement=\"above\">\n"
                    << "        <direction-type><metronome>"
                    << "<beat-unit>quarter</beat-unit>"
                    << "<per-minute>" << (int)std::lround(bpm) << "</per-minute>"
                    << "</metronome></direction-type>"
                    << "<sound tempo=\"" << (int)std::lround(bpm) << "\"/>\n"
                    << "      </direction>\n";
            }
        };

        // 弱起小节要标 implicit="yes"，否则 MuseScore 会把它当成一个完整小节，
        // 并要求补齐 4 拍；小节号也从 0 起算，第一个完整小节显示成 1。
        auto openMeasure = [&](int bar) {
            xml << "    <measure number=\"" << grid.measureNumber(bar) << "\"";
            if (grid.pickup > 0 && bar == 0) xml << " implicit=\"yes\"";
            xml << ">\n";
            emitAttributes();
            writtenUpTo = grid.barStart(bar);
        };

        auto closeMeasure = [&]() {
            long barEndTick = grid.barEnd(curBar);
            if (writtenUpTo < barEndTick) emitRest(writtenUpTo, barEndTick);
            xml << "    </measure>\n";
        };

        for (const auto& seg : segs) {
            if (seg.bar != curBar) {
                if (curBar >= 0) closeMeasure();

                // 补齐中间完全空的小节，否则小节编号会跳号
                for (int b = curBar + 1; b < seg.bar; b++) {
                    xml << "    <measure number=\"" << grid.measureNumber(b) << "\"";
                    // 弱起小节在别的声部里是空小节，同样要标 implicit，
                    // 否则各声部的小节号与 MuseScore 的编号对不上
                    if (grid.pickup > 0 && b == 0) xml << " implicit=\"yes\"";
                    xml << ">\n";
                    emitAttributes();   // 开头就是空小节时，属性要写在这里
                    xml << "      <note><rest measure=\"yes\"/><duration>"
                        << grid.barSpan(b) << "</duration><voice>1</voice>"
                        << "<type>whole</type></note>\n"
                        << "    </measure>\n";
                }

                curBar = seg.bar;
                openMeasure(curBar);
                lastSegStart = -1;
            }

            // 间隙补休止符（和弦成员不重复补）
            bool isChordMember = (seg.start == lastSegStart);
            if (!isChordMember && seg.start > writtenUpTo) emitRest(writtenUpTo, seg.start);

            int pc = seg.pitch % 12;
            if (pc < 0) pc += 12;
            int octave = seg.pitch / 12 - 1;
            const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
            int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];

            double beats = seg.dur / (double)division;
            // 三连音：<type> 写名义时值，实际缩放交给 <time-modification>
            auto [ntype, ndot] = durTypeDot(seg.triplet ? beats * 1.5 : beats);

            xml << "      <note>";
            if (isChordMember) xml << "<chord/>";
            xml << "<pitch><step>" << step << "</step>";
            if (alter != 0) xml << "<alter>" << alter << "</alter>";
            xml << "<octave>" << octave << "</octave></pitch>"
                << "<duration>" << seg.dur << "</duration>";
            if (seg.tieEnd)   xml << "<tie type=\"stop\"/>";
            if (seg.tieStart) xml << "<tie type=\"start\"/>";
            xml << "<voice>1</voice><type>" << ntype << "</type>";
            if (ndot) xml << "<dot/>";
            if (seg.triplet) {
                xml << "<time-modification>"
                    << "<actual-notes>3</actual-notes>"
                    << "<normal-notes>2</normal-notes>"
                    << "</time-modification>";
            }
            if (seg.tieEnd || seg.tieStart) {
                xml << "<notations>";
                if (seg.tieEnd)   xml << "<tied type=\"stop\"/>";
                if (seg.tieStart) xml << "<tied type=\"start\"/>";
                xml << "</notations>";
            }
            xml << "</note>\n";

            lastSegStart = seg.start;
            writtenUpTo = std::max(writtenUpTo, seg.start + seg.dur);
        }

        if (curBar >= 0) closeMeasure();
        xml << "  </part>\n";
        partIdx++;
    }

    xml << "</score-partwise>\n";
    return xml.str();
}

} // namespace m2j

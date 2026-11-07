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

// 歌词是用户贴进来的任意文本，'&' 或 '<' 会直接把 XML 打坏
static std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&apos;";
        else out += c;
    }
    return out;
}

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

// 和弦符号 <harmony> 的 root-step / bass-step：把音级写成「音名 + 升降」。
// MusicXML 用 <root-step>C</root-step><root-alter>1</root-alter> 这种两段式，
// 而不是一个 "C#" 字符串；升降为 0 时 <*-alter> 可以省掉。
static void emitHarmonyStep(std::ostringstream& xml, const char* tag, int pc, bool flat) {
    const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
    const int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];
    xml << "<" << tag << "-step>" << step << "</" << tag << "-step>";
    if (alter != 0) xml << "<" << tag << "-alter>" << alter << "</" << tag << "-alter>";
}

// 和弦符号。此前和弦只活在文本谱和 HTML 里，导进 MuseScore 就全没了；
// 现在写 <harmony>，MuseScore / Finale 会在谱表上方显示和弦名。
// kind 用 MusicXML 标准值（语义），text 属性放我们的显示后缀——m7b5、7sus4
// 没有对应的标准 kind，靠 text 才能原样显示。转位写 <bass>。
static void emitHarmonyXml(std::ostringstream& xml, const ChordMark& cm, bool flat) {
    if (cm.rootPc < 0) return;
    xml << "      <harmony><root>";
    emitHarmonyStep(xml, "root", cm.rootPc, flat);
    xml << "</root><kind";
    if (!cm.kindText.empty()) xml << " text=\"" << xmlEscape(cm.kindText) << "\"";
    xml << ">" << (cm.kind.empty() ? "major" : cm.kind) << "</kind>";
    if (cm.bassPc >= 0) {
        xml << "<bass>";
        emitHarmonyStep(xml, "bass", cm.bassPc, flat);
        xml << "</bass>";
    }
    xml << "</harmony>\n";
}

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
    int channel = 0;       // 演奏法按 (tick, channel) 查，不能只按 tick
    bool triplet = false;  // 属于三连音组，需要 <time-modification>
    bool tieStart = false; // 后面还有延续段 -> <tie type="start"/>
    bool tieEnd = false;   // 前面有被切开的段 -> <tie type="stop"/>
    bool grace = false;    // 倚音：<grace/> 不占时长，排在主音之前
};

std::string renderMusicXml(const MidiData& data, const KeyEstimate& key,
                           const std::vector<LyricLine>* lyricLines) {
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
        // 声部乐器：取该声部第一个音符的通道，查 0xC0 音色号得到 GM 乐器名。
        int pch = ns.empty() ? -1 : ns.front().channel;
        std::string inst;
        auto pit = data.channelProgram.find(pch);
        if (pit != data.channelProgram.end()) inst = gmInstrumentName(pit->second);
        xml << "    <score-part id=\"P" << partIdx << "\">"
            << "<part-name>" << xmlEscape(name) << "</part-name>";
        if (!inst.empty()) {
            xml << "<score-instrument id=\"P" << partIdx << "-I" << partIdx << "\">"
                << "<instrument-name>" << xmlEscape(inst) << "</instrument-name>"
                << "</score-instrument>";
        }
        xml << "</score-part>\n";
        partIdx++;
    }
    xml << "  </part-list>\n";

    // 小节长度（tick）。拍号缺失或非法时回退到 4/4，
    // 否则后面所有除法都会炸。弱起曲目的整套换算走 BarGrid。
    const BarGrid grid = barGridOf(data);
    // 调号按小节查表：中途转调时 MuseScore 必须看到新的 <fifths>/<mode>，
    // 音符的升/降等音写法也跟着当前调走。
    const KeyMap keys(data, key);
    // 演奏表情与文本谱、HTML 共用同一份判定。MusicXML 此前完全没写力度，
    // 导进 MuseScore 后「网页上有 mf、谱上什么都没有」。
    const ExpressionMap expr(data);

    // 速度：MIDI 里 tempo 是「每四分音符的微秒数」，取首个非零值
    double bpm = 120.0;
    if (data.tempo > 0) bpm = 60000000.0 / data.tempo;
    else if (!data.tempoChanges.empty() && data.tempoChanges[0].bpm > 0) bpm = data.tempoChanges[0].bpm;

    // 歌词只挂旋律轨。来源可能是 --lyrics 给的 LRC，也可能是 MIDI 自带的
    // 0x05 歌词；两者都走同一套 pickMelodyTrack（内置歌词还会给出人声轨证据）。
    const bool hasLyrics = lyricSourceOf(data, lyricLines) != LyricSource::None;
    const int mainTrack = hasLyrics ? pickMelodyTrack(data, lyricLines) : -1;

    // 和弦符号：和文本谱 / HTML 共用同一份 detectChords 结果。
    // 挂到旋律声部上方（标准 lead sheet 排法）；没有明确旋律声部时退化到
    // 第一个声部。按小节分桶，便于随音符位置插入 <harmony>。
    const std::vector<ChordMark> chordMarks = detectChords(data, key);
    const int chordTrack = pickMelodyTrack(data, lyricLines);
    std::map<int, std::vector<const ChordMark*>> chordsByBar;
    for (const auto& cm : chordMarks)
        chordsByBar[(int)grid.barIndex(cm.startTick)].push_back(&cm);

    partIdx = 1;
    for (auto& [t, ns] : byTrack) {
        if (ns.empty()) { partIdx++; continue; }

        const LyricAssignment lyrics = (t == mainTrack)
                                     ? resolveLyrics(data, lyricLines, t)
                                     : LyricAssignment();

        // CC11 是按通道的，一个 part 可能并非全是同一通道；取该声部第一个
        // 音符的通道，与 HTML 的 openBar(firstTick, channel) 保持同一取值口径。
        const int partChannel = ns.front().channel;
        std::string lastDyn;

        // 和弦符号只写一份：旋律声部优先，没有旋律声部时写进第一个 part。
        const bool writeChords = (chordTrack >= 0) ? (t == chordTrack) : (partIdx == 1);
        std::vector<const ChordMark*> pendingChords;  // 当前小节待写的和弦（按 tick 升序）
        size_t pendingIdx = 0;
        // 把 tick <= uptoTick 的和弦写出来。MusicXML 的 <harmony> 与音符按文档
        // 顺序定位，所以必须插在「该 tick 之前已写到的位置」——写在小节开头会
        // 让后半小节换的和弦全挤到第一拍。
        auto flushChords = [&](long uptoTick) {
            if (!writeChords) return;
            while (pendingIdx < pendingChords.size() &&
                   pendingChords[pendingIdx]->startTick <= uptoTick) {
                const ChordMark& cm = *pendingChords[pendingIdx++];
                emitHarmonyXml(xml, cm, prefersFlats(keys.at(cm.startTick)));
            }
        };

        // ---- 1. 展开成片段：跨小节的音符在此切开 ----
        auto triplets = detectTriplets(ns, division);
        std::vector<Segment> segs;
        segs.reserve(ns.size() * 2);

        for (size_t i = 0; i < ns.size(); i++) {
            const auto& n = ns[i];
            // 倚音（装饰音）：整颗跳过，改挂到紧随其后的主音上作为 <grace/>。
            // 否则它会自己占一格、还把主音往后推，整行节奏错位。
            if (expr.isGraceNote(n.startTicks, n.channel)) continue;

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
                s.channel = n.channel;
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
        // 中途变拍号时，在变化的那一小节再写一次只含 <time> 的 attributes：
        // 少写这一次，MuseScore 会把后半首也按曲首拍号解释，小节线全错。
        bool wroteAttrs = false;
        auto emitKeyXml = [&](const KeyEstimate& k) {
            xml << "        <key><fifths>" << keyFifths(k) << "</fifths>"
                << "<mode>" << (k.isMajor ? "major" : "minor") << "</mode></key>\n";
        };
        auto emitTimeXml = [&](const TimeSignature& sig) {
            xml << "        <time><beats>" << sig.numerator
                << "</beats><beat-type>" << (1 << sig.denomPow)
                << "</beat-type></time>\n";
        };
        auto emitAttributes = [&](int bar) {
            const bool meterChanged = grid.meterChangedAt(bar);
            const bool keyChanged = keys.keyChangedAtBar(bar, grid);
            const KeyEstimate k = keys.keyAtBar(bar, grid);
            const TimeSignature sig = grid.sigForBar(bar);
            if (!wroteAttrs) {
                wroteAttrs = true;
                xml << "      <attributes>\n"
                    // divisions 用文件自身的时值分辨率，保证 duration 全为整数
                    << "        <divisions>" << division << "</divisions>\n";
                emitKeyXml(k);
                emitTimeXml(sig);
                xml << "        <clef><sign>" << (bassClef ? "F" : "G") << "</sign><line>"
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
                    // 民族调式在 MusicXML 里没有标准 <mode> 值，音高/调号仍按
                    // 宫音记为大调，把「G 徵调式」作为文字标记写在谱头，导入
                    // MuseScore 后能看见。label 由程序构造，不含 XML 特殊字符。
                    if (!key.scaleLabel.empty()) {
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><words>"
                            << key.scaleLabel << "</words></direction-type>\n"
                            << "      </direction>\n";
                    }
                }
            } else if (meterChanged || keyChanged) {
                // attributes 的子元素有固定顺序：<key> 必须排在 <time> 前面，
                // 否则某些记谱软件只认到第一个。两者同时变化时合进一个块。
                xml << "      <attributes>\n";
                if (keyChanged) emitKeyXml(k);
                if (meterChanged) emitTimeXml(sig);
                xml << "      </attributes>\n";
            }
        };

        // 演奏表情写进 <direction>：必须排在 <attributes> 之后、第一个
        // <note> 之前，MuseScore 才会把它显示在小节上方而不是吞掉。
        // 一个 part 一份 lastDyn，力度档位没变就不重复写。
        auto emitDirections = [&](int bar) {
            if (!expr.enabled()) return;
            const long bs = grid.barStart(bar), be = grid.barEnd(bar);
            const std::string dyn = expr.dynamicAt(bs, partChannel);
            if (!dyn.empty() && dyn != lastDyn) {
                xml << "      <direction placement=\"above\">\n"
                    << "        <direction-type><dynamics><" << dyn
                    << "/></dynamics></direction-type>\n"
                    << "        <sound dynamics=\"" << dynamicToMidiVolume(dyn)
                    << "\"/>\n"
                    << "      </direction>\n";
                lastDyn = dyn;
            }
            // 段落名 / 渐强渐弱 / 速度变化属于整份谱，只在第一个 part 写一次；
            // 力度按通道各自写（伴奏轨有自己的 CC11 曲线）。
            if (partIdx == 1) {
                // 速度变化点：在那一小节标 ♩=N（初始速度已在 emitAttributes 写）。
                for (size_t ti = 1; ti < data.tempoChanges.size(); ti++) {
                    if (data.tempoChanges[ti].tick == bs) {
                        int tb = (int)std::lround(data.tempoChanges[ti].bpm);
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><metronome>"
                            << "<beat-unit>quarter</beat-unit>"
                            << "<per-minute>" << tb << "</per-minute>"
                            << "</metronome></direction-type>"
                            << "<sound tempo=\"" << tb << "\"/>\n"
                            << "      </direction>\n";
                        break;
                    }
                }
                for (const auto& m : expr.inRange(bs, be)) {
                    if (!m.section.empty())
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><words>"
                            << xmlEscape(m.section) << "</words></direction-type>\n"
                            << "      </direction>\n";
                    if (m.wedge != 0)
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><wedge type=\""
                            << (m.wedge > 0 ? "crescendo" : "diminuendo")
                            << "\" number=\"1\"/></direction-type>\n"
                            << "      </direction>\n";
                    if (m.wedgeStop)
                        xml << "      <direction placement=\"above\">\n"
                            << "        <direction-type><wedge type=\"stop\" "
                            << "number=\"1\"/></direction-type>\n"
                            << "      </direction>\n";
                }
            }
        };

        // 弱起小节要标 implicit="yes"，否则 MuseScore 会把它当成一个完整小节，
        // 并要求补齐 4 拍；小节号也从 0 起算，第一个完整小节显示成 1。
        auto openMeasure = [&](int bar) {
            xml << "    <measure number=\"" << grid.measureNumber(bar) << "\"";
            if (grid.pickup > 0 && bar == 0) xml << " implicit=\"yes\"";
            xml << ">\n";
            emitAttributes(bar);
            emitDirections(bar);
            pendingChords.clear();
            pendingIdx = 0;
            if (writeChords) {
                auto it = chordsByBar.find(bar);
                if (it != chordsByBar.end()) pendingChords = it->second;
            }
            writtenUpTo = grid.barStart(bar);
        };

        auto closeMeasure = [&]() {
            long barEndTick = grid.barEnd(curBar);
            // 小节末尾还没被音符消费的和弦（后半小节是休止时常见）：先补到
            // 它所在的拍位再写符号，否则和弦会被顶到小节第一拍。
            if (writeChords) {
                while (pendingIdx < pendingChords.size()) {
                    const ChordMark& cm = *pendingChords[pendingIdx++];
                    if (writtenUpTo < cm.startTick) {
                        emitRest(writtenUpTo, cm.startTick);
                        writtenUpTo = cm.startTick;
                    }
                    emitHarmonyXml(xml, cm, prefersFlats(keys.at(cm.startTick)));
                }
            }
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
                    emitAttributes(b);   // 开头就是空小节时，属性要写在这里
                    emitDirections(b);  // 段落标记可能落在纯休止的前奏
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

            // 和弦符号按音头位置插入：此刻文档已推进到 seg.start，写在这里
            // 就正好落在该拍上方。
            flushChords(seg.start);

            int pc = seg.pitch % 12;
            if (pc < 0) pc += 12;
            int octave = seg.pitch / 12 - 1;
            const bool flat = prefersFlats(keys.at(seg.start));
            const char* step = flat ? FLAT_STEP[pc] : SHARP_STEP[pc];
            int alter = flat ? FLAT_ALTER[pc] : SHARP_ALTER[pc];

            double beats = seg.dur / (double)division;
            // 三连音：<type> 写名义时值，实际缩放交给 <time-modification>
            auto [ntype, ndot] = durTypeDot(seg.triplet ? beats * 1.5 : beats);

            // ---- 歌词与一字多音 ----
            // 只挂在旋律轨的「真实音头」上：跨小节切出来的延续段（tieEnd）
            // 与和弦的第二、三个音（chordMember）都不是新音节，重复写会让
            // MuseScore 把同一个字显示两遍。
            // 一个字唱多个音时：首音 syllabic=begin + slur start，中间音
            // middle，末音 end + slur stop；一音一字则写 single。
            std::string lyricXml, slurXml;
            if (!lyrics.empty() && !seg.tieEnd && !isChordMember) {
                auto pit = lyrics.at.find(seg.start);
                if (pit != lyrics.at.end()) {
                    const LyricPlacement& pl = pit->second;
                    const bool multi = (pl.noteCount >= 2);
                    lyricXml = std::string("<lyric number=\"1\"><syllabic>")
                             + (multi ? "begin" : "single")
                             + "</syllabic><text>" + xmlEscape(pl.text) + "</text></lyric>";
                    if (multi) slurXml = "<slur type=\"start\" number=\"1\"/>";
                } else {
                    int g = lyrics.groupAt(seg.start);
                    if (g > 0) {
                        const LyricPlacement& pl = lyrics.melismas[g - 1];
                        const bool last = (seg.start == pl.lastNoteTick);
                        // <extend/> 是 MusicXML 里表示「这个字还在拖」的标准
                        // 元素；只写 syllabic 不写 text 有些记谱软件会当成空词。
                        lyricXml = std::string("<lyric number=\"1\"><syllabic>")
                                 + (last ? "end" : "middle")
                                 + "</syllabic><extend/></lyric>";
                        if (last) slurXml = "<slur type=\"stop\" number=\"1\"/>";
                    }
                }
            }

            // 倚音（装饰音）：在主音之前写一个 <grace/> 音符，不占时长，
            // MuseScore 会把它渲染成主音左侧的小装饰音。只在主音的第一个分段
            // （真实音头，非跨小节延续段、非和弦成员）上写一次。
            if (!isChordMember && !seg.tieEnd) {
                const NoteExpression ae0 = expr.noteAt(seg.start, seg.channel);
                if (ae0.hasGrace()) {
                    int gp = ae0.gracePitch;
                    double goff = bendOffset(seg.start - division / 16,
                                            seg.start, seg.channel);
                    if (std::abs(goff) >= 0.5) gp += (int)std::round(goff);
                    int gpc = gp % 12; if (gpc < 0) gpc += 12;
                    int goct = gp / 12 - 1;
                    const bool gflat = prefersFlats(keys.at(seg.start));
                    const char* gstep = gflat ? FLAT_STEP[gpc] : SHARP_STEP[gpc];
                    int galter = gflat ? FLAT_ALTER[gpc] : SHARP_ALTER[gpc];
                    xml << "      <note><grace/>"
                        << "<pitch><step>" << gstep << "</step>";
                    if (galter != 0) xml << "<alter>" << galter << "</alter>";
                    xml << "<octave>" << goct << "</octave></pitch>"
                        << "<voice>1</voice><type>eighth</type></note>\n";
                }
            }

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
            // MusicXML 的 note 子元素有固定顺序：notations 在 lyric 之前。
            // tied 与 slur 必须合进同一个 <notations>，分两个写会让某些
            // 记谱软件只认第一个。
            std::string notations;
            if (seg.tieEnd)   notations += "<tied type=\"stop\"/>";
            if (seg.tieStart) notations += "<tied type=\"start\"/>";
            notations += slurXml;
            // 断音 / 重音 / 滑音 / 颤音挂在真实音头上：和弦成员共用第一音的
            // 记号，跨小节切出来的延续段 seg.start 不是音头，自然查不到。
            if (!isChordMember) {
                const NoteExpression ae = expr.noteAt(seg.start, seg.channel);
                std::string arts;
                if (ae.staccatissimo) arts += "<staccatissimo/>";
                else if (ae.staccato) arts += "<staccato/>";
                if (ae.accent) arts += "<accent/>";
                if (!arts.empty())
                    notations += "<articulations>" + arts + "</articulations>";
                if (ae.bend == BendMark::Trill) {
                    notations += "<ornaments><trill-mark/></ornaments>";
                } else if (ae.bend == BendMark::SlideUp ||
                           ae.bend == BendMark::SlideDown) {
                    // 同一个音符内 start+stop 表示「音内滑音」，不需要跨小节配对。
                    notations += "<slide line-type=\"solid\" number=\"2\" "
                                 "type=\"start\"/><slide line-type=\"solid\" "
                                 "number=\"2\" type=\"stop\"/>";
                }
            }
            if (!notations.empty()) xml << "<notations>" << notations << "</notations>";
            xml << lyricXml;
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

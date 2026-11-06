// html_writer.cpp - 简谱 -> 自包含 HTML 渲染
//
// 排版坐标约定（一个音符是一列）：
//
//     ·  ·      <- 上八度点（.oct 固定 12px 高，无点时留空占位）
//     5  -  -   <- 数字与延音线同行（.core 用 flex row）
//     ·         <- 下八度点（.oct.low，紧贴数字下方，不会被歌词压住）
//     风        <- 歌词
//
// 早先 .note 用 flex-direction:column 却把 `-` 直接作为子元素，导致延音线
// 掉到数字下一行、整份谱子看起来像一堆方块；低八度点用 absolute 定位在
// .note 底部，有歌词时会压在字上。这两处都是纯排版问题，改用分层结构解决。
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/version.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace m2j {

static std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

// 时值 -> CSS class + 延音线条数 + 减时线条数
struct DurStyle {
    const char* cls;
    bool dotted;
    int extLines;   // 延音线数量：二分 1 条、附点二分 2 条、全音符 3 条
    int beams;      // 减时线数量：八分 1 条、十六分 2 条、三十二分 3 条
};

static DurStyle durStyleOf(double beats) {
    static const struct { double beats; DurStyle st; } table[] = {
        {0.125, {"thirtysecond", false, 0, 3}},
        {0.25,  {"sixteenth",    false, 0, 2}},
        {0.375, {"sixteenth",    true,  0, 2}},
        {0.5,   {"eighth",       false, 0, 1}},
        {0.75,  {"eighth",       true,  0, 1}},
        {1.0,   {"quarter",      false, 0, 0}},
        {1.5,   {"quarter",      true,  0, 0}},
        {2.0,   {"half",         false, 1, 0}},
        {3.0,   {"half",         true,  2, 0}},
        {4.0,   {"whole",        false, 3, 0}},
        {6.0,   {"whole",        true,  5, 0}},
    };
    double best = 1e9;
    DurStyle out{"quarter", false, 0, 0};
    for (auto& t : table) {
        double d = std::abs(beats - t.beats);
        if (d < best) { best = d; out = t.st; }
    }
    return out;
}

// 三连音的时值不能按普通时值表取近似值。
// 三连音八分是 1/3 拍，而时值表里最近的档位是 0.375（附点十六分），
// 于是一个「三连八分」被画成附点十六分——两条减时线外加一个附点，
// 懂简谱的人一眼就看出错。三连音的真实档位只有 1/3、1/6、1/12 拍。
static DurStyle tripletStyleOf(double beats) {
    double r = beats * 3.0;      // 1/3 拍 -> 1.0，1/6 拍 -> 0.5，1/12 拍 -> 0.25
    if (r < 0.34) return {"thirtysecond", false, 0, 3};
    if (r < 0.75) return {"sixteenth",    false, 0, 2};
    return {"eighth", false, 0, 1};
}

// 生成一个音符的 HTML 列。
//
// 关键点：延音线不参与宽度计算（绝对定位挂在 .core 右侧），所以 .note 的
// 实际宽度由数字决定，八度点和歌词都对准数字中心——这是简谱的硬性要求，
// 否则 `1 - - -` 的低八度点会飘到整个音符的正中，偏离数字好远。
static std::string noteColumn(const std::string& numText, int highDots, int lowDots,
                              double beats, const std::string& lyric,
                              const std::string& extraClass = "",
                              const DurStyle* overrideStyle = nullptr) {
    DurStyle ds = overrideStyle ? *overrideStyle : durStyleOf(beats);

    std::ostringstream os;
    os << "<span class='note " << ds.cls << extraClass << "'";
    // --n 是延音线条数，用来给 .note 预留右侧空间
    if (ds.extLines > 0) os << " style='--n:" << ds.extLines << "'";
    os << ">";

    // 上八度点：固定高度占位，保证各行音符顶部对齐
    os << "<span class='oct'>";
    for (int i = 0; i < highDots; i++) os << "<i></i>";
    os << "</span>";

    // 数字（延音线绝对定位在其右侧，不撑宽）
    os << "<span class='core'><span class='num'>" << htmlEscape(numText) << "</span>";
    if (ds.extLines > 0) {
        os << "<span class='exts'>";
        for (int i = 0; i < ds.extLines; i++) os << "<span class='ext'>-</span>";
        os << "</span>";
    }
    os << "</span>";

    // 下八度点：紧跟数字下方、歌词之前
    os << "<span class='oct low'>";
    for (int i = 0; i < lowDots; i++) os << "<i></i>";
    os << "</span>";

    // 歌词：始终占位，保证同一行音符底部对齐
    os << "<span class='lyric'>" << htmlEscape(lyric) << "</span>";
    os << "</span>";
    return os.str();
}

// 把和弦标记挂到一个音符列的顶部。
//
// 不能另起一列：和弦标记若占宽度，同一拍的音符间距会被它撑开，
// 谱面看起来就是参差不齐的。这里塞进 .note 内部绝对定位，浮在数字上方，
// 与标记行里的和弦（小节起点那种）落在同一高度，视觉上是一条线。
static std::string withChord(const std::string& col, const std::string& chord) {
    if (chord.empty()) return col;
    size_t p = col.find('>');
    if (p == std::string::npos) return col;
    return col.substr(0, p + 1) +
           "<span class='chordx'>" + htmlEscape(chord) + "</span>" +
           col.substr(p + 1);
}

// 生成和弦列：同一时刻发声的多个音竖向叠写，共用一个时值线。
//
// 简谱里和弦就是上下对齐的几行数字。若不叠排而顺序输出，一个三音和弦会被
// 画成三个连续音符（4/4 小节看着像有 12 拍），完全误导。
static std::string chordColumn(const std::vector<std::pair<std::string, std::pair<int, int>>>& tones,
                               double beats, const std::string& lyric) {
    DurStyle ds = durStyleOf(beats);

    std::ostringstream os;
    os << "<span class='note chordcol " << ds.cls << "'";
    if (ds.extLines > 0) os << " style='--n:" << ds.extLines << "'";
    os << "><span class='core'><span class='stack'>";

    for (const auto& tn : tones) {
        os << "<span class='tone'>";
        os << "<span class='oct'>";
        for (int i = 0; i < tn.second.first; i++) os << "<i></i>";
        os << "</span>";
        os << "<span class='num'>" << htmlEscape(tn.first) << "</span>";
        os << "<span class='oct low'>";
        for (int i = 0; i < tn.second.second; i++) os << "<i></i>";
        os << "</span>";
        os << "</span>";
    }

    os << "</span>";
    if (ds.extLines > 0) {
        os << "<span class='exts'>";
        for (int i = 0; i < ds.extLines; i++) os << "<span class='ext'>-</span>";
        os << "</span>";
    }
    os << "</span>";
    os << "<span class='lyric'>" << htmlEscape(lyric) << "</span>";
    os << "</span>";
    return os.str();
}

std::string renderHtml(const MidiData& data, const KeyEstimate& key,
                       const std::string& title,
                       const std::vector<LyricLine>* lyricLines) {
    std::string displayTitle = title.empty() ? "简谱" : title;

    std::ostringstream body;
    double barLen = data.timeSig.barLengthTicks(data.division);
    if (!(barLen > 0)) barLen = data.division * 4.0;

    // 和弦检测。按「和声窗」而不是整小节（4/4 半小节、6/8 附点四分），
    // 所以索引键是准确的 tick，同一小节里可以有多个和弦。
    auto chords = detectChords(data, key);
    std::map<long, std::string> chordAt;
    for (const auto& c : chords) chordAt[c.startTick] = c.name;

    // 歌词按行拆成单字
    std::vector<std::pair<long, std::vector<std::string>>> lineSyls;
    if (lyricLines) {
        for (const auto& ll : *lyricLines) {
            std::vector<std::string> syls;
            for (size_t i = 0; i < ll.text.size(); ) {
                unsigned char c = ll.text[i];
                int charLen = 1;
                if ((c & 0xE0) == 0xC0) charLen = 2;
                else if ((c & 0xF0) == 0xE0) charLen = 3;
                else if ((c & 0xF8) == 0xF0) charLen = 4;
                if (i + charLen > ll.text.size()) break;
                syls.push_back(ll.text.substr(i, charLen));
                i += charLen;
            }
            lineSyls.push_back({ll.startTick, syls});
        }
    }

    // 歌词分配表：tick -> 音节。需要先知道旋律轨才能建，见下方 pickMelodyTrack 之后。
    std::map<long, std::string> lyricAt;
    auto lyricForNote = [&](long tick) -> std::string {
        auto lit = lyricAt.find(tick);
        return lit == lyricAt.end() ? std::string() : lit->second;
    };

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

    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks, n.channel);
        if (std::abs(off) >= 0.5) n.pitch += (int)std::round(off);
        byTrack[n.track].push_back(n);
    }

    // 旋律轨：和弦标记与歌词都只给它。判定见 notation.cpp 的 pickMelodyTrack——
    // 有歌词时「LRC 行首能否对齐」是最强证据，比"音符最多"可靠得多
    // （音符最多的那条轨在真实工程里通常是鼓或柱式和弦伴奏）。
    int mainTrack = pickMelodyTrack(data, lyricLines);

    // ---- 歌词对齐 ----
    // 一行 LRC 常常跨两小节，而旋律音数往往多于字数（衬腔、走句、装饰音）。
    // 早先按「第 k 个音领第 k 个字」顺序发，7 个字在第一小节就被一口气用完，
    // 第二小节整段空着——看起来像漏词，也不像能照着唱的谱子。
    //
    // 改为按行内音数均匀分布：第 i 个字落在该行第 floor(i*n/m) 个音上，
    // 让一行词铺满它自己的时间跨度。n <= m（音数不多于字数）时保持顺序分配，
    // 行为与原来一致。
    if (lyricLines && !lineSyls.empty() && mainTrack >= 0) {
        auto mit = byTrack.find(mainTrack);
        if (mit != byTrack.end()) {
            std::vector<long> onsets;
            onsets.reserve(mit->second.size());
            for (const auto& n : mit->second) onsets.push_back(n.startTicks);
            std::sort(onsets.begin(), onsets.end());
            onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());

            const long kInf = std::numeric_limits<long>::max();
            for (size_t li = 0; li < lineSyls.size(); li++) {
                long from = lineSyls[li].first;
                long to = (li + 1 < lineSyls.size()) ? lineSyls[li + 1].first : kInf;
                std::vector<long> inLine;
                for (long t : onsets)
                    if (t >= from && t < to) inLine.push_back(t);

                const auto& syls = lineSyls[li].second;
                const size_t n = inLine.size(), m = syls.size();
                if (n == 0 || m == 0) continue;

                if (n > m) {
                    for (size_t i = 0; i < m; i++) {
                        size_t idx = (size_t)((unsigned long long)i * n / m);
                        if (idx >= n) idx = n - 1;
                        lyricAt[inLine[idx]] = syls[i];
                    }
                } else {
                    for (size_t i = 0; i < n; i++) lyricAt[inLine[i]] = syls[i];
                }
            }
        }
    }

    for (auto& [t, ns] : byTrack) {
        const bool isMain = (t == mainTrack);
        // 速度标记属于整份谱，只在第一行谱上印一次。每轨都印一遍是噪音，
        // 而且同一页上会出现好几个一模一样的 ♩=90。
        const bool showTempo = (t == byTrack.begin()->first);

        // ---- 同音合并：连续同音且首尾相接时并成一个长音 ----
        // 不合并的话同音重复会被画成 `5 - 5 -`，而简谱规范应写成 `5 - - -`。
        //
        // 但这里必须排除多声部：C 和弦的 5 和下一个 G 和弦的 5 音高相同、
        // 时间也相接，合并后 G 和弦就少了一个音。只在「该时刻只有一个音」时
        // 才允许合并。
        //
        // 也不能跨小节线合并：真实演奏里同音相邻几乎都是两次发音而不是延音，
        // 合并会把小节线上的音吞掉（第 15 小节开头的 6 变成 `-`）。
        auto triplets0 = detectTriplets(ns, data.division);
        std::map<long, int> onsetCount;
        for (const auto& n : ns) onsetCount[n.startTicks]++;
        auto isSingle = [&](long tick) {
            auto cit = onsetCount.find(tick);
            return cit == onsetCount.end() || cit->second <= 1;
        };

        std::vector<NoteEvent> merged;
        std::vector<bool> trip;
        for (size_t i = 0; i < ns.size(); i++) {
            if (i > 0 && !triplets0[i] && !triplets0[i - 1] && !merged.empty() &&
                ns[i].pitch == merged.back().pitch &&
                ns[i].startTicks == merged.back().startTicks + merged.back().durationTicks &&
                isSingle(ns[i].startTicks) && isSingle(merged.back().startTicks) &&
                (long)(ns[i].startTicks / barLen) == (long)(merged.back().startTicks / barLen)) {
                merged.back().durationTicks += ns[i].durationTicks;
                merged.back().durationSeconds += ns[i].durationSeconds;
                continue;
            }
            merged.push_back(ns[i]);
            trip.push_back(triplets0[i]);
        }

        body << "<div class='track'><h3>";
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) body << htmlEscape(it->second);
        else body << (isMain ? "旋律轨" : "伴奏轨 " + std::to_string(t));
        body << " <small>(" << merged.size() << " 音符)</small></h3><div class='staff'><div class='row'>";

        // 每行放多少小节：按 16 拍折算（4/4 即 4 小节一行），与简谱歌本的习惯一致。
        // 原来靠 flex 自动换行，每行 3~4 小节不等，整页行宽参差。
        double beatsPerBar = (double)barLen / data.division;
        int measuresPerRow = 1;
        if (beatsPerBar > 0) {
            int n = (int)std::lround(16.0 / beatsPerBar);
            measuresPerRow = n < 1 ? 1 : (n > 8 ? 8 : n);
        }

        // ---- 按小节切分 ----
        // 简谱的 `-` 是「延长一拍」，不能跨过小节线：一个 6 拍的长音若整个画在
        // 一个小节里，该小节就凭空多出 2 拍，小节线位置随之全部错位。
        // 切分后首段照常写数字，后续段（cont）只画 `-`。
        struct HNote { NoteEvent n; bool cont; bool trip; };
        std::vector<HNote> seq;
        for (size_t i = 0; i < merged.size(); i++) {
            const auto& n = merged[i];
            long s = n.startTicks;
            long e = s + std::max<long>(n.durationTicks, 1);
            bool cont = false;
            while (s < e) {
                long barEnd = (long)(((long)(s / barLen) + 1) * barLen);
                long segEnd = std::min(e, barEnd);
                if (segEnd <= s) segEnd = e;   // 防御，避免死循环
                NoteEvent seg = n;
                seg.startTicks = s;
                seg.durationTicks = segEnd - s;
                seq.push_back({seg, cont, cont ? false : (i < trip.size() ? trip[i] : false)});
                s = segEnd;
                cont = true;
            }
        }

        // ---- 同拍分组：同一 startTicks 的音符属于同一个和弦 ----
        std::vector<std::vector<const HNote*>> groups;
        for (const auto& s : seq) {
            if (!groups.empty()) {
                const HNote* prev = groups.back().back();
                if (s.cont == prev->cont && s.n.startTicks == prev->n.startTicks) {
                    groups.back().push_back(&s);
                    continue;
                }
            }
            groups.push_back({&s});
        }

        int curBar = -1;
        long prevEnd = 0;
        std::string lastDyn;
        std::string lastChord;
        int lastTempo = -1;
        bool inTriplet = false;

        // 和弦游标：按 tick 顺序推进。取「不晚于 tick 的最后一个还没标过的
        // 和弦」，于是小节内换和弦、以及和弦落在两个音之间（旋律正拖长音）
        // 这两种情况都不会漏标。
        auto chordIt = chordAt.begin();
        auto chordMarkFor = [&](long tick) -> std::string {
            if (!isMain) return std::string();
            std::string label;
            while (chordIt != chordAt.end() && chordIt->first <= tick) {
                if (chordIt->second != lastChord) label = chordIt->second;
                ++chordIt;
            }
            if (!label.empty()) lastChord = label;
            return label;
        };

        // 开一个小节：上面一行放标记（小节号 / 和弦 / 力度），下面才是音符。
        //
        // 早先把这三样直接塞进音符的 flex 流里，结果它们全挤在音符基线上：
        // 「Cmaj7 ff 1̇」看起来像一个词的三个字母，和弦与力度根本分不开。
        // 分成上下两层后，标记有固定的 17px 高度，各小节音符行也随之顶部对齐。
        auto openBar = [&](int bar, long firstTick, int channel) {
            long barStart = (long)(bar * barLen);
            if (prevEnd < barStart) prevEnd = barStart;
            body << "<span class='measure'><span class='marks'>";

            // 小节号：长谱子靠它定位，从第 2 小节起显示
            if (bar > 0) body << "<span class='barno'>" << (bar + 1) << "</span>";

            // 中途变速：标在速度变化的那一小节。单一速度已经写在页眉，
            // 谱面上再印一遍纯属噪音，所以只在有两个以上速度时显示。
            if (showTempo && data.tempoChanges.size() > 1) {
                int bpm = 0;
                for (const auto& tp : data.tempoChanges) {
                    if (tp.tick <= barStart) bpm = (int)std::lround(tp.bpm);
                    else break;
                }
                if (bpm > 0 && bpm != lastTempo) {
                    body << "<span class='tempo'>♩=" << bpm << "</span>";
                    lastTempo = bpm;
                }
            }


            // 力度记号按通道取，变化时才写
            if (!data.dynamics.empty()) {
                int val = -1;
                for (const auto& dp : data.dynamics) {
                    if (dp.tick > firstTick) break;
                    if (dp.channel == channel) val = dp.value;
                }
                if (val >= 0) {
                    const char* dyn = "mf";
                    if (val < 20) dyn = "pp"; else if (val < 40) dyn = "p";
                    else if (val < 60) dyn = "mp"; else if (val < 80) dyn = "mf";
                    else if (val < 100) dyn = "f"; else dyn = "ff";
                    if (lastDyn != dyn) {
                        body << "<span class='dyn'>" << dyn << "</span>";
                        lastDyn = dyn;
                    }
                }
            }
            body << "</span><span class='notes'>";   // 标记行结束，音符行开始
        };

        // ---- 减时线连通（beam）----
        // 简谱的减时线按「拍」连通：一拍里的两个八分音符共用一条横线，
        // 不是各画一小段。早先每个 .num 各带一条 border-bottom，被音符间距
        // 切开，八分音符看起来像一串断线，三连音更是三个孤立的短线。
        //
        // 做法：把能连成一条线的音符先攒进 beamBuf，遇到断点（换小节、休止、
        // 长音、三连音组结束）才一次性包进 <span class='bgrp'>。
        std::vector<std::string> beamBuf;
        int beamLevel = 0;
        long beamPrevEnd = -1;
        long beamBeat = -1;
        int beamBar = -1;
        bool beamTrip = false;

        auto flushBeam = [&]() {
            if (beamBuf.empty()) return;
            body << "<span class='bgrp";
            if (beamLevel > 1) body << " b" << beamLevel;
            body << "'>";
            for (const auto& h : beamBuf) body << h;
            body << "</span>";
            beamBuf.clear();
            beamLevel = 0;
            beamPrevEnd = -1;
        };

        bool firstNote = true;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const auto& grp = groups[gi];
            const NoteEvent& n = grp[0]->n;
            const bool cont = grp[0]->cont;
            int bar = (int)(n.startTicks / barLen);

            if (bar != curBar) {
                flushBeam();                                  // 小节线截断减时线
                if (curBar >= 0) body << "</span></span>";   // 收尾上一个小节的音符行与小节

                // 补齐开头与中间完全空的小节。
                // 原来这里直接跳过，谱面第一小节标号是「5」、中途还会跳号，
                // 和 MusicXML（那边是补齐的）对不上，也不像一份能照着唱的谱子。
                int from = (curBar >= 0) ? curBar + 1 : 0;
                for (int b = from; b < bar; b++) {
                    if (b > 0 && b % measuresPerRow == 0) body << "</div><div class='row'>";
                    body << "<span class='measure'><span class='marks'>";
                    if (b > 0) body << "<span class='barno'>" << (b + 1) << "</span>";
                    // 旋律还没进来、伴奏已经在走的前奏小节：和弦照标，
                    // 否则开头的 4 小节就是四块空白，看不出和声进行
                    body << "</span><span class='notes'>"
                         << withChord(noteColumn("0", 0, 0, beatsPerBar, ""),
                                      chordMarkFor((long)(b * barLen)))
                         << "</span></span>";
                    prevEnd = std::max(prevEnd, (long)((b + 1) * barLen));
                }

                // 每 measuresPerRow 个小节换一行
                if (bar > 0 && bar % measuresPerRow == 0) body << "</div><div class='row'>";

                curBar = bar;
                openBar(bar, n.startTicks, n.channel);
            } else if (inTriplet && !grp[0]->trip) {
                inTriplet = false;
            }

            // 三连音组的标记
            if (grp[0]->trip && !inTriplet) {
                flushBeam();
                body << "<span class='trip'>3</span>";
                inTriplet = true;
            }

            // 间隙补休止符。原来这里多了一个 `prevEnd > 0` 条件，
            // 导致乐曲开头的休止符（第一个音不在 tick 0）永远不显示。
            long gap = n.startTicks - prevEnd;
            if (gap >= data.division / 2) {
                flushBeam();          // 休止符打断减时线
                double restBeats = (double)gap / data.division;
                body << withChord(noteColumn("0", 0, 0, restBeats, ""),
                                  chordMarkFor(prevEnd));
            }

            // 整组的时值取最长那个音
            long durTicks = 0;
            for (const auto* h : grp) durTicks = std::max(durTicks, h->n.durationTicks);
            double beats = (double)durTicks / data.division;

            if (cont) {
                // 延续段：一拍一个 `-`
                flushBeam();          // 长音打断减时线
                int cnt = (int)std::llround(beats);
                if (cnt < 1) cnt = 1;
                for (int k = 0; k < cnt; k++) {
                    long tk = n.startTicks + (long)k * data.division;
                    body << withChord(noteColumn("-", 0, 0, 1.0, ""), chordMarkFor(tk));
                }
            } else if (grp.size() == 1) {
                std::string jp = pitchToJianpu(n.pitch, key);
                int highDots = 0, lowDots = 0;
                std::string num;
                for (char c : jp) {
                    if (c == '\'') highDots++;
                    else if (c == ',') lowDots++;
                    else num += c;
                }
                // 不要再排除第一个音：休止符是单独插入的列，本来就不参与歌词分配，
                // 多一个 !firstNote 只会把一句词的第一个字吞掉（"风霜染尽客行袍"
                // 变成"风霜染尽客行"+下一小节的"袍"）。
                std::string syl = (isMain && lyricLines && n.pitch != 0)
                                  ? lyricForNote(n.startTicks) : std::string();
                // 三连音走单独的时值档位，避免 1/3 拍被当成附点十六分
                DurStyle ds = grp[0]->trip ? tripletStyleOf(beats) : durStyleOf(beats);
                std::string col = withChord(
                    noteColumn(num, highDots, lowDots, beats, syl, "", &ds),
                    chordMarkFor(n.startTicks));

                // 能否接上上一条减时线：同小节、首尾相接、减时线条数相同，
                // 且同属一拍（三连音组例外，它本来就跨拍）。
                long beatLen = beatsPerBar > 0 ? (long)std::llround(barLen / beatsPerBar)
                                               : (long)data.division;
                long curBeat = beatLen > 0 ? n.startTicks / beatLen : 0;
                long endTick = n.startTicks + std::max<long>(n.durationTicks, 1);
                bool join = ds.beams > 0 && ds.beams == beamLevel && !beamBuf.empty() &&
                            bar == beamBar && n.startTicks == beamPrevEnd &&
                            grp[0]->trip == beamTrip &&
                            (beamTrip || curBeat == beamBeat);

                if (ds.beams > 0) {
                    if (!join) {
                        flushBeam();
                        beamLevel = ds.beams;
                        beamBar = bar;
                        beamBeat = curBeat;
                        beamTrip = grp[0]->trip;
                    }
                    beamBuf.push_back(col);
                    beamPrevEnd = endTick;
                } else {
                    flushBeam();
                    body << col;
                }
            } else {
                flushBeam();          // 和弦自成一列，不与单音连线
                // 多声部：竖向叠写。按音高降序，高音在上、低音在下，
                // 与五线谱的视觉方向一致（反过来看会很不习惯）。
                std::vector<NoteEvent> sorted;
                for (const auto* h : grp) sorted.push_back(h->n);
                std::sort(sorted.begin(), sorted.end(),
                          [](const NoteEvent& a, const NoteEvent& b) { return a.pitch > b.pitch; });

                std::vector<std::pair<std::string, std::pair<int, int>>> tones;
                for (const auto& t : sorted) {
                    std::string jp = pitchToJianpu(t.pitch, key);
                    int hi = 0, lo = 0;
                    std::string num;
                    for (char c : jp) {
                        if (c == '\'') hi++;
                        else if (c == ',') lo++;
                        else num += c;
                    }
                    tones.push_back({num, {hi, lo}});
                }
                body << withChord(chordColumn(tones, beats, ""), chordMarkFor(n.startTicks));
            }

            for (const auto* h : grp) prevEnd = std::max(prevEnd, h->n.startTicks + h->n.durationTicks);
            firstNote = false;
        }

        flushBeam();   // 收尾：把最后一条减时线画完再闭合小节
        body << "</span></span><span class='final-bar'></span></div></div></div>";
    }

    std::ostringstream html;
    html << "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
         << "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         << "<title>" << htmlEscape(displayTitle) << "</title>"
         << R"(<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='0.9em' font-size='90'>1</text></svg>"><style>
*{box-sizing:border-box}
body{font-family:"Noto Serif SC","Source Han Serif SC","SimSun",serif;max-width:960px;margin:36px auto;background:#fff;color:#222;padding:0 20px}
h1{font-size:26px;text-align:center;color:#b03030;margin:0 0 6px;font-weight:700}
.meta{color:#888;margin-bottom:26px;font-size:13px;text-align:center}

.track{padding:16px 0;margin-bottom:6px;border-bottom:1px dashed #eee}
.track:last-of-type{border-bottom:none}
.track h3{margin:0 0 10px;font-size:14px;color:#666;text-align:center;font-weight:600}
.track h3 small{color:#aaa;font-weight:normal;font-size:12px}

/* 谱面：每行固定若干小节，行内横排。原来用 flex 自动换行，行宽参差不齐。 */
.staff{display:block;font-size:26px;padding:4px 0}
.row{display:flex;align-items:flex-start;padding-top:10px}

/* 一个小节 = 上层标记行 + 下层音符行。
   早先小节号/和弦/力度和音符挤在同一条 flex 基线上，"Cmaj7 ff 1̇" 看起来
   像一个词的三个字母，和弦与力度根本分不开。分层后标记行高度固定 17px，
   各小节的音符行也随之顶部对齐。 */
.measure{display:flex;flex-direction:column;border-left:1.5px solid #bbb;
         padding:0 0 0 9px;margin:0 16px 0 0}
.marks{display:flex;align-items:flex-end;gap:8px;height:20px;padding-bottom:3px;
       line-height:1;white-space:nowrap}
.barno{font-size:10px;color:#c9c9c9;line-height:1;align-self:flex-start}
.dyn{font-size:14px;font-style:italic;color:#999;line-height:1}
.tempo{font-size:11px;font-weight:600;color:#4a6fa5;line-height:1;
       border:1px solid #cdd8e8;border-radius:3px;padding:1px 4px;background:#f4f7fc}

/* 和弦标记：浮在它所管的那个音符上方，不占宽度。
   位置必须跟音符走——标记行是从左往右排的，一旦前面多了一个速度标记，
   和弦就被顶右移，和第 3 拍的和弦叠在一起（`C` 与 `Am` 会糊成一团）。
   .notes 留 14px 上边距当和弦带，浮标底边贴住音符列上沿：
   上面是小节号/力度/速度那一行，下面是音符，三者互不侵入。
   也不能改成普通同行元素：那会把这一拍的音符间距撑开，谱面就参差不齐。 */
.chordx{position:absolute;bottom:calc(100% + 4px);left:50%;transform:translateX(-50%);
        font-size:13px;font-weight:600;font-style:normal;color:#b03030;
        line-height:1;white-space:nowrap;pointer-events:none}
.trip{font-size:12px;color:#666;align-self:flex-start;margin-right:2px;line-height:1.6}
.notes{display:flex;align-items:flex-start;padding-top:18px}

/* ---- 音符列 ----
   .note 的宽度只由数字决定：延音线用绝对定位挂在 .core 右侧，并通过 --n
   给右侧预留 padding。这样 .oct（八度点）和 .lyric（歌词）都会精确对齐数字。 */
.note{display:inline-flex;flex-direction:column;align-items:center;position:relative;
      margin:0 6px;vertical-align:top;padding-right:calc(var(--n,0) * 1.15em)}
.oct{display:flex;flex-direction:column;align-items:center;gap:2px;height:12px;flex:none}
.oct.low{margin-top:2px}
.oct i{display:block;width:5px;height:5px;border-radius:50%;background:#222}
.core{position:relative;display:inline-block;line-height:1.15}
.num{font-weight:600;font-size:26px;line-height:1.15;display:inline-block}
.exts{position:absolute;left:100%;top:50%;transform:translateY(-42%);
      display:flex;white-space:nowrap}
.ext{font-size:26px;line-height:1;margin-left:7px;color:#222}
.lyric{font-size:14px;color:#666;margin-top:3px;min-height:18px;line-height:1.2;white-space:nowrap}

/* 和弦：同一时刻的多个音竖向叠写，共用一个时值线。
   行高比单音列高，所以整行会被撑高——这是简谱表示和弦的固有代价。 */
.chordcol .core{display:inline-block}
/* 和弦的延音线对齐整个和弦的底部（最低音那一行），而不是垂直居中：
   居中的话看起来像只有中间那个音在延长。 */
.chordcol .exts{top:auto;bottom:2px;transform:none}
.stack{display:flex;flex-direction:column;align-items:center}
.tone{display:flex;flex-direction:column;align-items:center;margin:-7px 0}
.tone .oct{height:8px;gap:1px}
.tone .oct i{width:4px;height:4px}
.tone .num{font-size:24px;line-height:1.05}

/* 减时线：八分 1 条、十六分 2 条、三十二分 3 条 */
.eighth .num,.sixteenth .num,.thirtysecond .num{border-bottom:2px solid #222;padding-bottom:1px}
.sixteenth .num{box-shadow:0 4px 0 #222}
.thirtysecond .num{box-shadow:0 4px 0 #222,0 8px 0 #222}

/* 连成一条的减时线：一拍内的同组音符共用一条横线，而不是各自一小段。
   .bgrp 里取消逐音符的短线，改用一条跨全组的绝对定位线；十六分/三十二分
   再向下每 4px 叠一条平行线。line 的纵向位置 = 八度点区(12px) + 数字行高，
   与 .num 的 border-bottom 原本所在的位置对齐。 */
.bgrp{position:relative;display:inline-flex;align-items:flex-start}
.bgrp>.note{margin:0 4px}
.bgrp .num{border-bottom:none;box-shadow:none}
.bgrp::after{content:"";position:absolute;left:1px;right:1px;
             top:calc(12px + 1.15em - 1px);height:2px;background:#222}
.bgrp.b2::after{box-shadow:0 4px 0 #222}
.bgrp.b3::after{box-shadow:0 4px 0 #222,0 8px 0 #222}

/* 和弦的减时线只画在最下面那个音上：叠写的每个音各画一条，看起来像十六分音符 */
.chordcol .tone .num{border-bottom:none;box-shadow:none}
.chordcol.eighth .tone:last-child .num,
.chordcol.sixteenth .tone:last-child .num,
.chordcol.thirtysecond .tone:last-child .num{border-bottom:2px solid #222;padding-bottom:1px}
.chordcol.sixteenth .tone:last-child .num{box-shadow:0 4px 0 #222}
.chordcol.thirtysecond .tone:last-child .num{box-shadow:0 4px 0 #222,0 8px 0 #222}
.dotted .num::after{content:"";display:inline-block;width:5px;height:5px;border-radius:50%;
                    background:#222;margin-left:4px;vertical-align:middle}

.final-bar{display:inline-block;width:2px;height:34px;background:#333;margin-left:10px;
           align-self:flex-start;margin-top:26px;position:relative}
.final-bar::after{content:"";position:absolute;left:5px;top:0;width:4px;height:34px;background:#333}
.footer{color:#bbb;font-size:12px;text-align:center;margin:24px 0 10px}

@media print{
  body{max-width:none;margin:0;padding:0}
  .track{page-break-inside:auto;border-bottom:none}
  .measure{page-break-inside:avoid}
  .barno{color:#ddd}
}
@media (max-width:600px){
  .staff{font-size:20px}
  .num,.ext{font-size:20px}
  .lyric{font-size:12px}
}
</style></head><body>
<h1>)" << htmlEscape(displayTitle) << R"(</h1>
<div class="meta">)"
         << htmlEscape(keyDescription(key))
         << " ｜ " << data.timeSig.toString()
         << " ｜ " << (data.keySig.valid ? "MIDI调号" : "自动猜测")
         << (tempoSummary(data).empty() ? "" : " ｜ " + tempoSummary(data))
         << (data.drumNotesSkipped > 0 ? " ｜ 已跳过鼓轨" : "")
         << "</div>" << body.str()
         << "<footer class='footer'>由 midi2jianpu v" M2J_VERSION " 生成</footer>"
         << "</body></html>";
    return html.str();
}

} // namespace m2j

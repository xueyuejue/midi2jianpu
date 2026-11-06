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

// 时值 -> CSS class + 延音线条数
struct DurStyle {
    const char* cls;
    bool dotted;
    int extLines;   // 延音线数量：二分 1 条、附点二分 2 条、全音符 3 条
};

static DurStyle durStyleOf(double beats) {
    static const struct { double beats; DurStyle st; } table[] = {
        {0.125, {"thirtysecond", false, 0}},
        {0.25,  {"sixteenth",    false, 0}},
        {0.375, {"sixteenth",    true,  0}},
        {0.5,   {"eighth",       false, 0}},
        {0.75,  {"eighth",       true,  0}},
        {1.0,   {"quarter",      false, 0}},
        {1.5,   {"quarter",      true,  0}},
        {2.0,   {"half",         false, 1}},
        {3.0,   {"half",         true,  2}},
        {4.0,   {"whole",        false, 3}},
        {6.0,   {"whole",        true,  5}},
    };
    double best = 1e9;
    DurStyle out{"quarter", false, 0};
    for (auto& t : table) {
        double d = std::abs(beats - t.beats);
        if (d < best) { best = d; out = t.st; }
    }
    return out;
}

// 生成一个音符的 HTML 列。
//
// 关键点：延音线不参与宽度计算（绝对定位挂在 .core 右侧），所以 .note 的
// 实际宽度由数字决定，八度点和歌词都对准数字中心——这是简谱的硬性要求，
// 否则 `1 - - -` 的低八度点会飘到整个音符的正中，偏离数字好远。
static std::string noteColumn(const std::string& numText, int highDots, int lowDots,
                              double beats, const std::string& lyric,
                              const std::string& extraClass = "") {
    DurStyle ds = durStyleOf(beats);

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
    displayTitle += " v";
    displayTitle += M2J_VERSION;

    std::ostringstream body;
    double barLen = data.timeSig.barLengthTicks(data.division);
    if (!(barLen > 0)) barLen = data.division * 4.0;

    // 和弦检测（按小节起点索引）
    auto chords = detectChords(data, key);
    std::map<long, std::string> chordByBar;
    for (const auto& c : chords) chordByBar[c.startTick] = c.name;

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

    int curLineIdx = -1;
    int curCharIdx = 0;
    auto lyricForNote = [&](long tick) -> std::string {
        if (lineSyls.empty()) return "";
        int bestLine = -1;
        for (int i = 0; i < (int)lineSyls.size(); i++) {
            if (lineSyls[i].first <= tick) bestLine = i;
            else break;
        }
        if (bestLine < 0) return "";
        if (bestLine != curLineIdx) {
            // 上一行还剩一两个字没分配完时继续给它，避免歌词丢字
            if (curLineIdx >= 0 && curCharIdx < (int)lineSyls[curLineIdx].second.size()) {
                int remaining = (int)lineSyls[curLineIdx].second.size() - curCharIdx;
                if (remaining <= 2) return lineSyls[curLineIdx].second[curCharIdx++];
            }
            curLineIdx = bestLine;
            curCharIdx = 0;
        }
        if (curCharIdx >= (int)lineSyls[curLineIdx].second.size()) return "";
        return lineSyls[curLineIdx].second[curCharIdx++];
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

    // 音符最多的轨道视作旋律轨：和弦标记与歌词都只给它
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, ns] : byTrack)
        if (ns.size() > maxN) { maxN = ns.size(); mainTrack = t; }

    for (auto& [t, ns] : byTrack) {
        const bool isMain = (t == mainTrack);

        // ---- 同音合并：连续同音且首尾相接时并成一个长音 ----
        // 不合并的话同音重复会被画成 `5 - 5 -`，而简谱规范应写成 `5 - - -`。
        //
        // 但这里必须排除多声部：C 和弦的 5 和下一个 G 和弦的 5 音高相同、
        // 时间也相接，合并后 G 和弦就少了一个音。只在「该时刻只有一个音」时
        // 才允许合并。
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
                isSingle(ns[i].startTicks) && isSingle(merged.back().startTicks)) {
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
        bool inTriplet = false;

        // 开一个小节：写小节号、和弦标记、力度记号
        auto openBar = [&](int bar, long firstTick, int channel) {
            long barStart = (long)(bar * barLen);
            if (prevEnd < barStart) prevEnd = barStart;
            body << "<span class='measure'>";

            // 小节号：长谱子靠它定位，从第 2 小节起显示
            if (bar > 0) body << "<span class='barno'>" << (bar + 1) << "</span>";

            // 和弦标记只标旋律轨
            auto chordIt = chordByBar.find(barStart);
            if (isMain && chordIt != chordByBar.end())
                body << "<span class='chord'>" << htmlEscape(chordIt->second) << "</span>";

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
        };

        bool firstNote = true;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const auto& grp = groups[gi];
            const NoteEvent& n = grp[0]->n;
            const bool cont = grp[0]->cont;
            int bar = (int)(n.startTicks / barLen);

            if (bar != curBar) {
                if (curBar >= 0) {
                    body << "</span>";                 // 收尾上一个小节
                    // 每 measuresPerRow 个小节换一行
                    if (bar % measuresPerRow == 0) body << "</div><div class='row'>";
                }
                curBar = bar;
                openBar(bar, n.startTicks, n.channel);
            } else if (inTriplet && !grp[0]->trip) {
                inTriplet = false;
            }

            // 三连音组的标记
            if (grp[0]->trip && !inTriplet) {
                body << "<span class='trip'>3</span>";
                inTriplet = true;
            }

            // 间隙补休止符。原来这里多了一个 `prevEnd > 0` 条件，
            // 导致乐曲开头的休止符（第一个音不在 tick 0）永远不显示。
            long gap = n.startTicks - prevEnd;
            if (gap >= data.division / 2) {
                double restBeats = (double)gap / data.division;
                body << noteColumn("0", 0, 0, restBeats, "");
            }

            // 整组的时值取最长那个音
            long durTicks = 0;
            for (const auto* h : grp) durTicks = std::max(durTicks, h->n.durationTicks);
            double beats = (double)durTicks / data.division;

            if (cont) {
                // 延续段：一拍一个 `-`
                int cnt = (int)std::llround(beats);
                if (cnt < 1) cnt = 1;
                for (int k = 0; k < cnt; k++) body << noteColumn("-", 0, 0, 1.0, "");
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
                body << noteColumn(num, highDots, lowDots, beats, syl);
            } else {
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
                body << chordColumn(tones, beats, "");
            }

            for (const auto* h : grp) prevEnd = std::max(prevEnd, h->n.startTicks + h->n.durationTicks);
            firstNote = false;
        }

        body << "</span><span class='final-bar'></span></div></div></div>";
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
.row{display:flex;align-items:flex-start;padding-top:15px}
.measure{display:flex;align-items:flex-start;position:relative;border-left:1.5px solid #bbb;
         padding:0 0 0 9px;margin:0 16px 0 0}

/* 小节号放在小节左上角外侧，不再插在音符流里挤占位置 */
.barno{position:absolute;top:-13px;left:1px;font-size:10px;color:#c9c9c9;line-height:1}
.chord{font-size:14px;font-weight:600;color:#b03030;align-self:flex-start;margin-right:6px;line-height:1.6}
.dyn{font-size:14px;font-style:italic;color:#999;align-self:flex-start;margin-right:6px;line-height:1.6}
.trip{font-size:12px;color:#666;align-self:flex-start;margin-right:2px;line-height:1.6}

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
.tone{display:flex;flex-direction:column;align-items:center;margin:-5px 0}
.tone .oct{height:8px;gap:1px}
.tone .oct i{width:4px;height:4px}
.tone .num{font-size:24px;line-height:1.05}

/* 减时线：八分 1 条、十六分 2 条、三十二分 3 条 */
.eighth .num,.sixteenth .num,.thirtysecond .num{border-bottom:2px solid #222;padding-bottom:1px}
.sixteenth .num{box-shadow:0 4px 0 #222}
.thirtysecond .num{box-shadow:0 4px 0 #222,0 8px 0 #222}
.dotted .num::after{content:"";display:inline-block;width:5px;height:5px;border-radius:50%;
                    background:#222;margin-left:4px;vertical-align:middle}

.final-bar{display:inline-block;width:2px;height:34px;background:#333;margin-left:10px;align-self:center;position:relative}
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
<div class="meta">1 = )"
         << htmlEscape(keyDescription(key))
         << " ｜ " << data.timeSig.toString()
         << " ｜ " << (data.keySig.valid ? "MIDI调号" : "自动猜测")
         << "</div>" << body.str()
         << "<footer class='footer'>由 midi2jianpu 生成</footer>"
         << "</body></html>";
    return html.str();
}

} // namespace m2j

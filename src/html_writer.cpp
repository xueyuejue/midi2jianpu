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

// 断音 / 重音 / 滑音记号同样浮在音符列里、不占宽度。
// 放在右上角而不是正中：上八度点占的是正中那 12px，两者会叠在一起。
static std::string withArticulation(const std::string& col, const std::string& art) {
    if (art.empty()) return col;
    size_t p = col.find('>');
    if (p == std::string::npos) return col;
    return col.substr(0, p + 1) +
           "<span class='artic'>" + htmlEscape(art) + "</span>" +
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
    const std::string lyricLabel = lyricSourceLabel(data, lyricLines);

    std::ostringstream body;
    // 小节网格（含弱起）。三个输出模块共用同一套换算，见 notation.hpp 的 BarGrid
    const BarGrid grid = barGridOf(data);
    const long barLen = grid.barLen;
    // 调性也和网格一样是「按位置查表」：中途转调后同一个 MIDI 音高的唱名
    // 要随所在小节重算，页眉与谱面标记都由它给出。
    const KeyMap keys(data, key);
    // 力度 / 渐强渐弱 / 断音 / 滑音 / 段落：三个输出层共用同一份判定结果。
    const ExpressionMap expr(data);

    // 和弦检测。按「和声窗」而不是整小节（4/4 半小节、6/8 附点四分），
    // 所以索引键是准确的 tick，同一小节里可以有多个和弦。
    auto chords = detectChords(data, key);
    std::map<long, std::string> chordAt;
    for (const auto& c : chords) chordAt[c.startTick] = c.name;

    // 速度标记：折叠后的记号（阶跃 / 渐快 / 渐慢），再把「记号落在第几小节」
    // 预先算成一张表。同一个小节里有多个记号时保留第一个（后面的会在
    // 标记行里挤成一团，而且实用上也不会有一小节里变两次速的谱子）。
    const std::vector<TempoMark> tempoMarks = tempoMarksOf(data);
    std::map<long, size_t> tempoAtBar;
    for (size_t i = 0; i < tempoMarks.size(); i++) {
        const long b = grid.barIndex(tempoMarks[i].tick);
        if (tempoAtBar.find(b) == tempoAtBar.end()) tempoAtBar[b] = i;
    }

    // 歌词分配：HTML 的连音线弧线和 MusicXML 的 syllabic/slur 共用同一份结果。
    // 早先这里自己拆字、自己算「第几个音领第几个字」，导出 MusicXML 时整份
    // 歌词就丢了；现在统一走 notation.cpp 的 assignLyrics。
    LyricAssignment lyrics;
    auto lyricForNote = [&](long tick) -> std::string {
        auto lit = lyrics.at.find(tick);
        return lit == lyrics.at.end() ? std::string() : lit->second.text;
    };

    // 弯音修正（只统计同通道）
    auto bendOffset = [&](long start, long end, int channel) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.channel == channel && pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * pb.range;
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
    // 一行 LRC 常常跨两小节、音多于字，要按行内音数均匀铺开；MIDI 自带的
    // 0x05 歌词则每个音节都带精确 tick，不能抹平。两种来源由 resolveLyrics
    // 统一成同一份 LyricAssignment（显式 --lyrics 优先，否则自动用内置歌词），
    // 下面画连音线、写歌词字都只读这份结果。
    if (mainTrack >= 0) lyrics = resolveLyrics(data, lyricLines, mainTrack);

    // 同轨复调：一条轨常常同时装着两个声部（钢琴左右手写在同一轨、伴奏垫在
    // 人声轨下面）。简谱一行写不下两个声部，所以按声部拆成独立谱行；
    // 单声部轨（绝大多数文件）与旧版输出逐字节一致。
    struct PartView {
        int track = 0;
        int voice = 0;
        int voiceCount = 1;
        int channel = -1;
        std::vector<NoteEvent> notes;
    };
    std::vector<PartView> parts;
    for (auto& [t, ns] : byTrack) {
        const std::vector<int> vo = splitVoices(
            ns, 3, [&](const NoteEvent& n) { return expr.isGraceNote(n.startTicks, n.channel); });
        int voiceCount = 0;
        for (int v : vo) voiceCount = std::max(voiceCount, v + 1);
        if (voiceCount <= 1) {
            const int ch = dominantChannel(ns);
            parts.push_back({t, 0, 1, ch, std::move(ns)});
            continue;
        }
        for (int v = 0; v < voiceCount; v++) {
            PartView pv;
            pv.track = t;
            pv.voice = v;
            pv.voiceCount = voiceCount;
            for (size_t i = 0; i < ns.size(); i++)
                if (vo[i] == v) pv.notes.push_back(ns[i]);
            pv.channel = dominantChannel(pv.notes);
            if (!pv.notes.empty()) parts.push_back(std::move(pv));
        }
    }

    for (size_t pi = 0; pi < parts.size(); pi++) {
        const int t = parts[pi].track;
        const auto& ns = parts[pi].notes;
        const int partChannel = parts[pi].channel;
        const bool isMain = (t == mainTrack && parts[pi].voice == 0);
        // 速度标记属于整份谱，只在第一行谱上印一次。每轨都印一遍是噪音，
        // 而且同一页上会出现好几个一模一样的 ♩=90。
        const bool showTempo = (pi == 0);

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
                grid.barIndex(ns[i].startTicks) == grid.barIndex(merged.back().startTicks)) {
                merged.back().durationTicks += ns[i].durationTicks;
                merged.back().durationSeconds += ns[i].durationSeconds;
                continue;
            }
            merged.push_back(ns[i]);
            trip.push_back(triplets0[i]);
        }

        // 只有旋律轨、且真的存在一字多音时才抬高和弦带：其余谱面的行距
        // 保持原样，不因为一个新功能把整页撑高。
        const bool hasSlur = (isMain && !lyrics.melismas.empty());
        body << "<div class='track'><h3>";
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) body << htmlEscape(it->second);
        else body << ((t == mainTrack) ? "旋律轨" : "伴奏轨 " + std::to_string(t));
        // 复调轨按声部拆成多行：标题要标清楚这是第几声部，否则同一轨名
        // 连着出现三四次，读者会以为是导出错了。
        if (parts[pi].voiceCount > 1)
            body << " <span class='voiceTag'>声部 " << (parts[pi].voice + 1) << "</span>";
        body << " <small>(" << merged.size() << " 音符)</small></h3><div class='staff"
             << (hasSlur ? " hasslur" : "") << "'><div class='row'>";

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
                long barEnd = grid.barEnd(grid.barIndex(s));
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
        size_t lastTempoIdx = (size_t)-1;
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
            long barStart = grid.barStart(bar);
            if (prevEnd < barStart) prevEnd = barStart;
            body << "<span class='measure'><span class='marks'>";

            // 小节号：长谱子靠它定位，从第 2 小节起显示。
            // 弱起小节不编号，第一个完整小节才是 1（见 BarGrid::measureNumber）
            if (bar > 0) body << "<span class='barno'>" << grid.measureNumber(bar) << "</span>";

            // 中途变拍号：把新拍号标在变化那一小节的标记行。
            // 页眉的摘要说明「什么时候变」，这里说明「从哪一栏开始按新拍子数」。
            if (showTempo && grid.meterChangedAt(bar))
                body << "<span class='meter'>" << grid.sigForBar(bar).toString() << "</span>";

            // 中途转调：标在换调那一小节。页眉写完整链条，这里指出
            // 「从哪一栏起按新调唱」，与拍号 / 速度标记同在标记行。
            if (keys.keyChangedAtBar(bar, grid))
                body << "<span class='keychg'>转 "
                     << "1=" << keyName(keys.keyAtBar(bar, grid)) << "</span>";

            // 中途变速：标在速度变化落在的那一小节。单一速度已经写在页眉，
            // 谱面上再印一遍纯属噪音，所以只在有两个以上记号时显示。
            //
            // 旧实现是「每个小节采样一次当前速度」，遇到渐慢（MIDI 里几十个
            // 连续 set_tempo）会每一小节都冒出一个 ♩=116 / ♩=112 / ♩=108，
            // 满屏无意义数字。现在改走 tempoMarksOf 的折叠结果：阶跃标
            // `♩=90`，渐变标一次 `rit. 120→60`（只标起点，不再逐小节采样）。
            if (showTempo && tempoMarks.size() > 1) {
                auto tm = tempoAtBar.find(bar);
                if (tm != tempoAtBar.end() && tm->second != lastTempoIdx) {
                    const TempoMark& m = tempoMarks[tm->second];
                    if (m.kind == TempoMarkKind::Ritardando) {
                        body << "<span class='tempo ramp'>rit. " << m.bpm
                             << "→" << m.toBpm << "</span>";
                    } else if (m.kind == TempoMarkKind::Accelerando) {
                        body << "<span class='tempo ramp'>accel. " << m.bpm
                             << "→" << m.toBpm << "</span>";
                    } else {
                        body << "<span class='tempo'>♩=" << m.bpm << "</span>";
                    }
                    lastTempoIdx = tm->second;
                }
            }


            // 力度记号按通道取、变化时才写。分级表由 ExpressionMap 提供，
            // 与文本谱、MusicXML 完全一致（旧版这里自己写了一套阈值）。
            const std::string dyn = expr.dynamicAt(firstTick, channel);
            if (!dyn.empty() && lastDyn != dyn) {
                body << "<span class='dyn'>" << dyn << "</span>";
                lastDyn = dyn;
            }
            // 段落名是整份谱的全局标记，只印第一行谱；渐强渐弱是通道级
            // 演奏表情，每一行谱都按自己的主导通道各印一次。旧版两者共用
            // showTempo（= 第一行谱），第二行起的 cresc./dim. 全被吞掉。
            for (const auto& m : expr.inRange(grid.barStart(bar), grid.barEnd(bar), channel)) {
                if (showTempo && !m.section.empty())
                    body << "<span class='section'>" << htmlEscape(m.section) << "</span>";
                if (m.wedge > 0) body << "<span class='wedge'>cresc.</span>";
                else if (m.wedge < 0) body << "<span class='wedge'>dim.</span>";
            }
            body << "</span><span class='notes'>";   // 标记行结束，音符行开始
        };

        // ---- 一字多音的连音线（slur）----
        // 一个字拖着好几个音唱时，简谱要在这些音上方画一条弧线。做法是
        // 把「同组且连续输出」的音符列攒进 slurBuf，组结束（换字、换小节、
        // 休止）时一次性包进 <span class='sgrp'>，用 ::before 画一条跨全组的
        // 弧线——和减时线组 bgrp 是同一个思路。
        //
        // 不能给每个音符各画一段弧：那样得到的是一串波浪而不是一条连音线；
        // 也不能用 JS 量坐标：HTML 要能离线打开、直接打印。
        std::vector<std::string> slurBuf;
        int curSlur = 0;
        auto flushSlur = [&]() {
            if (slurBuf.empty()) return;
            // 单个音画不出弧线（跨小节被切断时就剩一列），直接吐出去
            if (slurBuf.size() >= 2) {
                body << "<span class='sgrp'>";
                for (const auto& h : slurBuf) body << h;
                body << "</span>";
            } else {
                body << slurBuf.front();
            }
            slurBuf.clear();
        };
        auto emitWithGroup = [&](const std::string& colHtml, int g) {
            if (g != curSlur) { flushSlur(); curSlur = g; }
            if (g > 0) slurBuf.push_back(colHtml);
            else body << colHtml;
        };
        // forceBreak 给休止符用：歌词拖过休止时不把「0」也圈进连音线
        auto emit = [&](const std::string& colHtml, long tick, bool forceBreak = false) {
            emitWithGroup(colHtml, (hasSlur && !forceBreak) ? lyrics.groupAt(tick) : 0);
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
        int beamGroup = 0;      // 减时线组所属的一字多音组，避免一条线跨两个字
        long beamPrevEnd = -1;
        long beamBeat = -1;
        int beamBar = -1;
        bool beamTrip = false;

        auto flushBeam = [&]() {
            if (beamBuf.empty()) return;
            std::ostringstream g;
            g << "<span class='bgrp";
            if (beamLevel > 1) g << " b" << beamLevel;
            g << "'>";
            for (const auto& h : beamBuf) g << h;
            g << "</span>";
            emitWithGroup(g.str(), beamGroup);
            beamBuf.clear();
            beamLevel = 0;
            beamGroup = 0;
            beamPrevEnd = -1;
        };

        bool firstNote = true;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const auto& grp = groups[gi];
            const NoteEvent& n = grp[0]->n;
            const bool cont = grp[0]->cont;
            int bar = (int)grid.barIndex(n.startTicks);

            // 倚音（装饰音）：整组跳过，只在它后面的主音上用小字上标呈现。
            // 否则倚音会自己占一格，把主音挤到下一拍，整行节奏错位。
            if (expr.isGraceNote(n.startTicks, n.channel)) {
                prevEnd = std::max(prevEnd, n.startTicks + n.durationTicks);
                firstNote = false;
                continue;
            }

            if (bar != curBar) {
                flushBeam();                                  // 小节线截断减时线
                flushSlur();                                  // 小节线同样截断连音线
                curSlur = 0;                                  // 跨小节的组各画各的一段弧
                if (curBar >= 0) body << "</span></span>";   // 收尾上一个小节的音符行与小节

                // 补齐开头与中间完全空的小节。
                // 原来这里直接跳过，谱面第一小节标号是「5」、中途还会跳号，
                // 和 MusicXML（那边是补齐的）对不上，也不像一份能照着唱的谱子。
                int from = (curBar >= 0) ? curBar + 1 : 0;
                for (int b = from; b < bar; b++) {
                    if (b > 0 && b % measuresPerRow == 0) body << "</div><div class='row'>";
                    body << "<span class='measure'><span class='marks'>";
                    if (b > 0) body << "<span class='barno'>" << grid.measureNumber(b) << "</span>";
                    if (showTempo && grid.meterChangedAt(b))
                        body << "<span class='meter'>" << grid.sigForBar(b).toString() << "</span>";
                    if (keys.keyChangedAtBar(b, grid))
                        body << "<span class='keychg'>转 "
                             << "1=" << keyName(keys.keyAtBar(b, grid)) << "</span>";
                    // 段落标记也可能落在纯休止的前奏小节上；渐变按本行主导通道
                    // 取，与正常小节的 openBar 保持同一口径（不能用 n.channel，
                    // 它可能是同轨混进来的伴奏通道）。
                    for (const auto& m : expr.inRange(grid.barStart(b), grid.barEnd(b), partChannel)) {
                        if (showTempo && !m.section.empty())
                            body << "<span class='section'>" << htmlEscape(m.section) << "</span>";
                        if (m.wedge > 0) body << "<span class='wedge'>cresc.</span>";
                        else if (m.wedge < 0) body << "<span class='wedge'>dim.</span>";
                    }
                    // 旋律还没进来、伴奏已经在走的前奏小节：和弦照标，
                    // 否则开头的 4 小节就是四块空白，看不出和声进行。
                    // 休止时值按本小节的实际长度算——弱起小节比正常小节短
                    double restBeats = (double)grid.barSpan(b) / data.division;
                    body << "</span><span class='notes'>"
                         << withChord(noteColumn("0", 0, 0, restBeats, ""),
                                      chordMarkFor(grid.barStart(b)))
                         << "</span></span>";
                    prevEnd = std::max(prevEnd, grid.barEnd(b));
                }

                // 每 measuresPerRow 个小节换一行
                if (bar > 0 && bar % measuresPerRow == 0) body << "</div><div class='row'>";

                curBar = bar;
                openBar(bar, n.startTicks, partChannel);
            } else if (inTriplet && !grp[0]->trip) {
                inTriplet = false;
            }

            // 三连音组的标记
            if (grp[0]->trip && !inTriplet) {
                flushBeam();
                flushSlur();          // 「3」是独立标记，不能混进连音线组
                curSlur = 0;
                body << "<span class='trip'>3</span>";
                inTriplet = true;
            }

            // 间隙补休止符。原来这里多了一个 `prevEnd > 0` 条件，
            // 导致乐曲开头的休止符（第一个音不在 tick 0）永远不显示。
            long gap = n.startTicks - prevEnd;
            if (gap >= data.division / 2) {
                flushBeam();          // 休止符打断减时线
                double restBeats = (double)gap / data.division;
                emit(withChord(noteColumn("0", 0, 0, restBeats, ""),
                               chordMarkFor(prevEnd)), prevEnd, true);
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
                    emit(withChord(noteColumn("-", 0, 0, 1.0, ""), chordMarkFor(tk)), tk);
                }
            } else if (grp.size() == 1) {
                std::string jp = pitchToJianpu(n.pitch, keys.at(n.startTicks));
                int highDots = 0, lowDots = 0;
                std::string num;
                for (char c : jp) {
                    if (c == '\'') highDots++;
                    else if (c == ',') lowDots++;
                    else num += c;
                }
                // 倚音（装饰音）：先快速滑/弹一个音再落到主音，谱面上用小字上标
                // 写在主音数字前面，如 <sup>3</sup>5，不参与节奏计数。
                const NoteExpression ne = expr.noteAt(n.startTicks, n.channel);
                std::string graceHtml;
                if (ne.hasGrace())
                    graceHtml = "<span class='grace'>" +
                                htmlEscape(pitchToJianpu(ne.gracePitch, keys.at(n.startTicks))) +
                                "</span>";
                // 不要再排除第一个音：休止符是单独插入的列，本来就不参与歌词分配，
                // 多一个 !firstNote 只会把一句词的第一个字吞掉（"风霜染尽客行袍"
                // 变成"风霜染尽客行"+下一小节的"袍"）。
                std::string syl = (isMain && !lyrics.empty() && n.pitch != 0)
                                  ? lyricForNote(n.startTicks) : std::string();
                // 三连音走单独的时值档位，避免 1/3 拍被当成附点十六分
                DurStyle ds = grp[0]->trip ? tripletStyleOf(beats) : durStyleOf(beats);
                std::string col = graceHtml + withArticulation(
                    withChord(noteColumn(num, highDots, lowDots, beats, syl, "", &ds),
                              chordMarkFor(n.startTicks)),
                    ne.symbol());
                const int g = hasSlur ? lyrics.groupAt(n.startTicks) : 0;

                // 能否接上上一条减时线：同小节、首尾相接、减时线条数相同，
                // 且同属一拍（三连音组例外，它本来就跨拍）。
                // 还要同属一个一字多音组：一条减时线跨两个字会把连音线切断。
                // 一拍多长要按「当前小节的拍号」算：6/8 是八分音符一拍，
                // 沿用 4/4 的 480 tick 会把六拍全算成三拍，减时线乱连。
                long beatLen = (long)std::llround(
                    grid.sigForBar(bar).barLengthTicks(data.division) /
                    std::max(1, grid.beatsForBar(bar)));
                long curBeat = beatLen > 0
                    ? (n.startTicks - grid.barStart(bar)) / beatLen : 0;
                long endTick = n.startTicks + std::max<long>(n.durationTicks, 1);
                bool join = ds.beams > 0 && ds.beams == beamLevel && !beamBuf.empty() &&
                            bar == beamBar && n.startTicks == beamPrevEnd &&
                            grp[0]->trip == beamTrip && g == beamGroup &&
                            (beamTrip || curBeat == beamBeat);

                if (ds.beams > 0) {
                    if (!join) {
                        flushBeam();
                        beamLevel = ds.beams;
                        beamGroup = g;
                        beamBar = bar;
                        beamBeat = curBeat;
                        beamTrip = grp[0]->trip;
                    }
                    beamBuf.push_back(col);
                    beamPrevEnd = endTick;
                } else {
                    flushBeam();
                    emitWithGroup(col, g);
                }
            } else {
                flushBeam();          // 和弦自成一列，不与单音连线
                // 多声部：竖向叠写。按音高降序，高音在上、低音在下，
                // 与五线谱的视觉方向一致（反过来看会很不习惯）。
                std::vector<NoteEvent> sorted;
                for (const auto* h : grp) sorted.push_back(h->n);
                std::sort(sorted.begin(), sorted.end(),
                          [](const NoteEvent& a, const NoteEvent& b) { return a.pitch > b.pitch; });

                // 倚音（装饰音）：和弦前面的快速装饰音同样用小字上标呈现。
                const NoteExpression ne = expr.noteAt(n.startTicks, n.channel);
                std::string graceHtml;
                if (ne.hasGrace())
                    graceHtml = "<span class='grace'>" +
                                htmlEscape(pitchToJianpu(ne.gracePitch, keys.at(n.startTicks))) +
                                "</span>";

                std::vector<std::pair<std::string, std::pair<int, int>>> tones;
                for (const auto& t : sorted) {
                    std::string jp = pitchToJianpu(t.pitch, keys.at(t.startTicks));
                    int hi = 0, lo = 0;
                    std::string num;
                    for (char c : jp) {
                        if (c == '\'') hi++;
                        else if (c == ',') lo++;
                        else num += c;
                    }
                    tones.push_back({num, {hi, lo}});
                }
                emit(graceHtml + withArticulation(
                         withChord(chordColumn(tones, beats, ""), chordMarkFor(n.startTicks)),
                         ne.symbol()),
                     n.startTicks);
            }

            for (const auto* h : grp) prevEnd = std::max(prevEnd, h->n.startTicks + h->n.durationTicks);
            firstNote = false;
        }

        flushBeam();   // 收尾：把最后一条减时线画完再闭合小节
        flushSlur();   // 连音线同理，否则最后一组会整段消失
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
/* 段落名（MIDI 0x06 marker）与渐强渐弱：和力度同在标记行，用颜色区分 */
.section{font-size:11px;font-weight:700;color:#2f7a5a;line-height:1;
         border:1px solid #cbe3d6;border-radius:3px;padding:1px 4px;background:#f3faf6}
.wedge{font-size:12px;font-style:italic;color:#a36a2a;line-height:1}
/* 断音/重音/滑音浮在音符列右上角：不占宽度，也不压住上八度点 */
.artic{position:absolute;right:-8px;top:-3px;font-size:14px;line-height:1;
       font-weight:700;color:#a33;white-space:nowrap;pointer-events:none}
.tempo{font-size:11px;font-weight:600;color:#4a6fa5;line-height:1;
       border:1px solid #cdd8e8;border-radius:3px;padding:1px 4px;background:#f4f7fc}
/* 渐快 / 渐慢：与阶跃变速同为速度记号，但用棕色区分——它不是一条
   「从这个速度开始」的指令，而是一整段的走向，读者扫谱时该分得开 */
.tempo.ramp{color:#8a5a2b;border-color:#e4d3bb;background:#fdf8f0}
.meter{font-size:11px;font-weight:700;color:#8a5a2b;line-height:1;
       border:1px solid #e4d3bb;border-radius:3px;padding:1px 4px;background:#fdf8f0}
/* 倚音（装饰音）：小字上标，写在主音数字前面，不参与节奏计数 */
.grace{font-size:11px;color:#a36a2a;vertical-align:super;margin-right:1px}
/* 转调标记与拍号/速度同为「谱面事件」，但用紫红色区分，扫一眼就知道
   这里不是节奏变了、而是整段唱名换了。 */
.keychg{font-size:11px;font-weight:700;color:#7a3f7d;line-height:1;
        border:1px solid #e0cbe1;border-radius:3px;padding:1px 4px;background:#fbf5fb}
/* 复调轨按声部拆行时的小标签：同一条轨会连着出现好几行，靠它区分 */
.voiceTag{font-size:12px;font-weight:400;color:#6b7a8d;margin-left:2px}

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

/* 一字多音的连音线：每个字拖着唱的那几个音共一条弧线。
   谱面上方从下到上分三层：数字 / 连音线 / 和弦字母。存在连音线时把
   .notes 的上留白从 18px 抬到 36px，和弦浮标相应抬高，弧线落在中间
   那 12px 里；三者各占一层，弧线不会压到高八度点，也不会和和弦粘连。
   只给有连音线的那一轨加 hasslur，其余谱面行距保持原样。 */
.hasslur .notes{padding-top:36px}
.hasslur .chordx{bottom:calc(100% + 22px)}
.sgrp{position:relative;display:inline-flex;align-items:flex-start}
.sgrp::before{content:"";position:absolute;left:3px;right:3px;top:-13px;height:9px;
              border-top:1.5px solid #555;border-radius:50% 50% 0 0 / 100% 100% 0 0;
              pointer-events:none}

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
         << htmlEscape(keys.summary(grid))
         << " ｜ " << meterSummary(data)
         << " ｜ " << (data.keySig.valid ? "MIDI调号"
                       : (key.scaleLabel.empty() ? "自动猜测" : "五声宫音校正"))
         << (tempoSummary(data).empty() ? "" : " ｜ " + tempoSummary(data))
         << (partSummary(data).empty() ? "" : " ｜ " + partSummary(data))
         << (pickupLabel(data).empty() ? "" : " ｜ " + pickupLabel(data))
         << (truncationLabel(data).empty() ? "" : " ｜ " + truncationLabel(data))
         << (timeBaseLabel(data).empty() ? "" : " ｜ " + timeBaseLabel(data))
         << (formatLabel(data).empty() ? "" : " ｜ " + formatLabel(data))
         << (controllerLabel(data).empty() ? "" : " ｜ " + controllerLabel(data))
         << (bendRangeLabel(data).empty() ? "" : " ｜ " + bendRangeLabel(data))
         << (runningStatusLabel(data).empty() ? "" : " ｜ " + runningStatusLabel(data))
         << (portLabel(data).empty() ? "" : " ｜ " + portLabel(data))
         << (sysExLabel(data).empty() ? "" : " ｜ " + sysExLabel(data))
         << (bankLabel(data).empty() ? "" : " ｜ " + bankLabel(data))
         << (trackLabel(data).empty() ? "" : " ｜ " + trackLabel(data))
         << (data.drumNotesSkipped > 0 ? " ｜ 已跳过鼓轨" : "")
         << (lyricLabel.empty() ? "" : " ｜ " + lyricLabel)
         << (expr.summary().empty() ? "" : " ｜ " + expr.summary())
         << "</div>" << body.str()
         << "<footer class='footer'>由 midi2jianpu v" M2J_VERSION " 生成</footer>"
         << "</body></html>";
    return html.str();
}

} // namespace m2j

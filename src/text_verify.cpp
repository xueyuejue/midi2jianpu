// text_verify.cpp - 文本谱拍数守恒（判据见 text_verify.hpp 顶部的说明）
//
// 实现上刻意只做两件事：**按文本谱自己的记法把一小节印出来的拍数加起来**，
// 再**拿 BarGrid 里这一小节的拍号去对**。两边都不复用 writer 的中间量：
// 写法错、网格错，这一对就对不上。
#include "midi2jianpu/text_verify.hpp"

#include "midi2jianpu/notation.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace m2j {
namespace {

constexpr double kTol = 1e-6;

// ---- 文本谱 token 语法 ----
// 一个 token 由这几段拼成（顺序与 notation.cpp 的写出顺序一致）：
//   [#|b]   升降号
//   <core>  度数 0-7，或延长线 '-' 的重复
//   [',]*   八度记号（' 高八度、, 低八度）
//   <suf>   时值后缀（见 kSuf）
//   <art>   演奏法 ▼ · > ↗ ↘ tr（只出现在音头上，不影响拍数）
//
// 和弦用 '+' 把同拍的音串起来（`1,+3,+5,`），整格只算一次时值——后缀写在
// 最后一个音上，所以取最后一个分量。三连音写成 `3(5 5 5 )`，倚音写成
// `(3)5` 贴在主音前。小节线是 '|'，一小节一行。

struct Suf {
    const char* txt;
    double beats;
};

// 长后缀必须排在短后缀前面（"=." 先于 "="、"%%" 先于两边）。表里没有
// '-'：二分音符以上的延长线由 emitNoteToken 拆成独立的 `-` token，
// 所以 `-` 单独出现时就是一拍，带着后缀才是「不足一拍的续写」。
const Suf kSuf[] = {
    {"%%", 0.125},  // 三十二分
    {"=.", 0.375},  // 附点十六分
    {"_.", 0.75},   // 附点八分
    {"=", 0.25},    // 十六分
    {"_", 0.5},     // 八分
    {".", 1.5},     // 附点四分
    {"", 1.0},      // 四分
};

bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

// 演奏法记号：全是「贴在这个音上、不占拍」的字符，只要求能认出来并丢掉。
// ▼ · ↗ ↘ 是多字节 UTF-8，按字节前缀识别。
bool isArtTail(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 't' || c == 'r' || c == '>' || c == ',' || c == '\'') { i++; continue; }
        if (c == 0xE2 && i + 2 < s.size()) {          // ▼ · ↗ ↘
            const unsigned char a = (unsigned char)s[i + 1];
            const unsigned char b = (unsigned char)s[i + 2];
            const bool ok = (a == 0x96 && b == 0xBC)    // ▼
                         || (a == 0x86 && (b == 0x97 || b == 0x98));  // ↗ ↘
            if (ok) { i += 3; continue; }
        }
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xB7) {
            i += 2;                                    // ·（U+00B7）
            continue;
        }
        return false;
    }
    return true;
}

// 一个音（或一条 '--' 延长线）的拍数。认不出来返回 false。
// tripletUnit：三连音组里每个音的名义拍数——八分三连音 1/3 拍、四分三连音
// 2/3 拍。文本谱统一写成 `3(1 1 1 )`，两种单位在字形上分不出来，所以调用方
// 两种都算一遍，能对上哪一个就认哪一个。
bool componentBeats(const std::string& t, bool triplet, double tripletUnit,
                    double* out) {
    std::string s = t;
    if (s.empty()) return false;
    if (s[0] == '#' || s[0] == 'b') s.erase(0, 1);
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-') {
        while (i < s.size() && s[i] == '-') i++;
    } else if (s[0] >= '0' && s[0] <= '7') {
        i = 1;
    } else {
        return false;
    }
    const std::string core = s.substr(0, i);
    // 八度记号
    size_t j = i;
    while (j < s.size() && (s[j] == '\'' || s[j] == ',')) j++;
    // 时值后缀（长档优先，空后缀在表尾兜底）
    double sufBeats = 1.0;
    for (const auto& d : kSuf) {
        const size_t n = std::strlen(d.txt);
        if (n > 0 && s.compare(j, n, d.txt) == 0) {
            sufBeats = d.beats;
            j += n;
            break;
        }
        if (n == 0) sufBeats = d.beats;
    }
    // 余下只能是演奏法记号
    if (j < s.size() && !isArtTail(s.substr(j))) return false;

    const bool isHeld = (core[0] == '-');
    if (isHeld) {
        // `-` 单独出现是一拍；`-_.` 这种带后缀的是「不足一拍的续写」
        if (j == i) {
            *out = (double)core.size();
        } else {
            *out = sufBeats;
        }
        return true;
    }
    if (triplet) {
        *out = tripletUnit;
        return true;
    }
    *out = sufBeats;
    return true;
}

// 去掉 [..] 标记（和弦名、段落名、力度、拍号、转调、小节号都在里面）。
// 标记里不会有 ']'，所以找到就跳过即可。
std::string stripMarks(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '[') {
            const size_t e = s.find(']', i + 1);
            if (e == std::string::npos) break;
            i = e + 1;
            out.push_back(' ');
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (unsigned char)s[i] <= ' ') i++;
        const size_t b = i;
        while (i < s.size() && (unsigned char)s[i] > ' ') i++;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

// 一小节（'|' 之前的那一段）印出来的拍数。
// ok=false 表示这一行里有认不出的写法，本行放弃判定。
bool segmentBeats(const std::string& seg, double* beats, bool* any,
                  double tripletUnit = 1.0 / 3.0) {
    *beats = 0.0;
    *any = false;
    const std::vector<std::string> toks = splitWs(stripMarks(seg));
    bool triplet = false;
    for (std::string t : toks) {
        if (t == ")") { triplet = false; continue; }
        // 三连音前缀 3( 可能连着好几个（嵌套时 writer 不会产生，防御）
        while (t.size() >= 2 && t[0] == '3' && t[1] == '(') {
            triplet = true;
            t.erase(0, 2);
        }
        // 倚音前缀 (3) / (7,)：整颗不占拍，只是一个装饰
        while (!t.empty() && t[0] == '(') {
            const size_t e = t.find(')');
            if (e == std::string::npos) return false;
            t.erase(0, e + 1);
        }
        if (t.empty()) continue;
        // 和弦：整格只算一次时值，取最后一个分量的后缀
        const size_t plus = t.rfind('+');
        if (plus != std::string::npos) t = t.substr(plus + 1);
        double b = 0.0;
        if (!componentBeats(t, triplet, tripletUnit, &b)) return false;
        *beats += b;
        *any = true;
    }
    return true;
}

bool isTrackHeader(const std::string& l, long* noteCount = nullptr) {
    if (l.empty() || l[0] != '[') return false;
    const std::string key = " 音符)";
    // 取**最后**一个「 音符)」：轨名里可能自带这串字（`X (200 音符)`），
    // 取第一个会把名字里那个数字当成本声部的音数，进而算错这条谱行该有
    // 几小节、整行被判不判。
    const size_t p = l.rfind(key);
    if (p == std::string::npos || p == 0) return false;
    // 「 音符)」之后只允许两种：行尾，或「 · 和弦: N 个」。别的都是谱行
    // 内容——段落名印在小节首（`[合唱段 (8 音符)] - - - 5_  |`），把它当
    // 小标题会把整条谱行吞掉，核对的小节数从「有」变「0」，坏谱反而通过。
    const size_t after = p + key.size();
    if (after < l.size()) {
        const std::string chord = " · 和弦: ";
        if (l.compare(after, chord.size(), chord) != 0) return false;
        size_t q = after + chord.size();
        const size_t dig = q;
        while (q < l.size() && isDigitChar(l[q])) q++;
        const std::string suffix = " 个";
        if (q == dig || l.compare(q, suffix.size(), suffix) != 0 ||
            q + suffix.size() != l.size())
            return false;
    }
    size_t b = p;
    while (b > 0 && isDigitChar(l[b - 1])) b--;
    if (!(b < p && b > 0 && l[b - 1] == '(')) return false;
    // 小标题的「(」紧跟在方括号收尾之后：`[声部名] (N 音符)`。轨名里可以
    // 带括号，但小标题这一个括号一定落在 `] ` 后面。
    if (!(b >= 3 && l[b - 2] == ' ' && l[b - 3] == ']')) return false;
    if (noteCount) {
        long v = 0;
        bool any = false;
        for (size_t i = b; i < p; i++) {
            if (v > 100000000L) { v = 100000000L; break; }   // 防御：坏文本
            v = v * 10 + (l[i] - '0');
            any = true;
        }
        *noteCount = any ? v : -1;
    }
    return true;
}

std::string fmtBeats(double b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", b);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? std::string("0") : s;
}

std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    // 别把 UTF-8 序列劈成两半
    size_t cut = n;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + "…";
}

struct PrintedBar {
    double beats = 0.0;
    // 同一小节按「四分三连音（2/3 拍）」重算的拍数。`3(1 1 1 )` 这种写法里
    // 看不出每组是八分三连音还是四分三连音，两种都能与字形相容；判定时
    // 只要有一种能与拍号对上就算守恒，避免把正常的四分三连音报成少拍。
    double beatsAlt = 0.0;
    bool ok = false;      // 认得出这一节的每一个 token
    bool okAlt = false;   // 同上，按 2/3 拍的三连音单位
    // 这一小节里出现过 3(...)。三连音的音头本来就落在 1/3 拍网格上，
    // 「音头不在 1/8 拍网格」这条近似理由对它不成立——有它就别放行，否则
    // 三连音少拍这类真缺陷会被记成「记谱极限」而静默通过。
    bool triplet = false;
    std::string text;     // 原文（给报告用）
};

struct PrintedTrack {
    long noteCount = -1;          // 小标题里写的「N 音符」
    std::vector<PrintedBar> bars;
};

}  // namespace

TextVerifyReport verifyTextScore(const std::string& text, const MidiData& data,
                                 bool allowApprox) {
    TextVerifyReport r;
    const BarGrid grid = barGridOf(data);
    const long division = data.division > 0 ? data.division : 480;

    // 1) 切出每条谱行、每行的小节
    std::vector<PrintedTrack> tracks;
    bool sawHeader = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        long ncount = -1;
        if (isTrackHeader(line, &ncount)) {
            PrintedTrack pt;
            pt.noteCount = ncount;
            tracks.push_back(pt);
            sawHeader = true;
            continue;
        }
        if (!sawHeader || tracks.empty()) continue;
        if (line.find('|') == std::string::npos) continue;

        // 一行里可能有多条小节线；空段（行末那个 ' |' 的尾巴）不算小节
        size_t s = 0;
        while (true) {
            const size_t bar = line.find('|', s);
            if (bar == std::string::npos) break;
            const std::string seg = line.substr(s, bar - s);
            s = bar + 1;
            PrintedBar pb;
            bool any = false, anyAlt = false;
            const bool parsed = segmentBeats(seg, &pb.beats, &any);
            const bool parsedAlt = segmentBeats(seg, &pb.beatsAlt, &anyAlt,
                                                2.0 / 3.0);
            pb.ok = parsed && any;
            pb.okAlt = parsedAlt && anyAlt;
            pb.triplet = seg.find("3(") != std::string::npos;
            pb.text = clip(stripMarks(seg), 60);
            // 去掉首尾空白，报告里好看一点
            size_t a = 0, b = pb.text.size();
            while (a < b && (unsigned char)pb.text[a] <= ' ') a++;
            while (b > a && (unsigned char)pb.text[b - 1] <= ' ') b--;
            pb.text = pb.text.substr(a, b - a);
            if (pb.text.empty() && !pb.ok) continue;    // 纯空白段
            tracks.back().bars.push_back(pb);
        }
    }

    // 2) 逐行逐小节对
    //
    // 先把「全曲有多少小节」定下来：文本谱的每一行都从第 0 小节起、一小节一行，
    // 所以只有小节数与全曲一致的谱行才能确定「第 i 行就是第 i 小节」。
    // 不足这一条的行整行跳过，不去猜它的起点——`edge/short_meter.mid` 就是
    // 反例：开头 1/16 那一小节整节空白且短于休止门槛（div/2），谱面按设计
    // 根本不印它，那一行其实从第 1 小节开始。猜错的代价是报一个假问题，
    // 比少判一行严重得多。
    long maxEnd = 0;
    for (const auto& n : data.notes)
        maxEnd = std::max(maxEnd, n.startTicks + std::max<long>(n.durationTicks, 1));
    // 末音正好落在小节线上时，barIndex(maxEnd) 已经是下一小节，但书写器只在
    // 它自己那一小节里铺 token（续写循环 t < noteEnd 不成立），所以谱面的最后
    // 一行是 maxEnd-1 所在的小节。
    const long totalBars = grid.barIndex(maxEnd > 0 ? maxEnd - 1 : 0) + 1;

    // 把谱行与轨道对上：谱行小标题写着「(N 音符)」，单声部轨的 N 就是该轨的
    // 音数。对上唯一一条轨，就能拿到这条轨自己的末音位置，从而知道这一行
    // 应该有几小节。对不上（同音数的轨不止一条、或这条轨被拆成了多声部）
    // 就退回「全曲小节数」，再对不上就整行不判。
    struct TrackSpan {
        long count = 0;
        long end = 0;
    };
    std::map<int, TrackSpan> spans;
    for (const auto& n : data.notes) {
        TrackSpan& s = spans[n.track];
        s.count++;
        s.end = std::max(s.end, n.startTicks + std::max<long>(n.durationTicks, 1));
    }

    for (size_t ti = 0; ti < tracks.size(); ti++) {
        const auto& bars = tracks[ti].bars;
        if (bars.empty()) continue;
        r.lines++;
        long want = totalBars;
        if (tracks[ti].noteCount >= 0) {
            long hitEnd = -1;
            int hit = 0;
            for (const auto& kv : spans) {
                if (kv.second.count == tracks[ti].noteCount) {
                    hit++;
                    hitEnd = kv.second.end;
                }
            }
            if (hit == 1)
                want = grid.barIndex(hitEnd > 0 ? hitEnd - 1 : 0) + 1;
        }
        if ((long)bars.size() != want) {
            r.skipped += (int)bars.size();      // 起点无法确定，整行不判
            continue;
        }
        for (size_t i = 0; i < bars.size(); i++) {
            const PrintedBar& pb = bars[i];
            const bool last = (i + 1 == bars.size());
            long barNo = (long)i;
            if (!pb.ok || last) { r.skipped++; continue; }
            // 网格里这一小节本身就不满（弱起 / 0x58 落在小节中间被截断 /
            // 全曲末节），按设计就短，不判。
            const long fullLen =
                (long)std::llround(grid.sigForBar(barNo).barLengthTicks(division));
            if (fullLen <= 0 || grid.barSpan(barNo) != fullLen) {
                r.skipped++;
                continue;
            }
            const TimeSignature sig = grid.sigForBar(barNo);
            const double meterBeats =
                (double)sig.numerator * 4.0 / (double)(1 << sig.denomPow);
            r.bars++;
            if (std::fabs(pb.beats - meterBeats) <= kTol) continue;
            // 三连音单位在字形上不可见：换 2/3 拍再对一次（见 PrintedBar）。
            if (pb.okAlt && std::fabs(pb.beatsAlt - meterBeats) <= kTol)
                continue;
            TextBarIssue is;
            is.line = (int)ti + 1;
            is.bar = barNo;
            is.printed = pb.beats;
            is.want = meterBeats;
            is.text = pb.text;
            // 这一小节的差是不是「记谱本身的极限」：音头 / 时值不在 1/8 拍
            // 网格上，简谱记到 1/8 拍为止，余下的洞印不出来。上界由模型算：
            // 每一个落不到网格上的音头、每一个拆不干净的时值，各允许一个
            // 1/8 拍的洞；差得比这个上界还多，说明另有原因（真缺陷）。
            const long unit = std::max<long>(1, division / 8);
            long allowance = 0;
            if (!pb.triplet) {
                const long bs = grid.barStart(barNo), be = grid.barEnd(barNo);
                // 本小节的音头表：用来算「两个音头之间那段补不出来」的空档。
                // 量化会把不在网格上的时值吸附到最近的档，于是原本首尾相接的
                // 两个音之间露出几十 tick——比休止门槛短，印不出休止符，
                // 这段就是一个必然的洞。
                std::vector<std::pair<long, long>> ons;   // (音头, 该音头最远结束)
                for (const auto& n : data.notes) {
                    if (n.startTicks < bs || n.startTicks >= be) continue;
                    const long end = n.startTicks + std::max<long>(n.durationTicks, 1);
                    if (!ons.empty() && ons.back().first == n.startTicks) {
                        if (end > ons.back().second) ons.back().second = end;
                        continue;
                    }
                    ons.push_back({n.startTicks, end});
                }
                std::stable_sort(ons.begin(), ons.end());
                for (size_t k = 0; k < ons.size(); k++) {
                    if ((ons[k].first - bs) % unit != 0) allowance += unit;
                    const long dur = ons[k].second - ons[k].first;
                    if (!canNotateExactly(dur, division)) allowance += unit;
                    if (k + 1 >= ons.size()) continue;
                    const long gap = ons[k + 1].first - ons[k].second;
                    // 门槛与 restGateTicks 同源：短到印不出休止符的那段，
                    // 简谱只能丢掉（文本谱与 HTML 都不补）。
                    if (gap > 0 && gap < std::max<long>(1, division / 2))
                        allowance += gap;
                }
            }
            const double deficit = meterBeats - pb.beats;
            if (allowApprox) r.approx.push_back(is);
            else if (allowance > 0 && deficit > -kTol &&
                     deficit * (double)division <= (double)allowance + kTol)
                r.approxGrid.push_back(is);
            else r.issues.push_back(is);
        }
    }
    return r;
}

std::string formatTextVerifyReport(const TextVerifyReport& r,
                                  const std::string& label) {
    const std::string pfx = label.empty() ? std::string() : (label + ": ");
    std::string out;
    if (r.ok()) {
        out = pfx + "文本谱核对 " + std::to_string(r.lines) + " 行 / "
            + std::to_string(r.bars) + " 小节";
        if (r.bars == 0) {
            // 一个小节都没真判（单小节文件整篇是「末小节」，或全部落在
            // 弱起/截短口径里）。这时说「守恒」是把没查说成查过——自检的
            // 可信度全靠这句话，宁可直说没核。
            out += "：没有可比对的小节，本次未做拍数核对";
        } else if (r.approx.empty() && r.approxGrid.empty()) {
            out += "，每小节拍数守恒";
        } else {
            // 有近似小节时不能说「每小节拍数守恒」——那就把「没精确核到」
            // 说成「核过了」。核过的与只能近似的必须分开说。
            const int loose = (int)(r.approx.size() + r.approxGrid.size());
            const int exactBars = r.bars - loose;
            out += "，" + std::to_string(exactBars > 0 ? exactBars : 0)
                + " 小节拍数精确守恒，" + std::to_string(loose)
                + " 小节只能近似记谱（见提示）";
        }
        if (r.skipped > 0)
            out += "（另有 " + std::to_string(r.skipped)
                 + " 小节按设计不判：弱起、变拍号截短、末小节）";
    } else {
        out = pfx + "文本谱拍数不守恒 " + std::to_string(r.issues.size())
            + " 处：";
        for (const auto& it : r.issues) {
            out += "\n  [FAIL] 第 " + std::to_string(it.line) + " 行谱 第 "
                 + std::to_string(it.bar + 1) + " 小节：印出 "
                 + fmtBeats(it.printed) + " 拍 / 拍号应 " + fmtBeats(it.want)
                 + " 拍：`" + it.text + "`";
        }
    }
    if (!r.approx.empty()) {
        out += "\n" + pfx + "提示：" + std::to_string(r.approx.size())
             + " 处小节拍数不满——--no-quantize 保留 MIDI 原始时值，记谱只能"
               "吸附到最近档，属近似记谱；要严格对齐请去掉该开关";
    }
    if (!r.approxGrid.empty()) {
        out += "\n" + pfx + "提示：" + std::to_string(r.approxGrid.size())
             + " 处小节拍数不满——MIDI 里的音头 / 时值不在 1/8 拍网格上"
               "（如 1780 tick 这种位置），简谱最小只记到 1/8 拍，余下不足"
               " 1/8 拍的部分印不出来。这是记谱精度本身的极限，不是缺陷；"
               "要对得整整齐齐，请先在编曲软件里把音符量化到 1/8 拍网格";
    }
    return out;
}

}  // namespace m2j

// xml_verify.cpp - 输出自检的实现：一个刻意「只读、不共享 writer 状态」的扫描器
//
// 为什么不复用 renderMusicXml() 内部的那套 Bookkeeping：那是**写**的时候顺手
// 记下的，写错了它自己也跟着错（v1.146.0 的尾节缺失就是这么漏过去的——循环
// 按片段推进，压根没有「全曲末节」这个概念）。自检必须从**已经生成的那份
// 文本**重新数一遍，用 MusicXML 的语义（游标 = note 前进 / backup 后退）而不是
// 生成时的中间量。这样 writer 的推算错位才照得出来。
//
// 代价是要自己扫字符串。好在输出是本程序自己写的，格式规整（一行一个元素、
// 无属性换行、无自闭合缩进变体），不需要通用的 XML 解析器；仓库也一直坚持
// 零第三方依赖。
#include "midi2jianpu/xml_verify.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace m2j {
namespace {

constexpr size_t kNpos = std::string::npos;

size_t litLen(const char* lit) { return std::char_traits<char>::length(lit); }

// 在 [from, to) 里找 lit。区间限定很重要：一个 part 的检查绝不能读到下一个
// part 的字节，否则「本 part 尾节是不是休止」会拿别人的音符来判定。
size_t findIn(const std::string& s, const char* lit, size_t from, size_t to) {
    if (from >= to || to > s.size()) return kNpos;
    const size_t p = s.find(lit, from);
    if (p == kNpos || p + litLen(lit) > to) return kNpos;
    return p;
}

// <duration>480</duration> 这类「标签包一个非负整数」的取值；没有就返回 -1。
long tagLong(const std::string& s, const char* open, const char* close,
             size_t from, size_t to) {
    const size_t a = findIn(s, open, from, to);
    if (a == kNpos) return -1;
    const size_t v = a + litLen(open);
    const size_t b = s.find(close, v);
    if (b == kNpos || b > to) return -1;
    long n = 0;
    bool any = false;
    for (size_t i = v; i < b; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') break;
        // 防御：畸形数字不许溢出成负数，溢出会让「差多少」这类报错变成噪音。
        if (n > 100000000L) return -1;
        n = n * 10 + (c - '0');
        any = true;
    }
    return any ? n : -1;
}

struct Measure {
    std::string number;
    size_t bodyBegin = 0;
    size_t bodyEnd = 0;
    bool implicit = false;
};

struct Part {
    std::string id;
    std::vector<Measure> measures;
};

// 按文档顺序切出各 <part> 及其 <measure>。两者都不嵌套，非贪婪匹配到自己
// 的收尾即可；`</part>` 不会被 `</part-list>` 误命中（多一个 '-'）。
std::vector<Part> splitParts(const std::string& xml) {
    std::vector<Part> parts;
    size_t pos = 0;
    for (;;) {
        const size_t a = xml.find("<part id=\"", pos);
        if (a == kNpos) break;
        const size_t vb = a + 10;                     // strlen("<part id=\"")
        const size_t ve = xml.find('"', vb);
        if (ve == kNpos) break;
        const size_t headEnd = xml.find('>', ve);
        if (headEnd == kNpos) break;
        const size_t bodyEnd = xml.find("</part>", headEnd);
        if (bodyEnd == kNpos) break;

        Part p;
        p.id = xml.substr(vb, ve - vb);

        size_t q = headEnd + 1;
        for (;;) {
            const size_t m = xml.find("<measure number=\"", q);
            if (m == kNpos || m >= bodyEnd) break;
            const size_t nb = m + 17;                 // strlen("<measure number=\"")
            const size_t ne = xml.find('"', nb);
            if (ne == kNpos || ne > bodyEnd) break;
            const size_t hEnd = xml.find('>', ne);
            if (hEnd == kNpos || hEnd > bodyEnd) break;
            const size_t mEnd = xml.find("</measure>", hEnd);
            if (mEnd == kNpos || mEnd > bodyEnd) break;

            Measure ms;
            ms.number = xml.substr(nb, ne - nb);
            ms.bodyBegin = hEnd + 1;
            ms.bodyEnd = mEnd;
            ms.implicit = xml.substr(ne, hEnd - ne).find("implicit=\"yes\"")
                          != kNpos;
            p.measures.push_back(ms);
            q = mEnd + 10;                            // strlen("</measure>")
        }
        parts.push_back(std::move(p));
        pos = bodyEnd + 7;                            // strlen("</part>")
    }
    return parts;
}

void add(std::vector<VerifyIssue>& v, std::string where, std::string what) {
    v.push_back(VerifyIssue{std::move(where), std::move(what)});
}

std::string num(long long n) { return std::to_string(n); }

// 一小节的时值守恒：游标从小节起点出发，结束时必须正好落在小节末尾。
//
// 这条口径与 tests/check_bar_beats.py / check_xml_structure.py 一致，但实现
// 完全独立——那边数文本谱的拍、这边按 MusicXML 的游标语义数 duration。
bool isWholeRest(const std::string& xml, const Measure& ms) {
    return findIn(xml, "<rest measure=\"yes\"/>", ms.bodyBegin, ms.bodyEnd)
           != kNpos;
}

bool hasTone(const std::string& xml, const Measure& ms) {
    return findIn(xml, "<pitch>", ms.bodyBegin, ms.bodyEnd) != kNpos
        || findIn(xml, "<unpitched>", ms.bodyBegin, ms.bodyEnd) != kNpos;
}

// 纯数字串 -> long；空串、含非数字、位数离谱都算解析失败。
bool parseDigits(const std::string& s, long& out) {
    if (s.empty()) return false;
    long n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        if (n > 100000000L) return false;
        n = n * 10 + (c - '0');
    }
    out = n;
    return true;
}

void auditMeasure(const std::string& xml, const std::string& pid,
                  const Measure& ms, long& div, long& beats, long& beatType,
                  std::vector<VerifyIssue>& issues) {
    // <attributes> 只在第一小节和「真的有变化」时才写，divisions 与拍号必须
    // 沿用上一小节（MusicXML 的继承语义），不能每小节重置成默认值。
    const long d = tagLong(xml, "<divisions>", "</divisions>", ms.bodyBegin, ms.bodyEnd);
    if (d > 0) div = d;
    const long b = tagLong(xml, "<beats>", "</beats>", ms.bodyBegin, ms.bodyEnd);
    if (b > 0) beats = b;
    const long bt = tagLong(xml, "<beat-type>", "</beat-type>", ms.bodyBegin, ms.bodyEnd);
    if (bt > 0) beatType = bt;

    long long cursor = 0;
    long long lowest = 0;
    size_t i = ms.bodyBegin;
    while (i < ms.bodyEnd) {
        const size_t nNote = findIn(xml, "<note", i, ms.bodyEnd);
        const size_t nBack = findIn(xml, "<backup", i, ms.bodyEnd);
        const size_t nFwd  = findIn(xml, "<forward", i, ms.bodyEnd);
        size_t k = kNpos;
        if (nNote != kNpos) k = nNote;
        if (nBack != kNpos && (k == kNpos || nBack < k)) k = nBack;
        if (nFwd  != kNpos && (k == kNpos || nFwd  < k)) k = nFwd;
        if (k == kNpos) break;

        if (k == nBack) {
            const size_t e = xml.find("</backup>", k);
            const size_t end = (e == kNpos || e > ms.bodyEnd) ? ms.bodyEnd : e;
            const long len = tagLong(xml, "<duration>", "</duration>", k, end);
            cursor -= (len > 0 ? len : 0);
            if (cursor < lowest) lowest = cursor;
            i = (end == ms.bodyEnd) ? ms.bodyEnd : end + 9;   // strlen("</backup>")
        } else if (k == nFwd) {
            const size_t e = xml.find("</forward>", k);
            const size_t end = (e == kNpos || e > ms.bodyEnd) ? ms.bodyEnd : e;
            const long len = tagLong(xml, "<duration>", "</duration>", k, end);
            if (len > 0) cursor += len;
            i = (end == ms.bodyEnd) ? ms.bodyEnd : end + 10;  // strlen("</forward>")
        } else {
            const size_t e = xml.find("</note>", k);
            const size_t end = (e == kNpos || e > ms.bodyEnd) ? ms.bodyEnd : e;
            // 和弦成员与前面的音同拍发声，不再推进游标；<grace/> 没有
            // <duration>，按 MusicXML 语义也不占时值。
            const bool chord = findIn(xml, "<chord/>", k, end) != kNpos;
            if (!chord) {
                const long len = tagLong(xml, "<duration>", "</duration>", k, end);
                if (len > 0) cursor += len;
            }
            i = (end == ms.bodyEnd) ? ms.bodyEnd : end + 7;    // strlen("</note>")
        }
    }

    const std::string where = pid + " 第 " + ms.number + " 小节";
    if (lowest < 0)
        add(issues, where,
            "<backup> 把游标退到了小节线之前（最低 " + num(lowest)
            + " tick）：多退一次会让下一条声部从负时间开始排");

    // 弱起 / 被 0x58 截短的短小节按设计就短，用 implicit="yes" 声明，
    // 它的时值**不该**等于拍号——这正是 implicit 存在的意义。
    if (ms.implicit) return;
    if (div <= 0 || beats <= 0 || beatType <= 0) return;   // 没见过拍号，无从判定
    const long long perBeat = 4LL * div;
    if (perBeat % beatType != 0) return;   // 一小节不是整 tick，断言不成立
    const long long want = (long long)beats * (perBeat / beatType);
    if (cursor != want)
        add(issues, where,
            "时值合计 " + num(cursor) + " tick，拍号 " + num(beats) + "/"
            + num(beatType) + " 要求 " + num(want) + " tick（差 "
            + num(cursor - want) + "）：导入器会按拍号补一串休止把小节撑满");
}

}  // namespace

VerifyReport verifyOutputs(const std::string& text, const std::string& html,
                           const std::string& xml) {
    VerifyReport r;

    // ---- 1. 编码：三端都不许出现 U+FFFD ----
    // 出现它就说明某个环节「猜错了字符集还硬转」，用户看到的是一串问号。
    // 这是 tests/check_utf8.py 守的那条口径；放到产品里，用户自己的 MIDI
    // 也能当场查出来。
    struct { const char* label; const std::string* body; } outs[] = {
        {"文本谱", &text}, {"HTML 谱面", &html}, {"MusicXML", &xml}
    };
    for (const auto& o : outs) {
        if (o.body->empty()) continue;   // 本次没生成这一端
        if (o.body->find("\xEF\xBF\xBD") != kNpos)
            add(r.issues, o.label,
                "出现了 U+FFFD 替换字符：转换链路里有一步按错误的字符集硬转，"
                "原文变成了问号");
    }

    // ---- 2. MusicXML 结构 ----
    if (xml.empty()) return r;
    const std::vector<Part> parts = splitParts(xml);
    if (parts.empty()) {
        add(r.issues, "MusicXML", "没有任何 <part>：导入器会得到一张空谱");
        return r;
    }
    r.parts = (int)parts.size();
    r.measures = (int)parts.front().measures.size();

    // 2a) part-list 与正文必须一一对应。声明了却没有 <part>，导入后少一条
    //     谱表；有 <part> 却没声明，多数据入器直接把它丢掉。
    std::vector<std::string> declared;
    {
        const size_t listEnd = xml.find("</part-list>");
        size_t p = 0;
        for (;;) {
            const size_t a = xml.find("<score-part id=\"", p);
            if (a == kNpos || (listEnd != kNpos && a > listEnd)) break;
            const size_t vb = a + 16;   // strlen("<score-part id=\"")
            const size_t ve = xml.find('"', vb);
            if (ve == kNpos) break;
            declared.push_back(xml.substr(vb, ve - vb));
            p = ve + 1;
        }
    }
    for (const auto& p : parts) {
        if (std::find(declared.begin(), declared.end(), p.id) == declared.end())
            add(r.issues, p.id,
                "<part> 没有在 part-list 里声明：多数导入器会直接忽略这条谱表");
    }
    for (const auto& id : declared) {
        const bool present = std::any_of(
            parts.begin(), parts.end(),
            [&](const Part& p) { return p.id == id; });
        if (!present)
            add(r.issues, "part-list",
                "声明了 " + id + "，正文却没有对应的 <part>：导入后少一条谱表");
    }

    // 2b) 各 part 的小节数与小节号序列必须完全相同——score-partwise 是矩形谱，
    //     小节靠「在自己 part 里的第几个」对齐，长短不一就整条谱表错位。
    const Part& base = parts.front();
    for (size_t i = 1; i < parts.size(); ++i) {
        const Part& p = parts[i];
        if (p.measures.size() != base.measures.size()) {
            add(r.issues, p.id,
                "小节数与 " + base.id + " 不同（" + num((long long)p.measures.size())
                + " 节 vs " + num((long long)base.measures.size())
                + " 节）：矩形谱要求各 part 等长，否则这条谱表末尾会与其它声部错位");
            continue;
        }
        for (size_t k = 0; k < p.measures.size(); ++k) {
            if (p.measures[k].number != base.measures[k].number) {
                add(r.issues, p.id,
                    "第 " + num((long long)k + 1) + " 个位置的小节号是 "
                    + p.measures[k].number + "，" + base.id + " 却是 "
                    + base.measures[k].number + "：同一位置的小节号必须一致");
                break;
            }
        }
    }

    // 2c) 逐个 part 查：小节号连续、尾节是整小节休止、每小节时值守恒。
    for (const auto& p : parts) {
        for (size_t k = 1; k < p.measures.size(); ++k) {
            const std::string& prev = p.measures[k - 1].number;
            const std::string& cur = p.measures[k].number;
            // 自己数而不是 std::stol：数字离谱时 stol 会抛 out_of_range，
            // 自检工具不该因为一份坏文件把自己弄崩。
            long a = 0, b = 0;
            if (parseDigits(prev, a) && parseDigits(cur, b) && b != a + 1)
                add(r.issues, p.id,
                    "小节号跳变 " + prev + " -> " + cur
                    + "：中间的小节没有占位，导入器会把它当成同一节的延续");
        }

        // 尾部连续「无实音」的小节必须是整小节休止。写成普通休止（没有
        // measure="yes"）时导入器会按拍号去凑，谱面凭空多出一串休止符。
        size_t tail = 0;
        for (size_t k = p.measures.size(); k-- > 0;) {
            if (hasTone(xml, p.measures[k])) break;
            ++tail;
        }
        for (size_t k = p.measures.size() - tail; k < p.measures.size(); ++k) {
            if (!isWholeRest(xml, p.measures[k]))
                add(r.issues, p.id + " 第 " + p.measures[k].number + " 小节",
                    "尾节不是整小节休止（缺 <rest measure=\"yes\"/>）："
                    "导入器会按拍号补休止符，谱面比简谱多出几拍");
        }

        long div = 0, beats = 0, beatType = 0;
        for (const auto& ms : p.measures) {
            if (ms.implicit) ++r.implicitMeasures;
            auditMeasure(xml, p.id, ms, div, beats, beatType, r.issues);
        }
    }

    return r;
}

std::string formatVerifyReport(const VerifyReport& r, const std::string& label) {
    const std::string pfx = label.empty() ? std::string() : (label + ": ");
    if (r.ok()) {
        std::string out = pfx + "输出自检通过：" + num(r.parts) + " 个 part × "
                        + num(r.measures) + " 小节";
        if (r.implicitMeasures > 0)
            out += "（含 " + num(r.implicitMeasures) + " 个弱起/截短小节）";
        out += "，各 part 等长、时值守恒、小节号连续、尾节均为整小节休止";
        return out;
    }
    std::string out = pfx + "输出自检发现 " + num((long long)r.issues.size())
                    + " 处问题：";
    for (const auto& it : r.issues)
        out += "\n  [FAIL] "
             + (it.where.empty() ? std::string() : (it.where + "：")) + it.what;
    return out;
}

}  // namespace m2j

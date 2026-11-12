// text_encoding.hpp - 把 MIDI 文本 meta 的裸字节归一化成合法 UTF-8
//
// SMF 的文本 meta 事件（0x01/0x02/0x03/0x04/0x05/0x06 等）在标准里只是一串
// 裸字节，没有编码声明。现实里除了 UTF-8，还有大批老中文 MIDI 用 GBK(CP936)、
// 老日文 MIDI 用 Shift-JIS(CP932)、欧美 MIDI 用 Latin-1/Windows-1252。工具把
// 这些字节原样印到文本谱 / HTML / MusicXML，前两者显示成方块，而 MusicXML 是
// XML —— 解析器在第一个非法字节就报错，整份谱在 MuseScore 里根本打不开。
//
// 这里的策略是「宽进严出」：
//   1. 严格 UTF-8 校验通过 -> 原样返回（既有素材逐字节零回归）；
//   2. 否则分别按 GB2312 和 Shift-JIS 试解，按字符分布打分；
//   3. 高分且为正者胜出；两者都不可信 -> CP1252/Latin-1 逐字节折叠。
// 任何分支都只产生合法 UTF-8，不产生 U+FFFD（那等于把问题藏起来）。
//
// 映射表由 tools/gen_legacy_tables.py 生成，纯数据、无平台 API、无第三方库，
// MSVC 与 Emscripten 三端结果完全一致。两张 GBK 表覆盖完整 GBK：
//   kGb2312Hex —— GBK/GB2312 常用区（lead 0xA1..0xFE, trail 0xA1..0xFE, 94x94）；
//   kGbkExtHex  —— GBK 扩展区（lead 0x81..0xA0 全 trail / lead 0xA1..0xFE 的
//                  trail 0x40..0x7E、0x80..0xA0），镕/喆/堃/淼/鑫 这类生僻字都在这里。
// 任何 GBK 字节对都尽量解出原字；实在解不出的才落到 CP1252 折叠分支。
#ifndef M2J_TEXT_ENCODING_HPP
#define M2J_TEXT_ENCODING_HPP

#include <cstdint>
#include <string>

#include "legacy_tables.inc"

namespace m2j {
namespace textenc {

enum class Encoding { Utf8, Gb2312, ShiftJis, Fold };

struct Candidate {
    std::string text;
    int score = 0;
};

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back((char)cp);
    } else if (cp < 0x800u) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

// Windows-1252 的 0x80..0x9F 是可读标点，Latin-1 里却是不可见控制字符。
// 未映射字节一律走这里折叠：既保真，又绝不会产出 U+FFFD 或非法字节。
inline uint32_t foldByte(uint8_t b) {
    static const uint16_t kCp1252[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b >= 0x80u && b <= 0x9Fu) return kCp1252[b - 0x80u];
    return b;
}

inline uint16_t readCell4(const char* p) {
    auto nib = [](char c) -> uint16_t {
        return (uint16_t)((c >= '0' && c <= '9') ? (c - '0') : (c - 'A' + 10));
    };
    return (uint16_t)((nib(p[0]) << 12) | (nib(p[1]) << 8) |
                      (nib(p[2]) << 4) | nib(p[3]));
}

inline uint16_t cellAt(const char* hex, int row, int col) {
    return readCell4(hex + (((row - 1) * 94) + (col - 1)) * 4);
}

// GBK 扩展区的扁平偏移：lead 0x81..0xA0 共 32 行 x 190 列在前，
// 其后接 lead 0xA1..0xFE 共 94 行 x 96 列。trail 在 0x40..0x7E（63 个）之后接
// 0x80 起的合法段。返回 -1 表示 (lead, trail) 不是扩展区码位。
inline int gbkExtOffset(uint8_t lead, uint8_t trail) {
    if (lead >= 0x81u && lead <= 0xA0u) {
        const int rs = (int)(lead - 0x81u) * 190;
        int ti;
        if (trail >= 0x40u && trail <= 0x7Eu)      ti = (int)trail - 0x40;
        else if (trail >= 0x80u && trail <= 0xFEu) ti = 63 + ((int)trail - 0x80);
        else return -1;
        return rs + ti;
    }
    if (lead >= 0xA1u && lead <= 0xFEu) {
        const int rs = 32 * 190 + (int)(lead - 0xA1u) * 96;
        int ti;
        if (trail >= 0x40u && trail <= 0x7Eu)      ti = (int)trail - 0x40;
        else if (trail >= 0x80u && trail <= 0xA0u) ti = 63 + ((int)trail - 0x80);
        else return -1;
        return rs + ti;
    }
    return -1;
}

// 严格 UTF-8 前缀长度：拒绝过长编码、代理区、>U+10FFFF、非法续字节。
// 与 s.size() 相等即整串合法。
inline size_t utf8Prefix(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = (uint8_t)s[i];
        if (c < 0x80u) { ++i; continue; }
        int need;
        uint32_t cp, minCp;
        if (c >= 0xC2u && c <= 0xDFu)      { need = 1; cp = c & 0x1Fu; minCp = 0x80u; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; cp = c & 0x0Fu; minCp = 0x800u; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; cp = c & 0x07u; minCp = 0x10000u; }
        else return i;
        if (i + (size_t)need >= s.size()) return i;   // 末尾残缺序列
        for (int k = 1; k <= need; ++k) {
            const uint8_t d = (uint8_t)s[i + (size_t)k];
            if ((d & 0xC0u) != 0x80u) return i;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return i;
        i += (size_t)need + 1;
    }
    return i;
}

inline bool isValidUtf8(const std::string& s) {
    return utf8Prefix(s) == s.size();
}

// 打分用的字符类别：汉字与假名是正向证据；半角片假名是「把 GBK 当成
// Shift-JIS 解」的典型指纹，重罚；未定义码位也重罚。
inline int scoreCp(uint32_t cp) {
    if (cp >= 0x3400u && cp <= 0x9FFFu) return 2;
    if (cp >= 0x3040u && cp <= 0x30FFu) return 4;
    if ((cp >= 0x3000u && cp <= 0x303Fu) || (cp >= 0xFF01u && cp <= 0xFF60u)) return 1;
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return -8;
    return -3;
}

inline void addUnmapped(Candidate& c, const std::string& s, size_t at, size_t count) {
    for (size_t k = 0; k < count && at + k < s.size(); ++k)
        appendUtf8(c.text, foldByte((uint8_t)s[at + k]));
    c.score -= 6;
}

inline Candidate decodeGb2312(const std::string& s) {
    Candidate c;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t b = (uint8_t)s[i];
        if (b < 0x80u) {
            c.text.push_back((char)b);
            if (b < 0x20u || b == 0x7Fu) --c.score;   // 只有控制字符算噪声
            ++i;
            continue;
        }
        // GBK 双字节：lead 0x81..0xFE、trail 0x40..0xFE（不含 0x7F）。
        // 常用区走 kGb2312Hex；落在扩展区（lead 0x81..0xA0 或 trail 不在
        // 0xA1..0xFE）的查 kGbkExtHex；都查不到才整对折叠。
        const uint8_t t = (i + 1 < s.size()) ? (uint8_t)s[i + 1] : 0u;
        const bool hasTrail = (i + 1 < s.size()) &&
                              (t >= 0x40u) && (t != 0x7Fu);
        const size_t used = hasTrail ? 2 : 1;
        uint32_t cp = 0;
        if (hasTrail) {
            if (b >= 0xA1u && b <= 0xFEu && t >= 0xA1u && t <= 0xFEu) {
                cp = cellAt(kGb2312Hex, (int)b - 0xA0, (int)t - 0xA0);
            } else {
                const int off = gbkExtOffset(b, t);
                if (off >= 0) cp = readCell4(kGbkExtHex + off * 4);
            }
        }
        if (cp) {
            appendUtf8(c.text, cp);
            c.score += scoreCp(cp);
        } else {
            addUnmapped(c, s, i, used);
        }
        i += used;
    }
    return c;
}

inline bool sjisToJis(uint8_t s1, uint8_t s2, int& row, int& col) {
    if (s1 >= 0x81u && s1 <= 0x9Fu) row = (int)(s1 - 0x81u) * 2 + 1;
    else if (s1 >= 0xE0u && s1 <= 0xFCu) row = (int)(s1 - 0xE0u) * 2 + 63;
    else return false;
    if (s2 < 0x9Fu) col = (int)s2 - (s2 < 0x80u ? 0x40 : 0x41) + 1;
    else if (s2 <= 0xFCu) { ++row; col = (int)(s2 - 0x9Fu) + 1; }
    else return false;
    return row >= 1 && row <= 94 && col >= 1 && col <= 94;
}

inline Candidate decodeShiftJis(const std::string& s) {
    Candidate c;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t b = (uint8_t)s[i];
        if (b < 0x80u) {
            c.text.push_back((char)b);
            if (b < 0x20u || b == 0x7Fu) --c.score;   // 只有控制字符算噪声
            ++i;
            continue;
        }
        if (b >= 0xA1u && b <= 0xDFu) {          // 单字节半角片假名
            const uint32_t cp = 0xFF61u + (b - 0xA1u);
            appendUtf8(c.text, cp);
            c.score += scoreCp(cp);
            ++i;
            continue;
        }
        const bool isLead = (b >= 0x81u && b <= 0x9Fu) || (b >= 0xE0u && b <= 0xFCu);
        const bool hasTrail = (i + 1 < s.size()) &&
                              ((uint8_t)s[i + 1] >= 0x40u) &&
                              ((uint8_t)s[i + 1] <= 0xFCu) &&
                              ((uint8_t)s[i + 1] != 0x7Fu);
        if (!isLead || !hasTrail) { addUnmapped(c, s, i, 1); ++i; continue; }
        int row = 0, col = 0;
        uint32_t cp = 0;
        if (sjisToJis(b, (uint8_t)s[i + 1], row, col))
            cp = cellAt(kJis0208Hex, row, col);
        if (cp) {
            appendUtf8(c.text, cp);
            c.score += scoreCp(cp);
        } else {
            addUnmapped(c, s, i, 2);
        }
        i += 2;
    }
    return c;
}

inline std::string foldAll(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) appendUtf8(out, foldByte(ch));
    return out;
}

// 一个孤立汉字夹在 ASCII 里，几乎一定是「把西欧字节偶合成了某个双字节码位」
// （it<0x92>s -> it痴），而不是真正的中文文本。真正的中文 / 日文里 CJK
// 字符会成串出现，或者与假名连成一片。这里给这类候选压分，让折叠兜底胜出。
inline int isolatedCjkPenalty(const std::string& t) {
    int count = 0, run = 0, maxRun = 0;
    size_t i = 0;
    while (i < t.size()) {
        const uint8_t c = (uint8_t)t[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xE0u && c <= 0xEFu) { n = 3; cp = c & 0x0Fu; }
        else if (c >= 0xC0u && c <= 0xDFu) { n = 2; cp = c & 0x1Fu; }
        else if (c >= 0xF0u && c <= 0xF4u) { n = 4; cp = c & 0x07u; }
        for (size_t k = 1; k < n && i + k < t.size(); ++k)
            cp = (cp << 6) | ((uint8_t)t[i + k] & 0x3Fu);
        if (scoreCp(cp) >= 2) {
            ++count; ++run;
            if (run > maxRun) maxRun = run;
        } else {
            run = 0;
        }
        i += n;
    }
    return (count <= 2 && maxRun == 1) ? -8 : 0;
}

inline Encoding detect(const std::string& s) {
    if (isValidUtf8(s)) return Encoding::Utf8;
    // 短串里 ASCII 占多数时，双字节候选很容易「偶合」出一个汉字：
    // Windows-1252 的 it<0x92>s 按 Shift-JIS 解会变成 it痴。信息量不够
    // 就别猜，按 CP1252 逐字节保真折叠。真正的 GBK / SJIS 双字节串
    // 高位占比高，不受此限；整份文件的判定汇总了全部文本，更走不到这里。
    if (s.size() <= 8) {
        size_t high = 0;
        for (unsigned char c : s) if (c >= 0x80u) ++high;
        if (high * 2 < s.size()) return Encoding::Fold;
    }
    Candidate g = decodeGb2312(s);
    Candidate j = decodeShiftJis(s);
    g.score += isolatedCjkPenalty(g.text);
    j.score += isolatedCjkPenalty(j.text);
    const int best = g.score > j.score ? g.score : j.score;
    if (best <= 0) return Encoding::Fold;
    return g.score >= j.score ? Encoding::Gb2312 : Encoding::ShiftJis;
}

inline std::string decode(const std::string& s, Encoding enc) {
    switch (enc) {
        case Encoding::Utf8:     return s;
        case Encoding::Gb2312:   return decodeGb2312(s).text;
        case Encoding::ShiftJis: return decodeShiftJis(s).text;
        default:                 return foldAll(s);
    }
}

inline std::string normalize(const std::string& s) {
    return decode(s, detect(s));
}

}  // namespace textenc
}  // namespace m2j

#endif  // M2J_TEXT_ENCODING_HPP

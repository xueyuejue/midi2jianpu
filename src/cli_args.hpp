// cli_args.hpp - 命令行参数校验（header-only）
// 只做纯字符串判断，不碰文件系统 / 输出流，CLI 与单元测试共用同一套口径。
#ifndef M2J_CLI_ARGS_HPP
#define M2J_CLI_ARGS_HPP

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace m2j {
namespace cli {

// 需要紧跟一个取值的选项。
inline const std::vector<std::string>& valueOptions() {
    static const std::vector<std::string> v = {
        "--html", "--musicxml", "--output", "--key", "--gong",
        "--track", "--transpose", "--lyrics", "--pickup", "--bars",
        "--soft-pedal-scale"
    };
    return v;
}

// 不带取值的开关。短别名 -h / -v 也列进来，拼错时才不会被当成输入文件。
inline const std::vector<std::string>& flagOptions() {
    static const std::vector<std::string> v = {
        "--help", "-h", "--version", "-v", "--list-tracks",
        "--no-pentatonic", "--no-key-changes", "--no-midi-lyrics",
        "--no-pickup", "--no-dynamics", "--no-soft-pedal",
        "--no-quantize", "--player", "--bar-numbers"
    };
    return v;
}

inline bool isKnownOption(const std::string& a) {
    for (const auto& s : valueOptions()) if (a == s) return true;
    for (const auto& s : flagOptions())  if (a == s) return true;
    return false;
}

inline bool optionNeedsValue(const std::string& a) {
    for (const auto& s : valueOptions()) if (a == s) return true;
    return false;
}

// -2 / -0.5 / +2 这类是合法取值，不能被当成「下一个选项」。
// 用 strtod 全串消费来判，与 main.cpp 里 parseDouble / parseInt 的口径一致。
inline bool isNumericToken(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && end != s.c_str() && *end == '\0';
}

// token 看起来是选项而不是取值：
//   --html / --foo / -x  -> true
//   -2 / -1.5 / a.mid / o.html / -  -> false
inline bool isOptionToken(const std::string& s) {
    if (s.size() < 2 || s[0] != '-') return false;
    if (isNumericToken(s)) return false;
    return true;
}

inline size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1),
                              prev[j - 1] + cost);
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

// 只给长选项做建议：--bar-number -> --bar-numbers。
// 阈值按长度放宽，且要求「错得不多」相对词长足够小，
// 免得 --foo 被硬凑成 --gong 这种误导。
inline std::string suggestOption(const std::string& a) {
    if (a.size() < 4 || a.compare(0, 2, "--") != 0) return {};
    std::string best;
    size_t bestD = static_cast<size_t>(-1);
    const std::vector<std::string> tables[] = { valueOptions(), flagOptions() };
    for (const auto& table : tables) {
        for (const auto& opt : table) {
            if (opt.compare(0, 2, "--") != 0) continue;
            const size_t d = editDistance(a, opt);
            if (d < bestD) { bestD = d; best = opt; }
        }
    }
    const size_t limit = a.size() <= 8 ? 2 : 3;
    // bestD==0 说明本来就拼对了（这类参数不会走到这里），别自作主张建议自己。
    if (!best.empty() && bestD > 0 && bestD <= limit && bestD * 2 < a.size())
        return best;
    return {};
}

// ---- 输出路径冲突预检（纯字符串，不碰文件系统）----
// 旧版三端各写各的：--output 指到输入 .mid 上会把源文件覆盖成文本谱，
// --output a --html a 则后写的把先写的盖掉，两次都还打印「已写入」。
struct OutputPath {
    const char* option;
    std::string path;
};

// 只用于「是不是同一个目标」的保守比较：Windows 不区分大小写、
// \ 与 / 等价，逐段吃掉 "." 与 ".."、合并重复斜杠。相对路径不与
// 绝对路径强行等同（避免误报），命令行里两种写法都覆盖得到。
inline std::string normalizePath(const std::string& p) {
    std::string s;
    s.reserve(p.size());
    for (unsigned char c : p) {
        char ch = (c == '\\') ? '/' : (char)c;
        s += (char)std::tolower((unsigned char)ch);
    }
    std::string prefix;
    size_t i = 0;
    if (s.size() >= 2 && s[1] == ':') { prefix = s.substr(0, 2); i = 2; }
    if (i < s.size() && s[i] == '/') {
        prefix += '/';
        while (i < s.size() && s[i] == '/') ++i;
    }
    std::vector<std::string> parts;
    while (i < s.size()) {
        const size_t j = s.find('/', i);
        const size_t end = (j == std::string::npos) ? s.size() : j;
        const std::string seg = s.substr(i, end - i);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") {
                if (!parts.empty() && parts.back() != "..") parts.pop_back();
                else if (prefix.empty()) parts.push_back(seg);
            } else parts.push_back(seg);
        }
        i = end + 1;
    }
    std::string out = prefix;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += '/';
        out += parts[k];
    }
    return out.empty() ? "." : out;
}

inline bool samePath(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && normalizePath(a) == normalizePath(b);
}

// 返回空串表示合法，否则是给 stderr 的错误文案（不带「错误: 」前缀）。
// sources 是「除输入 MIDI 外还会被读进来的源文件」清单（当前是 --lyrics
// 指定的 LRC / JSON）。v1.82 只保护输入 MIDI，可外部歌词同样会被三端输出
// 先读后写地盖掉，用户下次跑同一条命令就再也读不到歌词。
inline std::string validateOutputPaths(const std::string& input,
                                        const std::vector<OutputPath>& outs,
                                        const std::vector<OutputPath>& sources) {
    for (const auto& o : outs) {
        if (o.path.empty()) continue;
        if (samePath(o.path, input))
            return std::string(o.option) + " 的输出路径与输入 MIDI 相同（"
                 + o.path + "），会覆盖源文件；请换一个输出路径";
    }
    for (const auto& s : sources) {
        if (s.path.empty()) continue;
        for (const auto& o : outs) {
            if (o.path.empty()) continue;
            if (samePath(o.path, s.path))
                return std::string(o.option) + " 的输出路径与 " + s.option
                     + " 的源文件相同（" + s.path
                     + "），会覆盖歌词源文件；请换一个输出路径";
        }
    }
    for (size_t i = 0; i < outs.size(); ++i) {
        if (outs[i].path.empty()) continue;
        for (size_t j = i + 1; j < outs.size(); ++j) {
            if (outs[j].path.empty()) continue;
            if (samePath(outs[i].path, outs[j].path))
                return std::string(outs[i].option) + " 与 " + outs[j].option
                     + " 指向同一文件（" + outs[i].path
                     + "），后写入的会覆盖先写入的；请分别指定";
        }
    }
    return {};
}

// 两参重载保留 v1.82 起的既有口径（只把输入 MIDI 当源文件）。
inline std::string validateOutputPaths(const std::string& input,
                                        const std::vector<OutputPath>& outs) {
    return validateOutputPaths(input, outs, {});
}

// ---- 调名 / 宫音严格语法 ----
// 旧版 parseKey 只取头 1~2 个字符，尾部一概吞掉："--key Cmaj7" 被当成
// C 小调、"--key C " 被当成 C 大调、"--key C#b" 被当成 C# 大调——
// 用户写错了却拿到一份看似正常、调性却完全不是自己要的谱。
// 合法写法只有 [A-Ga-g](#|b)?[mM]?，且必须全串消费。
struct KeySpec {
    int pitchClass = -1;
    bool isMajor = true;
    bool flatSpelling = false;   // Gb / Bb 这类降号写法要记住「侧」
};

inline bool parseKeySpec(const std::string& s, KeySpec& out) {
    out = KeySpec{};
    if (s.empty() || s.size() > 3) return false;
    char letter = s[0];
    if (letter >= 'a' && letter <= 'g') letter = char(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'G') return false;
    size_t i = 1;
    bool accidental = false;
    if (i < s.size() && (s[i] == '#' || s[i] == 'b')) { accidental = true; ++i; }
    // 大小写 m/M 都表示小调（沿用既有口径，别把 "CM" 悄悄改义）。
    if (i < s.size() && (s[i] == 'm' || s[i] == 'M')) {
        out.isMajor = false;
        ++i;
    }
    if (i != s.size()) return false;
    static const char* names[]     = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* flatNames[] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    std::string name(1, letter);
    if (accidental) name += s[1];
    for (int k = 0; k < 12; ++k)
        if (name == names[k]) { out.pitchClass = k; return true; }
    for (int k = 0; k < 12; ++k)
        if (name == flatNames[k]) {
            out.pitchClass = k;
            out.flatSpelling = true;
            return true;
        }
    return false;
}

// ---- 有限浮点取值 ----
// std::stod("nan") / ("inf") 会成功且全串消费，NaN 又让所有范围比较都为
// false：--soft-pedal-scale nan 穿过 0~1 检查；--pickup nan 更隐蔽，
// "nan >= 0" 为 false 时整段手动弱起逻辑被跳过，用户给的拍数无声消失。
inline bool parseFiniteDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || end == s.c_str() || *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// ---- 两个「指定原调」的入口不能同时给 ----
// 旧版 --gong 优先、--key 被静默丢掉，命令行没有任何提示。
inline std::string validateKeyOptions(const std::string& keyArg,
                                      const std::string& gongArg) {
    if (!keyArg.empty() && !gongArg.empty())
        return "--key 与 --gong 不能同时使用（两者都指定原调，"
               "--gong 会覆盖 --key）；请只保留一个";
    return {};
}

// ---- 两个「弱起」入口不能同时给（v1.97.0） ----
// --pickup N 手动指定弱起、--no-pickup 明确关闭弱起，语义直接冲突。
// 旧版 noPickup 优先、pickupBeats 被静默丢掉，命令行零提示；更糟的是
// 「弱起须短于小节」的越界校验只在 !noPickup 分支里跑，于是
// `--no-pickup --pickup 99` 还能顺带把一个单独会被 rc=1 拒绝的非法值放行。
// 与 validateKeyOptions 同口径：解析后、读 MIDI 前就明确拒绝。
inline std::string validatePickupOptions(bool noPickup, double pickupBeats) {
    if (noPickup && pickupBeats >= 0)
        return "--pickup 与 --no-pickup 不能同时使用（一个手动指定弱起、"
               "一个明确关闭弱起，--no-pickup 会静默覆盖 --pickup，"
               "且会绕过弱起越界校验）；请只保留一个";
    return {};
}

// ---- 同一带值选项重复给（v1.99.0） ----
// 位置参数早在 v1.81 就只允许一个；11 个带值选项却一直是 last-wins：
// `--html a.html --html b.html` 只写 b.html，`--key C --key D` 把调性直接
// 改成 D，全程 rc=0、零提示。开关类（--player 等）重复是幂等的，不在其列。
// 传入 argv[1..argc-1]（不含程序名），纯字符串扫描，CLI 与单测共用。
inline std::string validateDuplicateValueOptions(
        const std::vector<std::string>& args) {
    std::vector<std::string> seen;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (!optionNeedsValue(a)) continue;
        for (const auto& s : seen)
            if (s == a)
                return a + " 被重复指定（后一个值会静默覆盖前一个值）；"
                       "每个带值选项只能给一次，请只保留一个";
        seen.push_back(a);
        // 与 main.cpp 的解析口径一致：紧跟的合法取值不是选项就跳过，
        // 免得把 --transpose -2 里的 -2 当成新的参数名。
        if (i + 1 < args.size() && !isOptionToken(args[i + 1])) ++i;
    }
    return {};
}
}  // namespace cli
}  // namespace m2j
#endif  // M2J_CLI_ARGS_HPP

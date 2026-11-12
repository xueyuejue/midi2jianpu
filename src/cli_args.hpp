// cli_args.hpp - 命令行参数校验（header-only）
// 只做纯字符串判断，不碰文件系统 / 输出流，CLI 与单元测试共用同一套口径。
#ifndef M2J_CLI_ARGS_HPP
#define M2J_CLI_ARGS_HPP

#include <algorithm>
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

}  // namespace cli
}  // namespace m2j
#endif  // M2J_CLI_ARGS_HPP

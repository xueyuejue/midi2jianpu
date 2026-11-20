// xml_verify.hpp - 输出自检：把「口径已经冻结」的自洽性判据收进产品
//
// 判据分两类，处置方式不同：
//
//   * **自洽性**（本模块）：一份输出自己跟定义对不对得上——各 part 等长、
//     每小节时值 == 拍号、小节号连续、part-list 与正文一致、短轨尾节是整小节
//     休止、正文里没有 U+FFFD。这类判据不需要任何旧版基线，对**用户自己的
//     MIDI** 同样成立，所以随产品出厂，`--verify` 现场就能查。
//   * **回归性**（留在 tests/*.py）：同一份素材，新版与旧版的输出应当逐字节
//     一致。这必须同时跑两个可执行文件再比对，Python 侧的 subprocess +
//     difflib 是更合适的工具，本模块不掺和。
//
// 换句话说：这里回答「这份谱对不对」，tests/check_*.py 回答「这次改动动了
// 别的地方没有」。两边判据互不替代，也互不共享代码——Python 那侧是独立
// 实现，正好能抓到本模块与 writer 一起犯的同一个错。
#ifndef M2J_XML_VERIFY_HPP
#define M2J_XML_VERIFY_HPP

#include <string>
#include <vector>

namespace m2j {

// 一处自检问题。where 定位到输出 / part / 小节，what 是判定结论。
struct VerifyIssue {
    std::string where;
    std::string what;
};

// 一次自检的结果。
struct VerifyReport {
    std::vector<VerifyIssue> issues;
    int parts = 0;             // MusicXML 里的 <part> 个数
    int measures = 0;          // 每个 part 的小节数（各 part 等长）
    int implicitMeasures = 0;  // 其中标了 implicit="yes" 的弱起 / 被截短小节

    bool ok() const { return issues.empty(); }
};

// 体检三端输出。任一参数传空串表示「本次没生成这端」，跳过它的检查。
//   text : 文本简谱（含页眉）
//   html : HTML 谱面
//   xml  : MusicXML
VerifyReport verifyOutputs(const std::string& text, const std::string& html,
                           const std::string& xml);

// 一屏能读懂的结论：通过时一行摘要，失败时逐条列出。
// label 是素材名（如 sample.mid），作为前缀进每条问题，便于对照。
std::string formatVerifyReport(const VerifyReport& r, const std::string& label);

}  // namespace m2j

#endif  // M2J_XML_VERIFY_HPP

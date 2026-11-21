// text_verify.hpp - 文本谱自检：一小节印出来的拍数必须等于这一小节的拍号
//
// 这是简谱最硬的约束。读者按 token 数拍子：一小节少印一拍，后面每个音头
// 的位置就整体前移，谱子按着唱会越唱越早。而这类错在文本谱上极难看出来
// ——`0.` 是 1.5 拍、`0` 是 1 拍，两者只差一个点，读谱的人未必当场发现。
//
// 判据的**基准是内部网格 BarGrid**，不是页眉文字：
//   * 页眉那行「拍号=4/4 → 3/4(第3小节)」是给人看的摘要，用它当尺子等于
//     拿被检查对象自己的说法当标准（tests/check_bar_beats.py 就是这么做的，
//     所以它只能抓「摘要与谱面不一致」，抓不到「两边一起错」）。
//   * BarGrid 是三个输出层共用的那一份网格（弱起、变拍号、0x58 落在小节
//     中间被截断，全都在里面），拿它当尺子才叫自洽性检查。
//
// 豁免完全由模型给出，不靠启发式：
//   * 这一小节在网格里本来就不满（弱起小节、变拍号截断小节）→ 不判；
//   * 每条谱行的最后一小节（尾上休止按设计不补）→ 不判；
//   * 认不出的 token（本行放弃判定）→ 不判。
// 只有「网格说这一小节是整整一小节、谱面却印出别的拍数」才报。
#ifndef M2J_TEXT_VERIFY_HPP
#define M2J_TEXT_VERIFY_HPP

#include <string>
#include <vector>

#include "midi2jianpu/midifile.hpp"

namespace m2j {

// 一小节的核对结果。track/bar 都从 1 起给人看。
struct TextBarIssue {
    int line = 1;        // 第几条谱行（输出顺序，从 1 起）
    long bar = 0;        // 网格里的小节序号（0 起；报告里 +1 显示）
    double printed = 0;  // 印出来的拍数
    double want = 0;     // 这一小节的拍号拍数
    std::string text;    // 该小节原文（过长会截断）
};

struct TextVerifyReport {
    int lines = 0;      // 认出来的谱行数
    int bars = 0;       // 核对过的小节数
    int skipped = 0;    // 按设计豁免的小节数（弱起 / 截短 / 末小节 / 认不出）
    std::vector<TextBarIssue> issues;  // 真问题：网格说是整小节，谱面却不满
    // `--no-quantize` 保留 MIDI 原始时值，时值只能吸附到最近的记谱档
    // （0.975 拍印成 `1`），个别小节拍数不满是这种模式的必然结果，
    // 不是缺陷。放到这里单独提示，不影响 ok()。
    std::vector<TextBarIssue> approx;
    // 音头 / 时值本身不在 1/8 拍网格上的小节（MIDI 里 1780 tick 这种位置，
    // 人性化演奏与晚进声部都很常见）：简谱能记到的最小单位是 1/8 拍，
    // 余下不足 1/8 拍的部分**印不出来**，这一小节必然短一截。判据不是
    // 「差多少都给过」——差多少必须落在模型算出来的洞的上界之内，且印多了
    // （超过拍号）永远算缺陷。与上面 --no-quantize 的 approx 分开记账，
    // 因为成因完全不同，提示语也不同。
    std::vector<TextBarIssue> approxGrid;

    bool ok() const { return issues.empty(); }
};

// 体检一段文本简谱（含页眉与小标题，与写盘的那一份逐字相同）。
// allowApprox=true 表示这次是 --no-quantize：拍数不满只进 approx。
TextVerifyReport verifyTextScore(const std::string& text, const MidiData& data,
                                 bool allowApprox);

// 一屏能读懂的结论。label 是素材名（如 sample.mid）。
std::string formatTextVerifyReport(const TextVerifyReport& r,
                                  const std::string& label);

}  // namespace m2j

#endif  // M2J_TEXT_VERIFY_HPP

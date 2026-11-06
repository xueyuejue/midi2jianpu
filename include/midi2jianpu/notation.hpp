#pragma once
// notation.hpp - 调性识别与音高->简谱转换
#include "midi2jianpu/midifile.hpp"
#include <string>
#include <vector>

namespace m2j {

// 调性识别结果
struct KeyEstimate {
    int tonicPitchClass = 0;  // 0=C ... 11=B
    bool isMajor = true;
};

// 根据 MIDI 数据估计调性（优先用内置调号，否则 Krumhansl-Schmuckler 相关度匹配）
KeyEstimate estimateKey(const MidiData& data);

// 简谱调号的 do 音所在音级。
// 简谱用首调唱名：小调不写 1=主音，而是把关系大调的 do 记作 1，主音落在 6 上
// （la 为主音）。所以 a 小调的调号是 1=C，e 小调是 1=G。
int notationDoPitchClass(const KeyEstimate& key);

// 调号音名（已按首调换算），如 "C"、"Bb"；a 小调返回 "C"
std::string keyName(const KeyEstimate& key);

// 完整调性描述，如 "1=C (a小调)"、"1=G 大调"
std::string keyDescription(const KeyEstimate& key);

// 该调性是否习惯用降号记谱（调号含降号）
bool prefersFlats(const KeyEstimate& key);

// 调号升降数（-7..+7），供 MusicXML 等输出使用
int keyFifths(const KeyEstimate& key);

// 单个音高 -> 简谱片段（只含数字/升降/八度，不含时值）
std::string pitchToJianpu(int pitch, const KeyEstimate& key);

// 时值 tick -> 简谱时值后缀（延长线/下划线）
//   4拍="---" 3拍="--" 2拍="-" 附点四分="." 四分=""
//   附点八分="_." 八分="_" 附点十六="=." 十六="=" 三十二="%%"
std::string durationSuffix(long durationTicks, int division);

// 时值量化报告
struct QuantizeReport {
    int adjusted = 0;    // 被修正时值/位置的音符数
    long gridTicks = 0;  // 实际使用的音头网格（tick）
    double gate = 1.0;   // 估计出的门限因子（note-off 提前比例）
    bool changed() const { return adjusted > 0; }
};

// 把演奏 MIDI 的时值吸附到记谱网格。
//
// 真实录音转出的 MIDI 里，note-off 普遍早于下一个音头（门限时间 85% 左右是
// DAW 的常见默认值），直接拿时长查时值表会把八分音符判成附点十六分。这里做两件事：
//   1) 音头吸附到 1/24 拍网格（同时容纳 32 分音符与三连音）
//   2) 时长取「到下一个音头的时间」——当原时长接近该间隔时（>=70%），
//      说明只是门限衰减而非休止；否则按原时长吸附到最近的合法时值
// 会就地修改 data.notes 的 startTicks / durationTicks，seconds 保持不变。
QuantizeReport quantizeTiming(MidiData& data);

// 把音符列表格式化成文本简谱（含拍号、小节线、时值）
std::string formatJianpu(const MidiData& data, const KeyEstimate& key);

// MIDI 音号 -> 音名字符串 (60 -> "C4")
std::string midiNoteName(int pitch);

// 检测三连音: 返回每个音符是否属于三连音组
std::vector<bool> detectTriplets(const std::vector<NoteEvent>& notes, int division);

// 歌词字（对齐到音符）
struct LyricSyllable {
    long tick;      // 对齐的起始tick
    std::string text;  // 单字
};

// 歌词行（起始tick + 整行文本）
struct LyricLine {
    long startTick;
    std::string text;
};

// 解析歌词JSON文件，返回按tick排序的歌词字列表
std::vector<LyricSyllable> parseLyrics(const std::string& jsonPath, int division);

// 解析LRC歌词文件（[mm:ss.xx]歌词），返回按tick排序的歌词字列表
std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, int division, int bpm);

// 解析LRC为行列表（用于按音符对齐）
std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, int division, int bpm);

// 同 parseLrcLines，但输入是 LRC 文本而不是路径。
// WASM 版没有文件系统，只能走这个入口。
std::vector<LyricLine> parseLrcText(const std::string& lrc, int division, int bpm);

// 和弦标记
struct ChordMark {
    long startTick;
    std::string name;  // 如 "C", "Am", "G7"
};

// 单条轨道的特征，供旋律轨判定使用
struct TrackProfile {
    int track = -1;
    size_t noteCount = 0;
    size_t onsetCount = 0;        // 不同音头的个数
    double polyphony = 1.0;       // 每个音头的平均发声音数（1 表示单声部）
    size_t distinctPitches = 0;
    int minPitch = 0;
    int maxPitch = 0;
    double avgPitch = 0;
    bool lyricAligned = false;    // 歌词行首能否落在该轨的音符上
    double score = 0;
};

// 统计每条轨道的特征
std::vector<TrackProfile> profileTracks(const MidiData& data,
                                       const std::vector<LyricLine>* lyrics = nullptr);

// 判定哪条轨道是旋律轨。
//
// 旧实现在四个地方各自写了一遍"音符最多的轨就是旋律轨"。真实编曲工程里音符最多
// 的那条轨几乎永远是鼓或柱式和弦伴奏，于是歌词和和弦标记全挂到伴奏上，主旋律
// 反而被当成伴奏。现在改成按证据强弱打分：
//   1) 歌词行首与音符对齐（最强证据）
//   2) 单音率——旋律是单声部，柱式和弦的复音数远大于 1
//   3) 音高丰富度与音域——贝斯也是单声部，但音域窄、音高种类少
//   4) 音符数只作微调
// 返回 -1 表示没有可用轨道。
int pickMelodyTrack(const MidiData& data, const std::vector<LyricLine>* lyrics = nullptr);

// 速度摘要（变速曲目会列出每次变化及其小节）
std::string tempoSummary(const MidiData& data);

// ---- 小节网格 ----
// 「tick -> 小节」的换算分散在文本 / HTML / MusicXML 三个输出里，弱起一进来
// 就得三处同时改，否则谱面一半对一半错。统一收进这个结构体。
struct BarGrid {
    long barLen = 1920;   // 一小节多少 tick
    long pickup = 0;      // 弱起小节长度 tick（0 = 没有弱起）
    int beatsPerBar = 4;  // 每小节拍数（拍号分子）

    long barIndex(long tick) const { return barIndexAt(tick, barLen, pickup); }
    long barStart(long bar) const { return barStartTick(bar, barLen, pickup); }
    long barEnd(long bar) const { return barEndTick(bar, barLen, pickup); }

    // 谱面上印出的小节号：弱起小节不编号，第一个完整小节才是 1。
    int measureNumber(long bar) const { return pickup > 0 ? (int)bar : (int)bar + 1; }

    // 一小节的实际 tick 数（弱起小节比正常小节短）
    long barSpan(long bar) const { return barEnd(bar) - barStart(bar); }
};

BarGrid barGridOf(const MidiData& data);

// 弱起（anacrusis）小节长度，单位 tick；0 表示判不出弱起。
//
// 判定只用硬证据，宁可漏判也不误判——判错的代价是整份谱子的小节线全错，
// 而漏判时用户还能用 --pickup N 手动指定：
//   1) 曲子必须从 tick 0 直接起唱。开头若干小节空白是前奏休止，不是弱起；
//   2) 全曲最后一个音的收尾必须落在小节线上。弱起的总长是 P + N*小节长，
//      于是「末音位置 mod 小节长」就是弱起长度，四舍五入到整拍；
//   3) 弱起之后那一拍必须真有音起，否则只是长音停在小节中间（最常见的误判）。
// 三条都成立时，弱起长度没有第二种解释，才敢用。
long detectPickupTicks(const MidiData& data);

// 实际要用的弱起长度：data.pickupTicks >= 0 时直接用它（手动覆盖），
// 否则走 detectPickupTicks（自动）。
long effectivePickupTicks(const MidiData& data);

// 弱起的人类可读说明，如 "弱起 1 拍"；没有弱起时返回空串。
// 文本谱头与 HTML 页眉共用，判错了用户一眼能看出来并去改 --pickup。
std::string pickupLabel(const MidiData& data);

// 和声窗长度（tick）：4/4 半小节、6/8 附点四分、3/4 整小节
long chordWindowTicks(const MidiData& data);

// 从MIDI数据中检测和弦（按和声窗，相邻同名合并）
std::vector<ChordMark> detectChords(const MidiData& data, const KeyEstimate& key);

} // namespace m2j

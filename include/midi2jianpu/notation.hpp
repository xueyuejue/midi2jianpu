#pragma once
// notation.hpp - 调性识别与音高->简谱转换
#include "midi2jianpu/midifile.hpp"
#include <string>

namespace m2j {

// 调性识别结果
struct KeyEstimate {
    int tonicPitchClass = 0;  // 0=C ... 11=B
    bool isMajor = true;
};

// 根据 MIDI 数据估计调性（优先用内置调号，否则 Krumhansl-Schmuckler 相关度匹配）
KeyEstimate estimateKey(const MidiData& data);

// 调性名（自动选择升/降写法，如 "Bb" 而不是 "A#"）
std::string keyName(const KeyEstimate& key);

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

// 和弦标记
struct ChordMark {
    long startTick;
    std::string name;  // 如 "C", "Am", "G7"
};

// 从MIDI数据中检测和弦(按小节)
std::vector<ChordMark> detectChords(const MidiData& data, const KeyEstimate& key);

} // namespace m2j

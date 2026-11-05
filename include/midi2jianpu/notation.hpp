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

// 根据音符序列估计主音与大小调
KeyEstimate estimateKey(const std::vector<NoteEvent>& notes);

// 单个音高 -> 简谱片段
// 返回如 "1"、"1'"、"#4"、"5,"（高八度加'，低八度加,）
std::string pitchToJianpu(int pitch, const KeyEstimate& key);

// 把音符列表格式化成文本简谱
std::string formatJianpu(const std::vector<NoteEvent>& notes,
                         const KeyEstimate& key);

// MIDI 音号 -> 音名字符串 (60 -> "C4")
std::string midiNoteName(int pitch);

} // namespace m2j

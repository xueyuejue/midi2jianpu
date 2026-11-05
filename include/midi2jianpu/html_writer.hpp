#pragma once
// html_writer.hpp - 把简谱渲染成自包含 HTML
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include <string>

namespace m2j {

// 生成完整的自包含 HTML 文档（含内联CSS）
std::string renderHtml(const MidiData& data, const KeyEstimate& key,
                       const std::string& title = "",
                       const std::vector<LyricLine>* lyricLines = nullptr);

} // namespace m2j

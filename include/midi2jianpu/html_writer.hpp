#pragma once
// html_writer.hpp - 把简谱渲染成自包含 HTML
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include <string>

namespace m2j {

// 生成完整的自包含 HTML 文档（含内联CSS）
//
// player=true 时额外内嵌一个「试听播放器」：Web Audio 合成旋律、当前音符
// 高亮、可调速。默认 false —— 播放器会给每个音符列加 data-t 并内嵌一段
// JS 与 JSON，不带开关的输必须与旧版逐字节一致（既有素材的零回归靠这条守住）。
std::string renderHtml(const MidiData& data, const KeyEstimate& key,
                       const std::string& title = "",
                       const std::vector<LyricLine>* lyricLines = nullptr,
                       bool player = false);

} // namespace m2j

#pragma once
// musicxml_writer.hpp - 把 MIDI 数据导出为 MusicXML（可被 MuseScore 打开）
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include <string>

namespace m2j {

// 生成 MusicXML 3.1 文档
// lyricLines 非空时，旋律轨会写出 <lyric> 与一字多音的 <slur>。
// 默认 nullptr 保持「只导谱、不带词」的旧行为。
std::string renderMusicXml(const MidiData& data, const KeyEstimate& key,
                           const std::vector<LyricLine>* lyricLines = nullptr);

} // namespace m2j

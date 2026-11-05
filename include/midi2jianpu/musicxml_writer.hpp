#pragma once
// musicxml_writer.hpp - 把 MIDI 数据导出为 MusicXML（可被 MuseScore 打开）
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include <string>

namespace m2j {

// 生成 MusicXML 3.1 文档
std::string renderMusicXml(const MidiData& data, const KeyEstimate& key);

} // namespace m2j

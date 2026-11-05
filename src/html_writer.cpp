// html_writer.cpp - 简谱 -> 自包含 HTML 渲染
#include "midi2jianpu/html_writer.hpp"

#include <map>
#include <sstream>

namespace m2j {

static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

static std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

// 把单个音符转成 HTML 片段
static std::string noteToHtml(int pitch, long durTicks,
                              const KeyEstimate& key, int division) {
    std::string jp = pitchToJianpu(pitch, key);  // 如 "1'", "#4", "5,"
    // 解析出八度标记
    int highDots = 0, lowDots = 0;
    std::string num;
    for (char c : jp) {
        if (c == '\'') highDots++;
        else if (c == ',') lowDots++;
        else num += c;
    }

    // 时值 class
    double beats = (double)durTicks / division;
    std::string durClass;
    if (beats >= 3.5) durClass = "whole";
    else if (beats >= 1.5) durClass = "half";
    else if (beats >= 0.75) durClass = "quarter";
    else if (beats >= 0.375) durClass = "eighth";
    else durClass = "sixteenth";

    std::ostringstream os;
    os << "<span class='note " << durClass << "'";
    if (highDots > 0) os << " dot-above" << (highDots > 1 ? "2" : "");
    if (lowDots > 0) os << " dot-below" << (lowDots > 1 ? "2" : "");
    os << "><span class='num'>" << htmlEscape(num) << "</span>";
    // 延长线（二分/全音符）
    if (beats >= 1.5) {
        int lines = (beats >= 3.5) ? 3 : 1;
        for (int i = 0; i < lines; i++) os << "<span class='ext'>-</span>";
    }
    os << "</span>";
    return os.str();
}

std::string renderHtml(const MidiData& data, const KeyEstimate& key) {
    std::ostringstream body;
    double barLen = data.timeSig.barLengthTicks(data.division);

    // 按轨分组
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (const auto& n : data.notes) byTrack[n.track].push_back(n);
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, ns] : byTrack)
        if (ns.size() > maxN) { maxN = ns.size(); mainTrack = t; }

    for (auto& [t, ns] : byTrack) {
        body << "<div class='track'><h3>"
             << (t == mainTrack ? "旋律轨" : "伴奏轨 " + std::to_string(t))
             << " <small>(" << ns.size() << " 音符)</small></h3><div class='staff'>";

        int curBar = -1;
        long prevEnd = 0;
        for (const auto& n : ns) {
            int bar = (int)(n.startTicks / barLen);
            if (bar != curBar) {
                if (curBar >= 0) body << "<span class='bar'>|</span>";
                curBar = bar;
            }
            // 休止符
            long gap = n.startTicks - prevEnd;
            if (gap >= data.division / 2 && prevEnd > 0) {
                body << "<span class='note quarter'><span class='num'>0</span></span>";
            }
            body << noteToHtml(n.pitch, n.durationTicks, key, data.division);
            prevEnd = n.startTicks + n.durationTicks;
        }
        body << "<span class='bar'>|</span></div></div>";
    }

    std::ostringstream html;
    html << R"(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<title>midi2jianpu 简谱</title><style>
body{font-family:"Noto Serif SC","SimSun",serif;max-width:900px;margin:40px auto;background:#fafafa;color:#222}
h1{font-size:22px;border-bottom:2px solid #333;padding-bottom:8px}
.meta{color:#666;margin-bottom:20px}
.track{background:#fff;border:1px solid #ddd;border-radius:8px;padding:16px 24px;margin-bottom:16px}
.track h3{margin:0 0 12px;font-size:15px;color:#444}
.track h3 small{color:#999;font-weight:normal}
.staff{font-size:32px;line-height:2;padding:10px 0;white-space:nowrap;overflow-x:auto}
.note{display:inline-block;position:relative;margin:0 8px;text-align:center;min-width:24px}
.num{font-weight:600}
.bar{color:#999;margin:0 10px;font-weight:300}
.dot-above::before{content:"";position:absolute;top:-6px;left:50%;transform:translateX(-50%);width:5px;height:5px;border-radius:50%;background:#222}
.dot-above2::before{top:-14px}
.dot-below::after{content:"";position:absolute;bottom:-6px;left:50%;transform:translateX(-50%);width:5px;height:5px;border-radius:50%;background:#222}
.dot-below2::after{bottom:-14px}
.eighth .num{border-bottom:2px solid #222}
.sixteenth .num{border-bottom:2px solid #222;box-shadow:0 3px 0 #222}
.ext{margin-left:2px;letter-spacing:2px;color:#222}
</style></head><body>
<h1>简谱</h1>
<div class="meta">1 = )"
         << NOTE_NAMES[key.tonicPitchClass]
         << (key.isMajor ? " 大调" : " 小调")
         << " ｜ " << data.timeSig.toString()
         << " ｜ " << (data.keySig.valid ? "MIDI调号" : "自动猜测")
         << "</div>" << body.str()
         << "<footer style='color:#aaa;font-size:12px;margin-top:20px'>由 midi2jianpu 生成</footer>"
         << "</body></html>";
    return html.str();
}

} // namespace m2j

// html_writer.cpp - 简谱 -> 自包含 HTML 渲染
#include "midi2jianpu/html_writer.hpp"

#include <map>
#include <sstream>
#include <cmath>

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
                              const KeyEstimate& key, int division,
                              const std::string& lyric = "") {
    std::string jp = pitchToJianpu(pitch, key);  // 如 "1'", "#4", "5,"
    // 解析出八度标记
    int highDots = 0, lowDots = 0;
    std::string num;
    for (char c : jp) {
        if (c == '\'') highDots++;
        else if (c == ',') lowDots++;
        else num += c;
    }

    // 时值：最近邻匹配标准时值
    double beats = (double)durTicks / division;
    struct DurT { double beats; const char* cls; bool dotted; int extLines; };
    DurT table[] = {
        {0.25, "sixteenth", false, 0},
        {0.5,  "eighth",    false, 0},
        {0.75, "eighth",    true,  0},
        {1.0,  "quarter",   false, 0},
        {1.5,  "quarter",   true,  0},
        {2.0,  "half",      false, 1},
        {3.0,  "half",      true,  1},
        {4.0,  "whole",     false, 3},
    };
    const char* durClass = "quarter";
    bool dotted = false;
    int extLines = 0;
    double bestDist = 1e9;
    for (auto& d : table) {
        double dist = std::abs(beats - d.beats);
        if (dist < bestDist) {
            bestDist = dist;
            durClass = d.cls; dotted = d.dotted; extLines = d.extLines;
        }
    }

    std::ostringstream os;
    os << "<span class='note " << durClass;
    if (highDots > 0) os << " dot-above" << (highDots > 1 ? "2" : "");
    if (lowDots > 0) os << " dot-below" << (lowDots > 1 ? "2" : "");
    if (dotted) os << " dotted";
    os << "'><span class='num'>" << htmlEscape(num) << "</span>";
    for (int i = 0; i < extLines; i++) os << "<span class='ext'>-</span>";
    if (!lyric.empty()) os << "<span class='lyric'>" << htmlEscape(lyric) << "</span>";
    os << "</span>";
    return os.str();
}

std::string renderHtml(const MidiData& data, const KeyEstimate& key,
                        const std::string& title,
                        const std::vector<LyricLine>* lyricLines) {
    std::string displayTitle = title.empty() ? "简谱" : title;
    std::ostringstream body;
    double barLen = data.timeSig.barLengthTicks(data.division);

    // 歌词对齐: 把行文本按字拆分, 对齐到该行内的音符
    // 预处理: 每行歌词的字列表
    std::vector<std::pair<long, std::vector<std::string>>> lineSyls;
    if (lyricLines) {
        for (const auto& ll : *lyricLines) {
            std::vector<std::string> syls;
            for (size_t i = 0; i < ll.text.size(); ) {
                unsigned char c = ll.text[i];
                int charLen = 1;
                if ((c & 0xE0) == 0xC0) charLen = 2;
                else if ((c & 0xF0) == 0xE0) charLen = 3;
                else if ((c & 0xF8) == 0xF0) charLen = 4;
                if (i + charLen > ll.text.size()) break;
                syls.push_back(ll.text.substr(i, charLen));
                i += charLen;
            }
            lineSyls.push_back({ll.startTick, syls});
        }
    }
    // 当前行索引 + 已分配的字索引
    int curLineIdx = -1;
    int curCharIdx = 0;
    auto lyricForNote = [&](long tick) -> std::string {
        if (lineSyls.empty()) return "";
        // 找当前tick属于哪一行
        int bestLine = -1;
        for (int i = 0; i < (int)lineSyls.size(); i++) {
            if (lineSyls[i].first <= tick) bestLine = i;
            else break;
        }
        if (bestLine < 0) return "";
        // 只有真正换行了才重置字索引
        if (bestLine != curLineIdx) {
            curLineIdx = bestLine;
            curCharIdx = 0;
        }
        if (curCharIdx >= (int)lineSyls[curLineIdx].second.size()) return "";
        return lineSyls[curLineIdx].second[curCharIdx++];
    };

    // 弯音修正
    auto bendOffset = [&](long start, long end) -> double {
        if (data.pitchBends.empty()) return 0.0;
        double sum = 0; int count = 0;
        for (const auto& pb : data.pitchBends) {
            if (pb.tick >= start && pb.tick <= end) {
                sum += (pb.value - 8192) / 8192.0 * data.bendRange;
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    // 按轨分组（应用弯音修正）
    std::map<int, std::vector<NoteEvent>> byTrack;
    for (auto n : data.notes) {
        double off = bendOffset(n.startTicks, n.startTicks + n.durationTicks);
        if (std::abs(off) >= 0.5) n.pitch += (int)std::round(off);
        byTrack[n.track].push_back(n);
    }
    int mainTrack = -1; size_t maxN = 0;
    for (auto& [t, ns] : byTrack)
        if (ns.size() > maxN) { maxN = ns.size(); mainTrack = t; }

    for (auto& [t, ns] : byTrack) {
        body << "<div class='track'><h3>";
        auto it = data.trackNames.find(t);
        if (it != data.trackNames.end() && !it->second.empty()) {
            body << it->second;
        } else {
            body << (t == mainTrack ? "旋律轨" : "伴奏轨 " + std::to_string(t));
        }
        body << " <small>(" << ns.size() << " 音符)</small></h3><div class='staff'>";

        int curBar = -1;
        long prevEnd = 0;
        int lastPitch = -1;
        std::string lastDyn;
        auto triplets = detectTriplets(ns, data.division);
        bool inTriplet = false;
        for (size_t idx = 0; idx < ns.size(); idx++) {
            const auto& n = ns[idx];
            int bar = (int)(n.startTicks / barLen);
            if (bar != curBar) {
                if (curBar >= 0) body << "</span>";
                curBar = bar;
                lastPitch = -1;
                body << "<span class='measure'>";
                if (!data.dynamics.empty()) {
                    int val = 80;
                    for (const auto& dp : data.dynamics) {
                        if (dp.tick <= n.startTicks) val = dp.value; else break;
                    }
                    const char* dyn = "mf";
                    if (val < 20) dyn = "pp"; else if (val < 40) dyn = "p";
                    else if (val < 60) dyn = "mp"; else if (val < 80) dyn = "mf";
                    else if (val < 100) dyn = "f"; else dyn = "ff";
                    std::string ds = dyn;
                    if (ds != lastDyn) {
                        body << "<span class='dyn'>" << dyn << "</span>";
                        lastDyn = ds;
                    }
                }
            }
            if (triplets[idx] && !inTriplet) {
                body << "<sup class='trip'>3</sup>";
                inTriplet = true;
            }
            if (!triplets[idx] && inTriplet) inTriplet = false;
            long gap = n.startTicks - prevEnd;
            bool hasRest = (gap >= data.division / 2 && prevEnd > 0);
            if (hasRest) {
                body << "<span class='note quarter'><span class='num'>0</span></span>";
                lastPitch = -1;
            }
            // 歌词显示在音符下方 (休止符和延音线不分配歌词)
            bool isExt = !triplets[idx] && n.pitch == lastPitch && !hasRest && gap < data.division / 4;
            std::string syl;
            if (lyricLines && n.pitch != 0 && !isExt) {
                syl = lyricForNote(n.startTicks);
            }
            if (isExt) {
                body << "<span class='ext'>-</span>";
            } else {
                body << noteToHtml(n.pitch, n.durationTicks, key, data.division, syl);
                lastPitch = n.pitch;
            }
            prevEnd = n.startTicks + n.durationTicks;
        }
        body << "</span><span class='final-bar'></span></div></div>";
    }

    std::ostringstream html;
    html << "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
         << "<title>" << displayTitle << "</title>"
         << R"(<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='0.9em' font-size='90'>1</text></svg>"><style>
body{font-family:"Noto Serif SC","SimSun",serif;max-width:900px;margin:40px auto;background:#fafafa;color:#222}
h1{font-size:24px;border-bottom:none;padding-bottom:8px;text-align:center;color:#b03030;margin-bottom:4px}
.meta{color:#666;margin-bottom:24px;font-size:14px;text-align:center}
.track{background:#fff;border:1px solid #ddd;border-radius:8px;padding:20px 32px;margin-bottom:16px}
.track h3{margin:0 0 12px;font-size:15px;color:#444;text-align:center}
.track h3 small{color:#999;font-weight:normal}
.staff{font-size:24px;line-height:2.4;padding:10px 0}
.measure{display:inline-block;margin:0 2px 8px 0;padding:0 4px 0 6px;white-space:nowrap;border-left:1px solid #ccc;vertical-align:bottom}
.measure:first-child{border-left:none}
.note{display:inline-flex;flex-direction:column;justify-content:flex-start;align-items:center;position:relative;margin:0 4px;min-width:20px;min-height:50px;vertical-align:bottom}
.num{font-weight:600;line-height:1.2}
.bar{color:#999;margin:0 12px;font-weight:300}
.dyn{font-size:16px;font-style:italic;color:#888;margin-right:8px}
.trip{font-size:14px;color:#666;margin-right:2px}
.lyric{display:block;font-size:14px;color:#555;text-align:center;margin-top:4px;min-height:20px}
.final-bar{display:inline-block;width:2px;height:32px;background:#333;margin-left:8px;vertical-align:middle}
.final-bar::after{content:"";display:block;width:4px;height:32px;background:#333;margin-left:4px;margin-top:-32px}
.footer{color:#999;font-size:12px;text-align:center;margin-top:20px}
@media print{
  body{background:#fff;padding:0}
  .track-card{border:none;page-break-inside:avoid}
  .measure{page-break-inside:avoid}
}
.dot-above::before{content:"";position:absolute;top:-4px;left:50%;transform:translateX(-50%);width:7px;height:7px;border-radius:50%;background:#222}
.dot-above2::before{top:-16px}
.dot-below::after{content:"";position:absolute;bottom:-4px;left:50%;transform:translateX(-50%);width:7px;height:7px;border-radius:50%;background:#222}
.dot-below2::after{bottom:-16px}
.eighth .num{border-bottom:2px solid #222}
.sixteenth .num{border-bottom:2px solid #222;box-shadow:0 4px 0 #222}
.dotted .num::after{content:"";display:inline-block;width:5px;height:5px;border-radius:50%;background:#222;margin-left:4px}
.ext{margin-left:2px;letter-spacing:3px;color:#222}
</style></head><body>
<h1>)" << displayTitle << R"(</h1>
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

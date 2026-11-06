// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/version.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

static void printHelp() {
    std::cout <<
        "midi2jianpu " M2J_VERSION " - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>                 打印文本简谱\n"
        "  midi2jianpu <input.mid> --html out.html 导出 HTML 谱面\n"
        "  midi2jianpu <input.mid> --musicxml out.xml  导出 MusicXML(可用 MuseScore 打开)\n"
        "  midi2jianpu <input.mid> --key Am         手动指定调性(如 Am=小调, C=大调)\n"
        "  midi2jianpu <input.mid> --output out.txt  文本简谱输出到文件\n"
        "  midi2jianpu <input.mid> --track 0         只输出指定轨道\n"
        "  midi2jianpu <input.mid> --transpose +2     移调(半音, 正负都可)\n"
        "  midi2jianpu <input.mid> --lyrics lyrics.lrc  加载歌词\n"
        "  midi2jianpu <input.mid> --pickup 1       手动指定弱起小节长度(拍)\n"
        "  midi2jianpu <input.mid> --no-pickup      关闭弱起自动判定\n"
        "  midi2jianpu <input.mid> --no-quantize    关闭时值量化(保留 MIDI 原始时长)\n"
        "  midi2jianpu --version\n"
        "  midi2jianpu --help\n";
}

// 解析 --key 参数: "C"=C大调, "Am"=a小调, "F#"=F#大调, "Bb"=降B大调
static bool parseKey(const std::string& s, m2j::KeyEstimate& out) {
    static const char* names[]     = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* flatNames[] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    if (s.empty()) return false;
    size_t i = 0;
    std::string name;
    if (s.size() >= 2 && (s[1] == '#' || s[1] == 'b')) { name = s.substr(0, 2); i = 2; }
    else { name = s.substr(0, 1); i = 1; }
    if (name[0] >= 'a' && name[0] <= 'z') name[0] = char(name[0] - 'a' + 'A');

    int pc = -1;
    for (int k = 0; k < 12; k++) if (names[k] == name) { pc = k; break; }
    if (pc < 0) for (int k = 0; k < 12; k++) if (flatNames[k] == name) { pc = k; break; }
    if (pc < 0) return false;

    out.tonicPitchClass = pc;
    out.isMajor = !(i < s.size() && (s[i] == 'm' || s[i] == 'M'));
    return true;
}

// 安全的整数解析：非法输入不再抛异常中断程序
static bool parseInt(const char* s, int& out) {
    try {
        size_t used = 0;
        std::string str(s);
        int v = std::stoi(str, &used);
        if (used != str.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// 拍数解析：弱起长度可以带小数（1.5 拍这种半拍弱起在歌本里并不罕见）
static bool parseBeats(const char* s, double& out) {
    try {
        size_t used = 0;
        std::string str(s);
        double v = std::stod(str, &used);
        if (used != str.size() || v < 0) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printHelp(); return 1; }

    std::string input, htmlOut, keyArg, txtOut, xmlOut, lyricsPath;
    int trackFilter = -1;
    int transpose = 0;
    bool quantize = true;
    double pickupBeats = -1;   // <0 表示没指定（走自动判定）
    bool noPickup = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--version" || a == "-v") {
            std::cout << "midi2jianpu " M2J_VERSION "\n";
            return 0;
        }
        else if (a == "--html" && i + 1 < argc) { htmlOut = argv[++i]; }
        else if (a == "--key" && i + 1 < argc) { keyArg = argv[++i]; }
        else if (a == "--output" && i + 1 < argc) { txtOut = argv[++i]; }
        else if (a == "--musicxml" && i + 1 < argc) { xmlOut = argv[++i]; }
        else if (a == "--lyrics" && i + 1 < argc) { lyricsPath = argv[++i]; }
        else if (a == "--no-quantize") { quantize = false; }
        else if (a == "--no-pickup") { noPickup = true; }
        else if (a == "--pickup" && i + 1 < argc) {
            if (!parseBeats(argv[++i], pickupBeats)) {
                std::cerr << "错误: --pickup 需要正数（单位：拍，如 1 / 1.5）\n"; return 1;
            }
        }
        else if (a == "--track" && i + 1 < argc) {
            if (!parseInt(argv[++i], trackFilter)) {
                std::cerr << "错误: --track 需要整数\n"; return 1;
            }
        }
        else if (a == "--transpose" && i + 1 < argc) {
            if (!parseInt(argv[++i], transpose)) {
                std::cerr << "错误: --transpose 需要整数（半音数，可为负）\n"; return 1;
            }
        }
        else if (a.substr(0, 2) != "--") input = a;
        else {
            std::cerr << "警告: 未知参数 " << a << "（用 --help 查看用法）\n";
            ++i;  // 跳过其可能的取值
        }
    }
    if (input.empty()) { printHelp(); return 1; }

    m2j::MidiData data;
    if (!m2j::loadMidiFile(input, data)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << input << "\n";
        return 2;
    }
    // 鼓轨的提示写在谱面头部（formatJianpu 里），这里只管「只剩鼓」这种致命情况
    if (data.notes.empty()) {
        if (data.drumNotesSkipped > 0) {
            std::cerr << "未提取到旋律音符：文件里只有打击乐（简谱不支持鼓谱）。\n";
        } else {
            std::cerr << "未提取到音符。\n";
        }
        return 3;
    }

    // --track 过滤指定轨道
    if (trackFilter >= 0) {
        std::vector<m2j::NoteEvent> filtered;
        for (auto& n : data.notes) if (n.track == trackFilter) filtered.push_back(n);
        data.notes = filtered;
        if (data.notes.empty()) { std::cerr << "轨道 " << trackFilter << " 无音符。\n"; return 4; }
    }

    // --transpose 移调
    if (transpose != 0) {
        for (auto& n : data.notes) n.pitch += transpose;
        std::cout << "移调: " << (transpose > 0 ? "+" : "") << transpose << " 半音\n";
    }

    // 时值量化：真实录音 MIDI 的 note-off 普遍提前，不量化会把八分判成附点十六
    if (quantize) {
        m2j::QuantizeReport qr = m2j::quantizeTiming(data);
        if (qr.adjusted > 0) {
            std::cout << "时值量化: 修正 " << qr.adjusted << " 个音符 (网格 "
                      << qr.gridTicks << " tick, 门限 " << qr.gate << ")\n";
        }
    }

    // 弱起：--pickup N 给的是「拍」，换算成 tick 写回数据；
    // --no-pickup 则明确关掉自动判定。两者都不给时由 detectPickupTicks 判。
    if (noPickup) {
        data.pickupTicks = 0;
    } else if (pickupBeats >= 0) {
        double beatLen = data.division * 4.0 / (double)(1 << data.timeSig.denomPow);
        if (!(beatLen > 0)) beatLen = data.division;
        data.pickupTicks = (long)std::llround(pickupBeats * beatLen);
    }
    {
        long pk = m2j::effectivePickupTicks(data);
        if (pk > 0) {
            std::cout << m2j::pickupLabel(data)
                      << (pickupBeats >= 0 ? "（手动指定）" : "（自动判定，如不对用 --pickup / --no-pickup 覆盖）")
                      << "\n";
        }
    }

    m2j::KeyEstimate key;
    std::string keySource;
    if (!keyArg.empty() && parseKey(keyArg, key)) {
        keySource = "(手动指定 " + keyArg + ")";
    } else {
        if (!keyArg.empty()) {
            std::cerr << "警告: 无法识别的调性 \"" << keyArg
                      << "\"，已改用自动识别（示例: C / Am / F# / Bb）\n";
        }
        key = m2j::estimateKey(data);
        keySource = data.keySig.valid ? "(MIDI调号)" : "(自动猜测)";
    }

    std::string jianpu = m2j::formatJianpu(data, key);
    std::string header = "\n共提取 " + std::to_string(data.notes.size())
              + " 个音符 (division=" + std::to_string(data.division)
              + ", 拍号=" + data.timeSig.toString() + ") " + keySource + "\n\n";
    std::cout << header << jianpu;

    if (!txtOut.empty()) {
        std::ofstream f(txtOut);
        f << header << jianpu;
        std::cout << "文本简谱已写入: " << txtOut << "\n";
    }
    if (!htmlOut.empty()) {
        std::ofstream f(htmlOut);
        std::string title = input;
        size_t slash = title.find_last_of("/\\");
        if (slash != std::string::npos) title = title.substr(slash + 1);
        size_t dot = title.find_last_of(".");
        if (dot != std::string::npos) title = title.substr(0, dot);
        std::vector<m2j::LyricLine> lyricLines;
        if (!lyricsPath.empty()) {
            size_t dot = lyricsPath.find_last_of(".");
            std::string ext = (dot != std::string::npos) ? lyricsPath.substr(dot) : "";
            if (ext == ".lrc" || ext == ".LRC") {
                int bpm = (int)(60000000.0 / data.tempo);
                lyricLines = m2j::parseLrcLines(lyricsPath, data.division, bpm);
            } else {
                // JSON格式: 转成行列表
                auto syls = m2j::parseLyrics(lyricsPath, data.division);
                // 简单处理: 每个字一行(实际应该按行合并)
                for (const auto& s : syls) lyricLines.push_back({s.tick, s.text});
            }
            std::cout << "歌词已加载: " << lyricLines.size() << " 行\n";
        }
        f << m2j::renderHtml(data, key, title, lyricLines.empty() ? nullptr : &lyricLines);
        std::cout << "HTML 谱面已写入: " << htmlOut << "\n";
    }
    if (!xmlOut.empty()) {
        std::ofstream f(xmlOut);
        f << m2j::renderMusicXml(data, key);
        std::cout << "MusicXML 已写入: " << xmlOut << "\n";
    }
    return 0;
}

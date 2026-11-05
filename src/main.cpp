// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

#include <fstream>
#include <iostream>
#include <string>

static void printHelp() {
    std::cout <<
        "midi2jianpu - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>                 打印文本简谱\n"
        "  midi2jianpu <input.mid> --html out.html 导出 HTML 谱面\n"
        "  midi2jianpu <input.mid> --key Am         手动指定调性(如 Am=小调, C=大调)\n"
        "  midi2jianpu <input.mid> --output out.txt  文本简谱输出到文件\n"
        "  midi2jianpu <input.mid> --track 0         只输出指定轨道\n"
        "  midi2jianpu <input.mid> --transpose +2     移调(半音, 正负都可)\n"
        "  midi2jianpu <input.mid> --lyrics lyrics.lrc  加载歌词\n"
        "  midi2jianpu --help\n";
}

// 解析 --key 参数: "C"=C大调, "Am"=a小调, "F#"=F#大调
static bool parseKey(const std::string& s, m2j::KeyEstimate& out) {
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    size_t i = 0;
    std::string name;
    if (s.size() >= 2 && (s[1] == '#' || s[1] == 'b')) {
        name = s.substr(0, 2); i = 2;
    } else {
        name = s.substr(0, 1); i = 1;
    }
    int pc = -1;
    for (int k = 0; k < 12; k++) if (names[k] == name) { pc = k; break; }
    if (pc < 0) return false;
    out.tonicPitchClass = pc;
    out.isMajor = !(i < s.size() && (s[i] == 'm' || s[i] == 'M'));
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { printHelp(); return 1; }

    std::string input, htmlOut, keyArg, txtOut, xmlOut, lyricsPath;
    int trackFilter = -1;
    int transpose = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--html" && i + 1 < argc) { htmlOut = argv[++i]; }
        else if (a == "--key" && i + 1 < argc) { keyArg = argv[++i]; }
        else if (a == "--output" && i + 1 < argc) { txtOut = argv[++i]; }
        else if (a == "--musicxml" && i + 1 < argc) { xmlOut = argv[++i]; }
        else if (a == "--lyrics" && i + 1 < argc) { lyricsPath = argv[++i]; }
        else if (a == "--track" && i + 1 < argc) { trackFilter = std::stoi(argv[++i]); }
        else if (a == "--transpose" && i + 1 < argc) { transpose = std::stoi(argv[++i]); }
        else if (a.substr(0, 2) != "--") input = a;
    }
    if (input.empty()) { printHelp(); return 1; }

    m2j::MidiData data;
    if (!m2j::loadMidiFile(input, data)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << input << "\n";
        return 2;
    }
    if (data.notes.empty()) { std::cerr << "未提取到音符。\n"; return 3; }

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

    m2j::KeyEstimate key;
    std::string keySource;
    if (!keyArg.empty() && parseKey(keyArg, key)) {
        keySource = "(手动指定 " + keyArg + ")";
    } else {
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

// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"

#include <fstream>
#include <iostream>
#include <string>

static void printHelp() {
    std::cout <<
        "midi2jianpu - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>                 打印文本简谱\n"
        "  midi2jianpu <input.mid> --html out.html 导出 HTML 谱面\n"
        "  midi2jianpu --help\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { printHelp(); return 1; }

    std::string input;
    std::string htmlOut;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--html" && i + 1 < argc) { htmlOut = argv[++i]; }
        else if (a.substr(0, 2) != "--") input = a;
    }
    if (input.empty()) { printHelp(); return 1; }

    m2j::MidiData data;
    if (!m2j::loadMidiFile(input, data)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << input << "\n";
        return 2;
    }
    if (data.notes.empty()) { std::cerr << "未提取到音符。\n"; return 3; }

    m2j::KeyEstimate key = m2j::estimateKey(data);
    std::string keySource = data.keySig.valid ? "(MIDI调号)" : "(自动猜测)";
    std::cout << "\n共提取 " << data.notes.size()
              << " 个音符 (division=" << data.division
              << ", 拍号=" << data.timeSig.toString() << ") " << keySource << "\n\n";
    std::cout << m2j::formatJianpu(data, key);

    if (!htmlOut.empty()) {
        std::ofstream f(htmlOut);
        f << m2j::renderHtml(data, key);
        std::cout << "\nHTML 谱面已写入: " << htmlOut << "\n";
    }
    return 0;
}

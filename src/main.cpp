// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"

#include <iostream>
#include <string>

static void printHelp() {
    std::cout <<
        "midi2jianpu - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>          转换并打印简谱\n"
        "  midi2jianpu --help               显示帮助\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printHelp();
        return 1;
    }
    std::string arg = argv[1];
    if (arg == "--help" || arg == "-h") {
        printHelp();
        return 0;
    }

    m2j::MidiData data;
    if (!m2j::loadMidiFile(arg, data)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << arg << "\n";
        return 2;
    }
    if (data.notes.empty()) {
        std::cerr << "未提取到音符。\n";
        return 3;
    }

    m2j::KeyEstimate key = m2j::estimateKey(data.notes);
    std::cout << "\n共提取 " << data.notes.size()
              << " 个音符 (division=" << data.division << ")\n\n";
    std::cout << m2j::formatJianpu(data.notes, key);
    return 0;
}

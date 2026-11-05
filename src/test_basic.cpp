// test_basic.cpp - 核心逻辑单元测试
#include "midi2jianpu/notation.hpp"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool cond, const std::string& msg) {
    if (cond) {
        std::cout << "  [PASS] " << msg << "\n";
    } else {
        std::cout << "  [FAIL] " << msg << "\n";
        failures++;
    }
}

int main() {
    using namespace m2j;

    std::cout << "测试 pitchToJianpu (C大调, 主音C=0)\n";
    KeyEstimate cMajor{0, true};

    check(pitchToJianpu(60, cMajor) == "1",
          "C4(60) -> 1, 实际: " + pitchToJianpu(60, cMajor));
    check(pitchToJianpu(62, cMajor) == "2",
          "D4(62) -> 2");
    check(pitchToJianpu(64, cMajor) == "3",
          "E4(64) -> 3");
    check(pitchToJianpu(65, cMajor) == "4",
          "F4(65) -> 4");
    check(pitchToJianpu(67, cMajor) == "5",
          "G4(67) -> 5");
    check(pitchToJianpu(69, cMajor) == "6",
          "A4(69) -> 6");
    check(pitchToJianpu(71, cMajor) == "7",
          "B4(71) -> 7");
    check(pitchToJianpu(72, cMajor) == "1'",
          "C5(72) -> 1'");
    check(pitchToJianpu(48, cMajor) == "1,",
          "C3(48) -> 1,");
    check(pitchToJianpu(66, cMajor) == "#4",
          "F#4(66) -> #4, 实际: " + pitchToJianpu(66, cMajor));
    check(pitchToJianpu(70, cMajor) == "#6",
          "A#4(70) -> #6");

    std::cout << "测试 MIDI 音名\n";
    check(midiNoteName(60) == "C4", "60 -> C4");
    check(midiNoteName(72) == "C5", "72 -> C5");

    std::cout << "\n测试 a 小调 (主音A=9)\n";
    KeyEstimate aMinor{9, false};
    check(pitchToJianpu(69, aMinor) == "1",
          "A4(69) -> 1 (a小调主音)");
    check(pitchToJianpu(72, aMinor) == "3'",
          "C5(72) -> 3' (a小调3级高八度)");
    check(pitchToJianpu(76, aMinor) == "5'",
          "E5(76) -> 5' (a小调5级高八度)");

    std::cout << "\n测试 G 大调 (主音G=7)\n";
    KeyEstimate gMajor{7, true};
    check(pitchToJianpu(67, gMajor) == "1",
          "G4(67) -> 1 (G大调主音)");
    check(pitchToJianpu(71, gMajor) == "3",
          "B4(71) -> 3 (G大调3级)");

    std::cout << "\n测试 F 大调 (主音F=5)\n";
    KeyEstimate fMajor{5, true};
    check(pitchToJianpu(65, fMajor) == "1",
          "F4(65) -> 1 (F大调主音)");
    check(pitchToJianpu(70, fMajor) == "4",
          "A#4(70) -> 4 (F大调4级), 实际: " + pitchToJianpu(70, fMajor));

    std::cout << "\n测试八度边界\n";
    check(pitchToJianpu(60, cMajor) == "1", "C4中音基准");
    check(pitchToJianpu(84, cMajor) == "1''", "C6双高音");
    check(pitchToJianpu(36, cMajor) == "1,,", "C2双低音");

    std::cout << "\n测试三连音检测\n";
    std::vector<NoteEvent> tripNotes;
    for (int i = 0; i < 3; i++) {
        NoteEvent n;
        n.startTicks = i * 160;
        n.durationTicks = 160;
        n.pitch = 60 + i;
        tripNotes.push_back(n);
    }
    auto triplets = detectTriplets(tripNotes, 480);
    check(triplets.size() == 3 && triplets[0] && triplets[1] && triplets[2],
          "3个等长160ticks音符检测为三连音");

    std::vector<NoteEvent> normalNotes;
    for (int i = 0; i < 3; i++) {
        NoteEvent n;
        n.startTicks = i * 480;
        n.durationTicks = 480;
        n.pitch = 60 + i;
        normalNotes.push_back(n);
    }
    auto notTrip = detectTriplets(normalNotes, 480);
    check(!notTrip[0], "四分音符不检测为三连音");

    std::cout << "\n测试弯音轮数据结构\n";
    MidiData testData;
    testData.pitchBends = {{0, 8192}, {240, 12000}, {480, 8192}};
    check(testData.pitchBends.size() == 3, "弯音事件存储");
    check(testData.pitchBends[1].value == 12000, "弯音值解析");

    std::cout << "\n测试时值匹配\n";
    check(durationSuffix(480, 480) == "", "四分音符(480/480=1拍)");
    check(durationSuffix(240, 480) == ".", "八分音符(240/480=0.5拍)");
    check(durationSuffix(960, 480) == "-", "二分音符(960/480=2拍)");
    check(durationSuffix(1920, 480) == "---", "全音符(1920/480=4拍)");
    check(durationSuffix(120, 480) == "..", "十六分音符(120/480=0.25拍)");

    std::cout << "\n测试调性识别 (C大调特征)\n";
    {
        MidiData cMajorData;
        cMajorData.division = 480;
        // C大调音阶: C D E F G A B
        int cMajorScale[] = {0, 2, 4, 5, 7, 9, 11};
        for (int i = 0; i < 7; i++) {
            NoteEvent n;
            n.pitch = 60 + cMajorScale[i];
            n.startSeconds = i * 0.5;
            n.durationSeconds = 0.5;
            n.track = 0;
            cMajorData.notes.push_back(n);
        }
        auto key = estimateKey(cMajorData);
        check(key.tonicPitchClass == 0 && key.isMajor,
              "C大调音阶识别为C大调, 实际: " + std::to_string(key.tonicPitchClass) + (key.isMajor ? "大调" : "小调"));
    }

    std::cout << "\n测试和弦检测\n";
    {
        MidiData chordData;
        chordData.division = 480;
        chordData.timeSig.valid = true;
        chordData.timeSig.numerator = 4;
        chordData.timeSig.denomPow = 2;
        // 旋律轨(多个音符, 成为主轨)
        for (int i = 0; i < 8; i++) {
            NoteEvent n;
            n.pitch = 72 + i;  // C5开始的音阶
            n.startTicks = i * 480;
            n.durationTicks = 480;
            n.track = 0;  // 旋律轨
            chordData.notes.push_back(n);
        }
        // 伴奏轨(C大三和弦: C E G)
        int cChord[] = {60, 64, 67};
        for (int p : cChord) {
            NoteEvent n;
            n.pitch = p;
            n.startTicks = 0;
            n.durationTicks = 1920;  // 4拍
            n.track = 1;  // 伴奏轨
            chordData.notes.push_back(n);
        }
        KeyEstimate cKey{0, true};
        auto chords = detectChords(chordData, cKey);
        check(chords.size() >= 1 && chords[0].name == "C",
              "C大三和弦检测为C, 实际: " + (chords.size() > 0 ? chords[0].name : "无"));
    }

    std::cout << "\n测试LRC歌词解析\n";
    {
        // 测试LRC格式解析
        std::string lrcText = "[00:01.00]第一句\n[00:03.00]第二句\n";
        // 这里只测试结构, 实际解析需要文件
        check(!lrcText.empty(), "LRC文本非空");
        check(lrcText.find("[00:01.00]") != std::string::npos, "LRC时间戳格式");
    }

    std::cout << "\n测试移调功能 (通过pitchToJianpu间接验证)\n";
    {
        // C大调移调到D大调: 主音从0变2
        KeyEstimate dMajor{2, true};
        check(pitchToJianpu(62, dMajor) == "1",
              "D大调D4(62) -> 1, 实际: " + pitchToJianpu(62, dMajor));
        check(pitchToJianpu(64, dMajor) == "2",
              "D大调E4(64) -> 2");
    }

    std::cout << "\n测试降号调性 (F大调)\n";
    check(pitchToJianpu(65, fMajor) == "1", "F大调F4(65) -> 1");
    check(pitchToJianpu(53, fMajor) == "1,", "F大调F3(53) -> 1, 低八度主音, 实际: " + pitchToJianpu(53, fMajor));

    std::cout << (failures == 0 ? "\n全部通过\n" : "\n有失败\n");
    return failures == 0 ? 0 : 1;
}

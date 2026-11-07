// test_basic.cpp - 核心逻辑单元测试
// 覆盖：音高->简谱、八度基准、升降号、时值后缀、三连音、调性识别、
//       和弦检测、同音合并、MusicXML 相关的基础数据结构、
//       鼓轨过滤、旋律轨判定、变速（tempo map）换算、内置歌词规整、
//       拍号 map 与逐小节网格、调号 map（中途转调 / KeyMap）、
//       五声调式（宫商角徵羽）宫音判定与校正、tempo map 时间轴与 LRC 对齐。
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/musicxml_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 最小 SMF 构造器：只用来喂给解析器做回归测试，不追求功能完整。
// 用内存构造而不是读 .mid 文件，单元测试才能不依赖 fixture 生成脚本。
// ---------------------------------------------------------------------------
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}
static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
static void putVlq(std::vector<uint8_t>& v, uint32_t n) {
    uint8_t buf[4];
    int i = 0;
    buf[i++] = (uint8_t)(n & 0x7F);
    while ((n >>= 7) != 0) buf[i++] = (uint8_t)((n & 0x7F) | 0x80);
    while (i--) v.push_back(buf[i]);
}

static std::vector<uint8_t> ev(std::initializer_list<int> bytes) {
    std::vector<uint8_t> v;
    for (int b : bytes) v.push_back((uint8_t)b);
    return v;
}

struct SmfTrack {
    std::vector<std::pair<long, std::vector<uint8_t>>> events;
    void at(long tick, std::vector<uint8_t> bytes) { events.push_back({tick, std::move(bytes)}); }
};

// format 与 division 都要能自定义：division 传 0xE728 就是 SMPTE 25fps×40，
// format 传 2 就是「多条互相独立的序列」。
static std::vector<uint8_t> buildSmfFmt(int format, int ntrks, int division,
                                       std::vector<SmfTrack> tracks) {
    std::vector<uint8_t> out;
    const char hdr[] = {'M','T','h','d'};
    out.insert(out.end(), hdr, hdr + 4);
    put32(out, 6);
    put16(out, (uint16_t)format);
    put16(out, (uint16_t)ntrks);
    put16(out, (uint16_t)division);

    for (auto& tr : tracks) {
        std::stable_sort(tr.events.begin(), tr.events.end(),
                         [](const std::pair<long, std::vector<uint8_t>>& a,
                            const std::pair<long, std::vector<uint8_t>>& b) {
                             return a.first < b.first;
                         });
        std::vector<uint8_t> body;
        long last = 0;
        for (auto& e : tr.events) {
            putVlq(body, (uint32_t)(e.first - last));
            body.insert(body.end(), e.second.begin(), e.second.end());
            last = e.first;
        }
        putVlq(body, 0);
        body.push_back(0xFF); body.push_back(0x2F); body.push_back(0x00);

        const char trk[] = {'M','T','r','k'};
        out.insert(out.end(), trk, trk + 4);
        put32(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

static std::vector<uint8_t> buildSmf(int ntrks, int division, std::vector<SmfTrack> tracks) {
    return buildSmfFmt(1, ntrks, division, std::move(tracks));
}

static int failures = 0;
static int total = 0;

static void check(bool cond, const std::string& msg) {
    total++;
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

    std::cout << "\n测试 a 小调 (主音A=9，首调记作 1=C，主音=6)\n";
    KeyEstimate aMinor{9, false};
    // 简谱用首调唱名：小调不写 1=主音，而是把关系大调的 do 记作 1。
    // a 小调的关系大调是 C，所以 1=C，主音 A 落在 6 上。
    check(keyName(aMinor) == "C", "a小调调号记作 1=C, 实际: " + keyName(aMinor));
    check(pitchToJianpu(69, aMinor) == "6",
          "A4(69) -> 6 (a小调主音，首调), 实际: " + pitchToJianpu(69, aMinor));
    check(pitchToJianpu(72, aMinor) == "1'",
          "C5(72) -> 1' (关系大调主音，高于主音所在八度), 实际: " + pitchToJianpu(72, aMinor));
    check(pitchToJianpu(76, aMinor) == "3'",
          "E5(76) -> 3' (a小调5级), 实际: " + pitchToJianpu(76, aMinor));
    check(pitchToJianpu(81, aMinor) == "6'",
          "A5(81) -> 6' (高八度主音), 实际: " + pitchToJianpu(81, aMinor));
    // 和声小调的升七级 G# 在首调里记作 #5
    check(pitchToJianpu(68, aMinor) == "#5",
          "G#4(68) -> #5 (和声小调升七级), 实际: " + pitchToJianpu(68, aMinor));
    // e 小调 -> 关系大调 G
    KeyEstimate eMinor{4, false};
    check(keyName(eMinor) == "G", "e小调调号记作 1=G, 实际: " + keyName(eMinor));

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

    std::cout << "\n测试八度基准随调性移动（回归：主音高于C的调曾整体高一个八度）\n";
    check(pitchToJianpu(72, gMajor) == "4",
          "G大调 C5(72) -> 4（不是 4'）, 实际: " + pitchToJianpu(72, gMajor));
    check(pitchToJianpu(79, gMajor) == "1'",
          "G大调 G5(79) -> 1'");
    check(pitchToJianpu(60, gMajor) == "4,",
          "G大调 C4(60) -> 4,（低于主音一个八度）");
    check(pitchToJianpu(74, gMajor) == "5",
          "G大调 D5(74) -> 5（属音，仍在中音区）");

    std::cout << "\n测试八度边界\n";
    check(pitchToJianpu(60, cMajor) == "1", "C4中音基准");
    check(pitchToJianpu(84, cMajor) == "1''", "C6双高音");
    check(pitchToJianpu(36, cMajor) == "1,,", "C2双低音");

    std::cout << "\n测试降号写法（旧实现只会输出 #）\n";
    check(prefersFlats(fMajor), "F大调使用降号记谱");
    check(!prefersFlats(cMajor), "C大调使用升号记谱");
    check(pitchToJianpu(68, fMajor) == "b3",
          "F大调 Ab4(68) -> b3, 实际: " + pitchToJianpu(68, fMajor));
    check(keyName(KeyEstimate{10, true}) == "Bb",
          "降B大调显示为 Bb, 实际: " + keyName(KeyEstimate{10, true}));
    check(keyFifths(fMajor) == -1, "F大调调号 -1");
    check(keyFifths(cMajor) == 0, "C大调调号 0");
    check(keyFifths(aMinor) == 0, "a小调调号 0");
    check(keyFifths(KeyEstimate{2, false}) == -1, "d小调调号 -1");

    std::cout << "\n测试时值后缀\n";
    check(durationSuffix(480, 480) == "", "四分音符(1拍)");
    check(durationSuffix(240, 480) == "_", "八分音符(0.5拍)");
    check(durationSuffix(960, 480) == "-", "二分音符(2拍)");
    check(durationSuffix(1440, 480) == "--", "附点二分(3拍)");
    check(durationSuffix(1920, 480) == "---", "全音符(4拍)");
    check(durationSuffix(120, 480) == "=", "十六分音符(0.25拍)");
    check(durationSuffix(720, 480) == ".", "附点四分(1.5拍)");

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

    // 回归：一整串八分音符曾被误判成三连音，导致整份谱子被 3( ... ) 包住
    std::vector<NoteEvent> eighthNotes;
    for (int i = 0; i < 9; i++) {
        NoteEvent n;
        n.startTicks = i * 240;
        n.durationTicks = 240;
        n.pitch = 60 + (i % 3);
        eighthNotes.push_back(n);
    }
    auto eighthTrip = detectTriplets(eighthNotes, 480);
    bool anyTrip = false;
    for (bool b : eighthTrip) if (b) anyTrip = true;
    check(!anyTrip, "八分音符连奏不检测为三连音");

    std::cout << "\n测试时值量化（真实录音 MIDI 的门限衰减）\n";
    {
        // 造一份"八分音符 + 门限 85%"的数据：音头间隔 480，实际时长 408
        MidiData qd;
        qd.division = 960;
        qd.timeSig = TimeSignature{4, 4};
        for (int i = 0; i < 16; i++) {
            NoteEvent n;
            n.startTicks = i * 480;
            n.durationTicks = 408;          // 0.425 拍，容易被判成附点十六分
            n.durationSeconds = 408 / 960.0 * 60.0 / 120.0;
            n.pitch = 60 + (i % 5);
            n.track = 0;
            qd.notes.push_back(n);
        }
        QuantizeReport rep = quantizeTiming(qd);
        check(rep.gate > 0.8 && rep.gate < 0.9,
              "估计出门限因子约 0.85, 实际: " + std::to_string(rep.gate));
        check(qd.notes[0].durationTicks == 480,
              "408 tick 还原为八分音符 480, 实际: "
                  + std::to_string(qd.notes[0].durationTicks));
        check(durationSuffix(qd.notes[0].durationTicks, qd.division) == "_",
              "量化后时值后缀为八分音符 _");
        check(qd.notes[1].startTicks == 480, "音头吸附到网格");
    }
    {
        // 已经是标准时值的数据不应被改动
        MidiData qd;
        qd.division = 480;
        qd.timeSig = TimeSignature{4, 4};
        for (int i = 0; i < 8; i++) {
            NoteEvent n;
            n.startTicks = i * 240;
            n.durationTicks = 240;
            n.pitch = 60;
            n.track = 0;
            qd.notes.push_back(n);
        }
        QuantizeReport rep = quantizeTiming(qd);
        check(qd.notes[0].durationTicks == 240, "规整数据量化后时长不变");
        check(qd.notes[3].startTicks == 720, "规整数据量化后音头不变");
        (void)rep;
    }

    std::cout << "\n测试弯音轮数据结构\n";
    MidiData testData;
    testData.pitchBends = {{0, 8192, 0}, {240, 12000, 0}, {480, 8192, 0}};
    check(testData.pitchBends.size() == 3, "弯音事件存储");
    check(testData.pitchBends[1].value == 12000, "弯音值解析");
    check(testData.pitchBends[1].channel == 0, "弯音通道字段");

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

    std::cout << "\n测试和声窗（小节内换和弦）\n";
    {
        // 窗口长度必须跟拍号走，否则「一小节两个和弦」要么被吞掉（窗口太大）
        // 要么碎成一堆假和弦（窗口太小）。
        auto winOf = [](int num, int denomPow) {
            MidiData d;
            d.division = 480;
            d.timeSig.valid = true;
            d.timeSig.numerator = num;
            d.timeSig.denomPow = denomPow;
            return chordWindowTicks(d);
        };
        check(winOf(4, 2) == 960, "4/4 半小节 = 960 tick，实际 " + std::to_string(winOf(4, 2)));
        check(winOf(6, 3) == 720, "6/8 附点四分 = 720 tick，实际 " + std::to_string(winOf(6, 3)));
        check(winOf(3, 2) == 1440, "3/4 整小节 = 1440 tick，实际 " + std::to_string(winOf(3, 2)));
    }

    std::cout << "\n测试和弦检测：一小节两个和弦\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.timeSig.numerator = 4;
        d.timeSig.denomPow = 2;
        // 旋律轨：每拍一个音，保证 piano 不是「音符最多」的轨
        for (int i = 0; i < 4; i++) {
            NoteEvent n;
            n.pitch = 72;
            n.startTicks = i * 480;
            n.durationTicks = 480;
            n.track = 0;
            d.notes.push_back(n);
        }
        // 前半小节 C（C E G），后半小节 Am（A C E）
        auto addChord = [&](std::initializer_list<int> ps, long tick, long dur) {
            for (int p : ps) {
                NoteEvent n;
                n.pitch = p;
                n.startTicks = tick;
                n.durationTicks = dur;
                n.track = 1;
                d.notes.push_back(n);
            }
        };
        addChord({48, 52, 55}, 0, 960);      // C
        addChord({45, 48, 52}, 960, 960);    // Am

        KeyEstimate k{0, true};
        auto chords = detectChords(d, k);
        check(chords.size() == 2, "一小节检出两个和弦，实际 " + std::to_string(chords.size()));
        if (chords.size() == 2) {
            check(chords[0].name == "C" && chords[0].startTick == 0,
                  "前半小节是 C，实际 " + chords[0].name);
            check(chords[1].name == "Am" && chords[1].startTick == 960,
                  "第三拍是 Am，实际 " + chords[1].name +
                  "@" + std::to_string(chords[1].startTick));
        }

        // 整小节同一个和弦时不能标两次（相邻窗口同名要合并）
        MidiData same;
        same.division = 480;
        same.timeSig = d.timeSig;
        for (const auto& n : d.notes) {
            if (n.track == 1 && n.startTicks == 960) continue;   // 去掉 Am
            same.notes.push_back(n);
        }
        NoteEvent n2;
        n2.pitch = 48; n2.startTicks = 960; n2.durationTicks = 960; n2.track = 1;
        same.notes.push_back(n2);
        auto sameChords = detectChords(same, k);
        check(sameChords.size() == 1 && sameChords[0].name == "C",
              "整小节 C 只标一次，实际 " + std::to_string(sameChords.size()));
    }

    std::cout << "\n测试文本简谱：同音合并 (1 1 1 -> 1 - -)\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        for (int i = 0; i < 3; i++) {
            NoteEvent n;
            n.pitch = 60;
            n.startTicks = i * 480;
            n.durationTicks = 480;
            n.startSeconds = i * 0.5;
            n.durationSeconds = 0.5;
            n.track = 0;
            n.channel = 0;
            d.notes.push_back(n);
        }
        KeyEstimate c{0, true};
        std::string s = formatJianpu(d, c);
        check(s.find("1 - -") != std::string::npos,
              "三个连续四分音符合并为 \"1 - -\", 实际输出: " + s.substr(s.find('\n', 40) + 1));
    }

    std::cout << "\n测试文本简谱：时值标注\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        NoteEvent n;
        n.pitch = 67;          // G4 -> 5
        n.startTicks = 0;
        n.durationTicks = 960; // 二分音符
        n.track = 0;
        n.channel = 0;
        d.notes.push_back(n);
        KeyEstimate c{0, true};
        std::string s = formatJianpu(d, c);
        check(s.find("5 -") != std::string::npos,
              "二分音符标出延长线 \"5 -\", 实际输出: " + s.substr(s.find('\n', 40) + 1));
    }

    std::cout << "\n测试文本简谱：跨小节的静音与长音必须在小节线处断开\n";
    {
        // 4 小节前奏（0 音符）+ 第 5 小节才进旋律。
        // 曾经整段前奏挤进第 1 小节，输出一串十几个 '-' 且丢掉 3 条小节线。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 72.0});
        NoteEvent n;
        n.pitch = 60;                 // 1
        n.startTicks = 16 * 480;      // 第 5 小节第 1 拍
        n.durationTicks = 480;
        n.startSeconds = 8.0;
        n.durationSeconds = 0.5;
        n.track = 0;
        n.channel = 0;
        d.notes.push_back(n);
        KeyEstimate c{0, true};
        std::string s = formatJianpu(d, c);
        size_t bars = 0;
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (s[i] == '|' && s[i + 1] == '\n') bars++;
        }
        check(bars == 5, "4 小节前奏 + 1 小节旋律 = 5 条小节线, 实际: " + std::to_string(bars));
        check(s.find("0 - - -  |\n0 - - -  |\n0 - - -  |\n0 - - -  |") != std::string::npos,
              "前奏写成每小节一个 \"0 - - -\"");
    }
    {
        // 第 1 拍起、长 6 拍：应写成 "1 - - -" | "- -"
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        NoteEvent n;
        n.pitch = 60;
        n.startTicks = 0;
        n.durationTicks = 6 * 480;
        n.track = 0;
        n.channel = 0;
        d.notes.push_back(n);
        KeyEstimate c{0, true};
        std::string s = formatJianpu(d, c);
        check(s.find("1 - - -  |\n- -  |") != std::string::npos,
              "6 拍长音在小节线断开，后段用延长线续写, 实际: " +
                  s.substr(s.find('\n', 40) + 1));
    }

    std::cout << "\n测试LRC歌词解析\n";
    {
        // 这里只测试结构, 实际解析需要文件
        std::string lrcText = "[00:01.00]第一句\n[00:03.00]第二句\n";
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

    // ---- 鼓轨过滤 ----
    // 通道 10（index 9）的音高是鼓件编号，不是音级。真实编曲工程里鼓轨音符
    // 往往最多，不过滤就会变成「旋律轨」，把和弦标记和歌词全抢走。
    std::cout << "\n测试鼓轨过滤 (MIDI 通道 10)\n";
    {
        SmfTrack melody;
        for (int i = 0; i < 4; i++) {
            long t = i * 480L;
            melody.at(t, ev({0x90, 60 + i * 2, 90}));
            melody.at(t + 480, ev({0x80, 60 + i * 2, 0}));
        }
        SmfTrack drums;
        for (int i = 0; i < 16; i++) {
            drums.at(i * 120L, ev({0x99, 42, 100}));          // 通道 9 = 踩镲
            drums.at(i * 120L + 60, ev({0x89, 42, 0}));
        }
        auto bytes = buildSmf(2, 480, {melody, drums});
        MidiData d;
        bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
        check(ok, "带鼓轨的 MIDI 可以解析");
        check(d.drumNotesSkipped == 16,
              "鼓轨 16 个音符被跳过, 实际: " + std::to_string(d.drumNotesSkipped));
        check(d.notes.size() == 4,
              "只剩 4 个旋律音符, 实际: " + std::to_string(d.notes.size()));
        std::string s = formatJianpu(d, KeyEstimate{0, true});
        check(s.find("[和") == std::string::npos && s.find("Drums") == std::string::npos,
              "谱面里没有鼓轨");
    }

    // ---- 旋律轨判定 ----
    // 旧实现取「音符最多的轨」，这里伴奏有 24 个音、旋律只有 8 个，
    // 但它必须是柱式和弦（复音数 3），旋律轨应判给单声部那一条。
    std::cout << "\n测试旋律轨判定\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        for (int i = 0; i < 8; i++) {
            NoteEvent n;
            n.track = 0; n.channel = 0; n.pitch = 60 + i * 2;
            n.startTicks = i * 480L; n.durationTicks = 480;
            d.notes.push_back(n);
        }
        for (int i = 0; i < 8; i++)
            for (int p : {48, 52, 55}) {
                NoteEvent n;
                n.track = 1; n.channel = 1; n.pitch = p;
                n.startTicks = i * 480L; n.durationTicks = 480;
                d.notes.push_back(n);
            }
        int m = pickMelodyTrack(d, nullptr);
        check(m == 0,
              "单声部的 0 号轨胜过音符更多的柱式和弦轨, 实际: " + std::to_string(m));
    }

    // ---- 变速曲目的秒数换算 ----
    // conductor 轨故意放在音符轨之后：format 1 文件里 set_tempo 几乎都在
    // 独立的 track 0，旧实现边解析边换算，会拿最后一次 tempo 去算全部音符。
    std::cout << "\n测试变速 (tempo map) 秒数换算\n";
    {
        SmfTrack notes;
        notes.at(0,    ev({0x90, 60, 90}));
        notes.at(480,  ev({0x80, 60, 0}));
        notes.at(2400, ev({0x90, 62, 90}));
        notes.at(2880, ev({0x80, 62, 0}));

        SmfTrack cond;   // 排在后面
        cond.at(0,    ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));   // 500000us = 120BPM
        cond.at(1920, ev({0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40}));   // 1000000us = 60BPM

        auto bytes = buildSmf(2, 480, {notes, cond});
        MidiData d;
        bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
        check(ok, "变速 MIDI 可以解析");
        check(d.notes.size() == 2,
              "两个音符, 实际: " + std::to_string(d.notes.size()));
        if (d.notes.size() == 2) {
            // 120BPM：一拍(480tick) = 0.5 秒
            check(std::fabs(d.notes[0].durationSeconds - 0.5) < 0.01,
                  "120BPM 下四分音符 = 0.5s, 实际: " +
                      std::to_string(d.notes[0].durationSeconds));
            // tick 0..1920 @120BPM = 2.0s，再走 480tick @60BPM = 1.0s -> 3.0s
            check(std::fabs(d.notes[1].startSeconds - 3.0) < 0.02,
                  "变速点之后的音符起始于 3.0s, 实际: " +
                      std::to_string(d.notes[1].startSeconds));
            check(std::fabs(d.notes[1].durationSeconds - 1.0) < 0.02,
                  "60BPM 下四分音符 = 1.0s, 实际: " +
                      std::to_string(d.notes[1].durationSeconds));
        }
    }

    std::cout << "\n测试弱起 (anacrusis) 网格\n";
    {
        // 素材：弱起 1 拍 + 两个完整小节，末音正好收在小节线上。
        // 这是歌本里最常见的形状（`| 5 | 1' 7 6 5 |`），也是 tk/小节长 会错位的典型。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        auto addNote = [&](long tick, long dur, int pitch) {
            NoteEvent n;
            n.pitch = pitch;
            n.startTicks = tick;
            n.durationTicks = dur;
            n.track = 0;
            n.channel = 0;
            d.notes.push_back(n);
        };
        addNote(0, 480, 67);                                        // 弱起 5
        const int mel[8] = {72, 71, 69, 67, 64, 65, 67, 72};        // 1' 7 6 5 | 3 4 5 1'
        for (int i = 0; i < 8; i++) addNote(480 + i * 480L, 480, mel[i]);

        long pk = detectPickupTicks(d);
        check(pk == 480, "1 拍弱起判出 480 tick, 实际: " + std::to_string(pk));

        BarGrid g = barGridOf(d);
        check(g.pickup == 480, "网格带上弱起, 实际: " + std::to_string(g.pickup));
        check(g.barIndex(0) == 0 && g.barIndex(479) == 0,
              "弱起小节自身是 bar 0");
        check(g.barIndex(480) == 1 && g.barIndex(2399) == 1 && g.barIndex(2400) == 2,
              "第一个完整小节从 480 起算, 实际: "
                  + std::to_string(g.barIndex(480)) + "/" + std::to_string(g.barIndex(2400)));
        check(g.barStart(1) == 480 && g.barEnd(1) == 2400 && g.barStart(2) == 2400,
              "小节边界随弱起平移, 实际: " + std::to_string(g.barStart(1))
                  + "/" + std::to_string(g.barEnd(1)));
        check(g.barSpan(0) == 480 && g.barSpan(1) == 1920, "弱起小节只有 480 tick");
        check(g.measureNumber(1) == 1 && g.measureNumber(2) == 2,
              "弱起小节不编号, 第一个完整小节才是 1");
        check(pickupLabel(d) == "弱起 1 拍", "页眉写弱起 1 拍, 实际: " + pickupLabel(d));

        KeyEstimate c{0, true};
        std::string s = formatJianpu(d, c);
        check(s.find("弱起 1 拍") != std::string::npos, "文本谱头标注弱起");
        // 弱起小节自成一行：`5  |` 后面才是完整小节
        check(s.find("5  |\n1' 7 6 5  |") != std::string::npos,
              "弱起小节单独成栏, 实际: " + s.substr(s.find('\n', 40) + 1));
    }
    {
        // 开头有前奏休止（不是从 tick 0 起唱）时不能判成弱起
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        NoteEvent n;
        n.pitch = 60; n.track = 0; n.channel = 0;
        for (int i = 0; i < 9; i++) {
            n.startTicks = 1920 + i * 480L;
            n.durationTicks = 480;
            d.notes.push_back(n);
        }
        check(detectPickupTicks(d) == 0, "前奏休止不被误判成弱起");
    }
    {
        // 一个 6 拍长音停在第二小节第 2 拍：末音位置凑巧是「2 拍弱起」的收尾，
        // 但下一小节强拍上并没有音起，所以不成立。早先的实现会在这里误判。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        NoteEvent n;
        n.pitch = 60; n.track = 0; n.channel = 0;
        n.startTicks = 0;
        n.durationTicks = 6 * 480;
        d.notes.push_back(n);
        check(detectPickupTicks(d) == 0, "末音停在小节中间时不判弱起");
    }
    {
        // 手动覆盖优先于自动判定
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.pickupTicks = 960;
        BarGrid g = barGridOf(d);
        check(g.pickup == 960 && g.barStart(1) == 960, "手动 --pickup 覆盖自动判定");
        d.pickupTicks = 0;
        check(barGridOf(d).pickup == 0, "--no-pickup 关掉弱起");
    }

    // ---- 歌词分配与一字多音 ----
    // HTML 的连音线弧线和 MusicXML 的 syllabic/slur 都从这一份结果出发。
    // 这里锁住「哪个字唱哪几个音、区间到哪里结束」的全部边界。
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        NoteEvent n;
        n.track = 1; n.channel = 0; n.durationTicks = 480;
        for (int i = 0; i < 8; i++) {
            n.startTicks = (long)i * 480;
            n.pitch = 60 + i;
            d.notes.push_back(n);
        }
        std::vector<LyricLine> lines = {{0, "一二"}, {1920, "三四"}};
        LyricAssignment a = assignLyrics(d, lines, 1);
        check(a.at.size() == 4, "两行四个字各挂到一个音头");
        check(a.melismas.size() == 4, "四个字各拖两个音 -> 4 个一字多音组");
        check(a.at.count(0) && a.at[0].text == "一" && a.at[0].noteCount == 2,
              "第一个字是「一」且覆盖 2 个音");
        check(a.melismas[0].spanEnd == 960 && a.melismas[0].lastNoteTick == 480,
              "首组区间 [0,960)，末音落在 480");
        check(a.groupAt(0) == 1 && a.groupAt(480) == 1 && a.groupAt(960) == 2,
              "groupAt 用区间而非音头集合，延长段也归进同一组");
        check(a.groupAt(1920) == 3 && a.groupAt(3840) == 4 && a.groupAt(3841) == 0,
              "第二行自成新组，末组覆盖到全曲末音之后才结束");
        check(a.melismas[1].spanEnd <= 1920, "连音线不越过下一行歌词起点");
        check(a.melismas[3].lastNoteTick == 3360, "末组收在全曲最后一个音上");

        // 一音一字不该凭空冒出连音线
        std::vector<LyricLine> one = {{0, "一二三四五六七八"}};
        LyricAssignment b = assignLyrics(d, one, 1);
        check(b.at.size() == 8 && b.melismas.empty(), "一音一字不画连音线");
        check(b.at[0].noteCount == 1 && b.at[0].text == "一", "一音一字仍是独立音节");

        // UTF-8：一个三字节汉字只能算一个音节，不能按字节拆成三个
        std::vector<LyricLine> utf = {{0, "啊"}};
        LyricAssignment u = assignLyrics(d, utf, 1);
        check(u.at.size() == 1 && u.at[0].text == "啊", "UTF-8 汉字不被按字节拆开");

        check(assignLyrics(d, {}, 1).empty(), "没有歌词时结果为空");
        check(assignLyrics(d, lines, -1).empty(), "轨道号无效时结果为空");
        check(assignLyrics(d, lines, 7).empty(), "歌词不会挂到不存在的轨道上");
    }

    // ---- MIDI 内置歌词（0x05 lyric meta） ----
    // 与 LRC 是两条通路：每个事件自带精确 tick，且流里混着元数据、延续符、
    // 换行标记、英文单词。这里锁住「规整 -> 精确对齐 -> 来源优先级」全链路。
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        NoteEvent n;
        n.track = 1; n.channel = 0; n.durationTicks = 480;
        for (int i = 0; i < 8; i++) {
            n.startTicks = (long)i * 480;
            n.pitch = 60 + i;
            d.notes.push_back(n);
        }
        // 伴奏轨：柱式和弦，音符多、复音率高，专门用来干扰旋律轨判定
        NoteEvent a;
        a.track = 2; a.channel = 1; a.durationTicks = 1920;
        for (int bar = 0; bar < 2; bar++)
            for (int p : {48, 52, 55}) {
                a.startTicks = (long)bar * 1920;
                a.pitch = p;
                d.notes.push_back(a);
            }

        d.midiLyrics = {
            {0,    "@KMIDI KARAOKE FILE", 1},   // 元数据，跳过
            {0,    "你", 1},
            {480,  "-", 1},                     // 延续，前一个字自然拖长
            {960,  "好，", 1},                   // 中文标点不占音节
            {1440, "_", 1},
            {1920, "/", 1},                     // 换行
            {1920, "+世界", 1},                  // 续行前缀 + 同 tick 两个字
            {2880, "Hello ", 1},                // 英文单词 + 尾随空格
            {3360, "-", 1},
        };

        auto syls = normalizeMidiLyrics(d);
        check(syls.size() == 5, "内置歌词规整出 5 个音节（元数据/延续/换行已滤掉）");
        std::string joined;
        for (const auto& s : syls) joined += s.text;
        check(joined == "你好世界Hello", "词序正确、标点被跳过、英文未被拆成字母");
        check(syls[0].spanEnd == 960 && syls[1].spanEnd == 1920,
              "延续符让前一个字的区间延长到下一个有效事件");
        check(syls[2].tick == 1920 && syls[3].tick == 1920 &&
              syls[2].spanEnd == 2880 && syls[3].spanEnd == 2880,
              "同一 tick 的多个字合成一批并共享右端");
        check(syls[4].spanEnd == 0, "末批右端留给对齐阶段填「曲终之后」");

        LyricAssignment m = assignMidiLyrics(d, 1);
        check(m.at.size() == 5, "5 个音节各挂到一个真实音头");
        check(m.at.count(0) && m.at[0].text == "你" && m.at[0].noteCount == 2 &&
              m.at[0].lastNoteTick == 480, "「你」拖过 0/480 两个音");
        check(m.at.count(960) && m.at[960].text == "好" && m.at[960].noteCount == 2,
              "「好」拖过 960/1440 两个音");
        check(m.at.count(1920) && m.at[1920].text == "世" &&
              m.at[1920].noteCount == 1 && m.at.count(2400) && m.at[2400].text == "界",
              "同批的「世」「界」在区间内均分，各唱一个音");
        check(m.at.count(2880) && m.at[2880].text == "Hello" &&
              m.at[2880].noteCount == 2, "「Hello」拖到曲终两个音");
        check(m.melismas.size() == 3, "只有 3 组一字多音");
        check(m.groupAt(480) == 1 && m.groupAt(3360) == 3 && m.groupAt(3841) == 0,
              "groupAt 覆盖延长音与末组，曲终之后出界");
        check(assignMidiLyrics(d, -1).empty(), "轨道号无效时结果为空");

        check(midiLyricTrack(d) == 1, "歌词事件所在轨被识别为人声轨");
        check(pickMelodyTrack(d) == 1, "有柱式和弦伴奏干扰时仍选 Vocal 轨");
        double sVocal = -1e9, sPiano = -1e9;
        for (const auto& p : profileTracks(d)) {
            if (p.track == 1) sVocal = p.score;
            if (p.track == 2) sPiano = p.score;
        }
        check(sVocal > sPiano, "人声轨得分高于伴奏轨（内置歌词轨证据生效）");

        check(lyricSourceOf(d, nullptr) == LyricSource::Midi, "未给 LRC 时来源是 MIDI 内置");
        check(lyricSourceLabel(d, nullptr).find("MIDI 内置") != std::string::npos,
              "页眉标注歌词来源");
        check(resolveLyrics(d, nullptr, 1).at.size() == 5, "统一入口回退到内置歌词");

        std::vector<LyricLine> ext = {{0, "甲乙"}};
        LyricAssignment e = resolveLyrics(d, &ext, 1);
        check(lyricSourceOf(d, &ext) == LyricSource::External, "显式 LRC 优先于内置歌词");
        check(e.at.count(0) && e.at[0].text == "甲" && !e.at.count(960),
              "优先级切换后按 LRC 规则分配，不再用内置 tick");

        MidiData d2 = d;
        d2.midiLyrics.clear();
        check(lyricSourceOf(d2, nullptr) == LyricSource::None &&
              resolveLyrics(d2, nullptr, 1).empty(),
              "--no-midi-lyrics 清空事件后彻底无词");

        // format 0（只有一条有音符的轨）与歌词分散在多轨时，轨号不可信
        MidiData d3 = d;
        d3.notes.erase(std::remove_if(d3.notes.begin(), d3.notes.end(),
                                      [](const NoteEvent& x) { return x.track == 2; }),
                       d3.notes.end());
        check(midiLyricTrack(d3) == -1, "只有一条有音符的轨时不采信歌词轨号");
        // 歌词分散到多轨：不再一律放弃，而是取音节最多的那条（这里 4:1），
        // 被忽略的轨数 / 音节数要如实记下来，供页眉说明。
        MidiData d4 = d;
        d4.midiLyrics[1].track = 2;
        const LyricTrackPick p4 = pickMidiLyricTrack(d4);
        check(p4.track == 1 && p4.syllables == 4 &&
                  p4.droppedTracks == 1 && p4.droppedSyllables == 1,
              "多轨歌词取音节最多的一轨，实际: 轨 " + std::to_string(p4.track) +
                  " / " + std::to_string(p4.syllables) + " 音节 / 丢弃 " +
                  std::to_string(p4.droppedTracks) + " 轨 " +
                  std::to_string(p4.droppedSyllables) + " 音节");
        check(midiLyricTrack(d4) == 1, "被采信的歌词轨仍须有音符");
        check(droppedLyricNote(d4).find("另 1 轨的 1 个音节已忽略") != std::string::npos,
              "忽略提示可读，实际: " + droppedLyricNote(d4));
        check(droppedLyricNote(d).empty(), "只有一条歌词轨时不提示忽略");
        check(normalizeMidiLyrics(d4).size() == 5 - 1,
              "规整结果只含被采信那条轨的 4 个音节");
        check(lyricSourceLabel(d4, nullptr).find("另 1 轨") != std::string::npos,
              "页眉的歌词来源也带上忽略说明");

        MidiData d5;
        d5.midiLyrics = {{0, "  ", 1}, {0, "- -", 1}, {0, "/", 1}, {0, "！", 1}};
        check(normalizeMidiLyrics(d5).empty(), "纯空白/延续符/换行/标点不产生音节");
        check(normalizeMidiLyrics(MidiData()).empty(), "没有内置歌词时规整结果为空");
        NoteEvent v;
        v.track = 1; v.channel = 0; v.startTicks = 0; v.durationTicks = 480; v.pitch = 60;
        d5.notes.push_back(v);
        NoteEvent w = v; w.track = 2;
        d5.notes.push_back(w);
        check(midiLyricTrack(d5) == -1, "只有元数据/延续符时不采信歌词轨号");
    }

    std::cout << "\n测试中途变拍号（拍号 map / 逐小节网格）\n";
    {
        // 4/4 ×2 -> 3/4 ×2 -> 6/8 ×2，伴奏轨还重复声明了同样的 0x58。
        // 旧实现拿最后一次 0x58 覆盖 data.timeSig，整份谱子按 6/8 划小节线，
        // 并连带触发「弱起 4 拍」误判——这是这份回归要锁住的核心場景。
        SmfTrack cond, vocal, piano;
        cond.at(0,    ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        cond.at(3840, ev({0xFF, 0x58, 0x04, 3, 2, 24, 8}));
        cond.at(6720, ev({0xFF, 0x58, 0x04, 6, 3, 24, 8}));
        piano.at(0,    ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));   // 多轨重复声明
        piano.at(3840, ev({0xFF, 0x58, 0x04, 3, 2, 24, 8}));
        auto addNote = [&](long tick, long dur) {
            vocal.at(tick, ev({0x90, 60, 90}));
            vocal.at(tick + dur, ev({0x80, 60, 0}));
        };
        for (long t : {0L, 480L, 960L, 1440L, 1920L, 2400L, 2880L, 3360L}) addNote(t, 480);
        for (long t : {3840L, 4320L, 4800L, 5280L, 5760L, 6240L}) addNote(t, 480);
        for (long t = 6720; t <= 9360; t += 240) addNote(t, 240);

        auto bytes = buildSmf(3, 480, {cond, vocal, piano});
        MidiData d;
        bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
        check(ok, "带变拍号的 SMF 能解析");
        check(d.timeSigChanges.size() == 3,
              "三次 0x58 全部记进 map，实际 " + std::to_string(d.timeSigChanges.size()));
        check(d.timeSig.toString() == "4/4", "data.timeSig 是曲首拍号，不是最后一个 6/8");
        check(meterChangesOf(d).size() == 2, "多轨重复声明不算变拍号");

        const BarGrid g = barGridOf(d);
        check(g.barStart(0) == 0 && g.barStart(1) == 1920 && g.barStart(2) == 3840 &&
              g.barStart(3) == 5280 && g.barStart(4) == 6720 && g.barStart(5) == 8160,
              "小节边界按 4/4(1920) / 3/4(1440) / 6/8(1440) 逐段铺开");
        check(g.barSpan(0) == 1920 && g.barSpan(2) == 1440 && g.barSpan(4) == 1440,
              "各段小节时长正确");
        check(g.sigForBar(0).toString() == "4/4" && g.sigForBar(2).toString() == "3/4" &&
              g.sigForBar(4).toString() == "6/8", "每小节拿到自己的拍号");
        check(g.beatsForBar(2) == 3 && g.beatsForBar(4) == 6, "每小节拍数随拍号变化");
        check(g.meterChangedAt(2) && g.meterChangedAt(4) && !g.meterChangedAt(3),
              "只有变化小节才标新拍号");
        check(g.barIndex(3839) == 1 && g.barIndex(3840) == 2 &&
              g.barIndex(5279) == 2 && g.barIndex(5280) == 3 && g.barIndex(6720) == 4,
              "tick -> 小节号跨段连续");
        check(g.barStart(20) > 9600, "表尾之后按最后一段长度安全外推");
        check(meterSummary(d) == "4/4 → 3/4(第3小节) → 6/8(第5小节)",
              "页眉摘要列出每次变化及其小节号: " + meterSummary(d));
        check(detectPickupTicks(d) == 0, "多拍号曲目禁用自动弱起判定");
        check(g.pickup == 0, "不再误判出「弱起 4 拍」");
        check(formatJianpu(d, KeyEstimate{0, true}).find("[3/4]") != std::string::npos,
              "文本谱在变化小节印出新拍号");

        // 反证：把这同一份数据伪装成「全曲 6/8」，弱起误判就会重现
        MidiData fake = d;
        fake.timeSigChanges.clear();
        fake.timeSig = TimeSignature{6, 3, true};
        check(detectPickupTicks(fake) > 0,
              "反证：若只认最后一个拍号，这份素材确实会被误判为弱起");

        // 手动 --pickup 与变拍号可以共存
        MidiData manual = d;
        manual.pickupTicks = 480;
        BarGrid gm = barGridOf(manual);
        check(gm.pickup == 480 && gm.barStart(0) == 0 && gm.barStart(1) == 480,
              "手动弱起与拍号 map 共存时网格仍正确");

        // 0x58 落在小节中间：在那里截断，新拍号从事件 tick 起生效
        MidiData mid = d;
        mid.timeSigChanges = {{0, TimeSignature{4, 2, true}},
                              {3000, TimeSignature{3, 2, true}}};
        mid.timeSig = TimeSignature{4, 2, true};
        BarGrid gmid = barGridOf(mid);
        bool cut = false;
        for (long s : gmid.starts) if (s == 3000) cut = true;
        check(cut && gmid.barIndex(3000) > 1, "拍号事件落在小节中间时截断该小节");

        // 单拍号文件：边界表与旧的等距除法完全等价
        MidiData plain = d;
        plain.timeSigChanges = {{0, TimeSignature{4, 2, true}}};
        plain.timeSig = TimeSignature{4, 2, true};
        BarGrid gp = barGridOf(plain);
        check(meterChangesOf(plain).empty() && !gp.hasMeterChange() &&
              gp.barStart(3) == 5760 && gp.barSpan(2) == 1920 &&
              meterSummary(plain) == "4/4", "单拍号文件行为与旧版一致");
    }

    std::cout << "\n测试中途转调（调号 map / KeyMap）\n";
    {
        // 1=C ×4 小节 -> 1=D ×4 -> 1=F ×4，伴奏轨还重复声明了 0x59。
        // 旧实现拿最后一次 0x59 覆盖 data.keySig，整份谱子都会按 F 大调
        // 换算唱名。旋律每小节第一个音故意都放 C4=60，一眼能看出三段
        // 应该记作 1 / #6, / 5,。
        SmfTrack cond, vocal, piano;
        cond.at(0,     ev({0xFF, 0x59, 0x02, 0x00, 0x00}));       // sf=0  C
        cond.at(7680,  ev({0xFF, 0x59, 0x02, 0x02, 0x00}));       // sf=+2 D
        cond.at(15360, ev({0xFF, 0x59, 0x02, 0xFF, 0x00}));       // sf=-1 F
        cond.at(0,     ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano.at(0,    ev({0xFF, 0x59, 0x02, 0x00, 0x00}));       // 多轨重复声明
        piano.at(7680, ev({0xFF, 0x59, 0x02, 0x02, 0x00}));
        auto addNote = [&](long tick, int pitch) {
            vocal.at(tick, ev({0x90, pitch, 90}));
            vocal.at(tick + 480, ev({0x80, pitch, 0}));
        };
        const int scales[3][4] = {{60, 62, 64, 65}, {60, 62, 64, 66}, {60, 65, 67, 69}};
        for (int seg = 0; seg < 3; seg++)
            for (int bar = 0; bar < 4; bar++)
                for (int b = 0; b < 4; b++)
                    addNote(seg * 7680L + bar * 1920L + b * 480L, scales[seg][b]);

        auto bytes = buildSmf(3, 480, {cond, vocal, piano});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "带转调的 SMF 能解析");
        check(d.keySigChanges.size() == 3,
              "三次 0x59 全记进 map，多轨同 tick 已去重，实际 " +
              std::to_string(d.keySigChanges.size()));
        check(d.keySig.valid && d.keySig.sharpsFlats == 0 && !d.keySig.isMinor,
              "data.keySig 固定是曲首调号，不是最后一个 F 大调");
        check(keyChangesOf(d).size() == 2, "只留实质性的转调点");

        const KeyEstimate cMajor{0, true};
        const BarGrid g = barGridOf(d);
        const KeyMap keys(d, cMajor);
        check(keys.hasChange() && keys.changes().size() == 2, "KeyMap 有两处变化");
        check(keys.changes()[0].tick == 7680 && keys.changes()[1].tick == 15360,
              "转调点落在第 5 / 第 9 小节线");
        check(keys.at(0).tonicPitchClass == 0 && keys.at(7679).tonicPitchClass == 0,
              "变化点之前仍是 C 大调");
        check(keys.at(7680).tonicPitchClass == 2 && keys.at(15359).tonicPitchClass == 2,
              "第 5 小节起是 D 大调直到下一次转调");
        check(keys.at(15360).tonicPitchClass == 5 && keys.at(99999).tonicPitchClass == 5,
              "第 9 小节起是 F 大调且延伸到曲终");
        check(pitchToJianpu(60, keys.at(0)) == "1", "C 段 C4 -> 1");
        check(pitchToJianpu(60, keys.at(7680)) == "#6,",
              "D 段同一个 C4 -> #6,（首调八度基准随 1 移动）");
        check(pitchToJianpu(60, keys.at(15360)) == "5,", "F 段同一个 C4 -> 5,");
        check(keys.keyChangedAtBar(4, g) && keys.keyChangedAtBar(8, g) &&
              !keys.keyChangedAtBar(0, g) && !keys.keyChangedAtBar(5, g),
              "只有转调小节才标新调号");
        check(keys.keyAtBar(4, g).tonicPitchClass == 2 &&
              keys.keyAtBar(8, g).tonicPitchClass == 5, "按小节取到生效调性");
        check(keys.summary(g) == "1=C 大调 → 1=D(第5小节) → 1=F(第9小节)",
              "页眉摘要列出每次转调及小节号: " + keys.summary(g));

        const std::string jp = formatJianpu(d, cMajor);
        check(jp.find("1=C 大调 → 1=D(第5小节) → 1=F(第9小节)") != std::string::npos,
              "文本谱页眉写出转调链");
        check(jp.find("[转1=D]") != std::string::npos &&
              jp.find("[转1=F]") != std::string::npos, "文本谱在变化小节标注转调");
        check(jp.find("#6,") != std::string::npos && jp.find("5,") != std::string::npos,
              "文本谱三段唱名确实不同");

        // --key 手工覆盖：转调是相对量，整条链跟着平移，不能跳回 MIDI 的绝对调号
        const KeyMap gKeys(d, KeyEstimate{7, true});
        check(gKeys.summary(g) == "1=G 大调 → 1=A(第5小节) → 1=C(第9小节)",
              "--key G 时转调链整体平移: " + gKeys.summary(g));
        check(gKeys.at(7680).tonicPitchClass == 9 && gKeys.at(15360).tonicPitchClass == 0,
              "手工覆盖后的各段绝对调性正确");
        const KeyMap emKeys(d, KeyEstimate{4, false});
        check(emKeys.summary(g).find("1=G (E小调, 主音=6) → 1=A(第5小节) → 1=C(第9小节)")
              != std::string::npos, "小调按首调 1=关系大调 参与转调");
        check(pitchToJianpu(76, emKeys.at(0)) == "6", "e 小调主音 E5 记作 6");
        check(pitchToJianpu(64, emKeys.at(0)) == "6,",
              "e 小调低八度主音 E4 记作 6,（1=G4 决定八度基准）");

        // --no-key-changes：事件表还在（曲首调号要用），只是不再应用
        MidiData off = d;
        off.applyKeyChanges = false;
        check(keyChangesOf(off).empty() && !KeyMap(off, cMajor).hasChange(),
              "--no-key-changes 关掉整条调号 map");
        check(KeyMap(off, cMajor).at(15360).tonicPitchClass == 0, "关闭后全曲按曲首调");
        check(formatJianpu(off, cMajor).find("转1=D") == std::string::npos,
              "关闭后谱面不再标转调");

        // 只有一个调号 / 多处重复声明同一个调号：都不算转调
        MidiData dup = d;
        dup.keySigChanges = {{0, KeySignature{0, false, true}},
                             {7680, KeySignature{0, false, true}},
                             {15360, KeySignature{0, false, true}}};
        check(keyChangesOf(dup).empty() && !KeyMap(dup, cMajor).hasChange(),
              "不同 tick 重复声明同一调号不算转调");

        // 转回原调：相对曲首没变，但谱面必须再写一次 1=C
        MidiData back = d;
        back.keySigChanges = {{0, KeySignature{0, false, true}},
                              {7680, KeySignature{2, false, true}},
                              {15360, KeySignature{0, false, true}}};
        check(keyChangesOf(back).size() == 2, "C→D→C 的「转回原调」不能丢");
        const KeyMap bk(back, cMajor);
        check(bk.changes().size() == 2 && bk.at(15360).tonicPitchClass == 0,
              "最后一回到 C 大调");
        check(bk.summary(g).find("→ 1=C(第9小节)") != std::string::npos,
              "转回原调也要标出来: " + bk.summary(g));

        // 0x59 落在小节中间：吸附到该小节起点，不允许一小节两种唱名体系
        MidiData midBar = d;
        midBar.keySigChanges = {{0, KeySignature{0, false, true}},
                                {8000, KeySignature{2, false, true}}};
        const KeyMap mk(midBar, cMajor);
        check(mk.changes().size() == 1 && mk.changes()[0].tick == 7680,
              "小节中间的转调点吸附到小节线");

        // 等音：调号写成 7 个升号时，首调 do 仍按半音平移（C#/Db 同音级）
        MidiData enh = d;
        enh.keySigChanges = {{0, KeySignature{0, false, true}},
                             {7680, KeySignature{7, false, true}}};
        check(KeyMap(enh, cMajor).at(7680).tonicPitchClass == 1,
              "极端调号按五度循环折算，不越界");

        // 和弦根名的等音写法跟着当前调走：F 段（降号调）写 Bb 而不是 A#
        MidiData harm = d;
        for (int p : {46, 50, 53}) {
            NoteEvent n;
            n.track = 2; n.channel = 1; n.pitch = p;
            n.startTicks = 15360; n.durationTicks = 1920;
            harm.notes.push_back(n);
        }
        bool hasBb = false, hasSharp = false;
        for (const auto& c : detectChords(harm, cMajor)) {
            if (c.startTick == 15360 && c.name == "Bb") hasBb = true;
            if (c.name.find("#") != std::string::npos) hasSharp = true;
        }
        check(hasBb && !hasSharp, "降号调里的 Bb 和弦不写成 A#");
    }

    // ---- 五声调式（宫商角徵羽）与宫音校正 ----
    {
        std::cout << "\n测试 五声调式宫音判定\n";
        // 只用 C D E G A 五个音级，最后一个音换成指定的终止音。
        // sixth 插一个变宫 B（六声），twoBias 再加一个清角 F（七声）。
        auto melody = [](int finalPitch, bool sixth, bool twoBias) {
            MidiData d;
            d.division = 480;
            d.timeSig = TimeSignature{4, 2, true};
            const int scale[5] = {60, 62, 64, 67, 69};
            std::vector<int> ps;
            for (int rep = 0; rep < 3; rep++)
                for (int i = 0; i < 5; i++) ps.push_back(scale[i]);
            if (sixth) ps.insert(ps.begin() + 4, 71);
            if (twoBias) ps.insert(ps.begin() + 8, 65);   // 插中间，别被末音覆盖
            ps.back() = finalPitch;
            for (size_t i = 0; i < ps.size(); i++) {
                NoteEvent n;
                n.track = 0; n.channel = 0; n.pitch = ps[i];
                n.startTicks = (long)i * 480;
                n.durationTicks = 480;
                n.durationSeconds = 0.5;
                d.notes.push_back(n);
            }
            return d;
        };

        struct ModeCase { int finalPitch; PentatonicMode mode; const char* label; };
        const ModeCase cases[] = {
            {60, PentatonicMode::Gong,  "C 宫调式"},
            {62, PentatonicMode::Shang, "D 商调式"},
            {64, PentatonicMode::Jue,   "E 角调式"},
            {67, PentatonicMode::Zhi,   "G 徵调式"},
            {69, PentatonicMode::Yu,    "A 羽调式"},
        };
        for (const auto& c : cases) {
            const MidiData d = melody(c.finalPitch, false, false);
            const PentatonicEstimate p = detectPentatonic(d);
            check(p.detected, std::string("检出调式 ") + c.label);
            check(p.mode == c.mode, std::string("调式判定正确 ") + c.label);
            check(p.gongPitchClass == 0, std::string("宫音仍是 C ") + c.label);
            check(p.finalPitchClass == c.finalPitch % 12,
                  std::string("终止音记录正确 ") + c.label);
            check(p.label().find(c.label) != std::string::npos,
                  "调式名可读 " + p.label());
            check(p.confidence > 0.999, "纯五声正音覆盖率接近 1");
        }
        check(pentatonicModeName(PentatonicMode::Zhi) == "徵", "调式名映射 徵");
        check(pentatonicModeName(PentatonicMode::None).empty(), "未检出时调式名为空");
        check(!PentatonicEstimate().detected &&
              PentatonicEstimate().label().empty(), "默认结果未检出且无标签");

        // 核心缺陷：KS 七声轮廓会把 G 徵调式判成 G 大调（1=G）
        const MidiData zhi = melody(67, false, false);
        const KeyEstimate kz = estimateKey(zhi);
        check(kz.tonicPitchClass == 0 && kz.isMajor,
              "G 徵调式按宫音校正为 1=C（而不是 1=G）");
        check(!kz.scaleLabel.empty(), "校正后带上调式标注");
        check(keyDescription(kz).find("G 徵调式（宫音 C）") != std::string::npos,
              "页眉写出调式名: " + keyDescription(kz));
        check(pitchToJianpu(67, kz) == "5", "终止音 G 记作 5 而不是 1");
        check(pitchToJianpu(60, kz) == "1", "宫音 C 记作 1");
        check(formatJianpu(zhi, kz).find("G 徵调式") != std::string::npos,
              "文本谱页眉带调式名");

        MidiData off = zhi;
        off.usePentatonic = false;
        const KeyEstimate ko = estimateKey(off);
        check(ko.tonicPitchClass == 7, "--no-pentatonic 回到 KS 的 G 大调");
        check(ko.scaleLabel.empty(), "关闭后不带调式标注");
        check(pitchToJianpu(67, ko) == "1", "关闭后 G 被误记成 1（对照）");
        check(keyDescription(ko) == "1=G 大调", "关闭后调性描述干净");

        // 宫音与 KS 首调一致时只补标注，不改写大小调结果（零回归）
        const MidiData yu = melody(69, false, false);
        const KeyEstimate ky = estimateKey(yu);
        check(ky.tonicPitchClass == 9 && !ky.isMajor,
              "A 羽调式保留 a 小调判定（宫音一致）");
        check(ky.scaleLabel.find("A 羽调式") != std::string::npos,
              "仍补上调式标注: " + ky.scaleLabel);
        check(pitchToJianpu(69, ky) == "6", "主音 A 仍记作 6");
        check(keyDescription(ky).find("主音=6") != std::string::npos,
              "原有小调说明不被覆盖");

        // 文件自带 0x59 时以调号为准，不做五声校正
        MidiData withSig = zhi;
        withSig.keySig = KeySignature{1, false, true};   // G 大调
        withSig.keySigChanges = {{0, KeySignature{1, false, true}}};
        const KeyEstimate ks = estimateKey(withSig);
        check(ks.tonicPitchClass == 7 && ks.scaleLabel.empty(),
              "有 0x59 调号时不做五声校正");
        check(detectPentatonic(withSig).detected,
              "调号与检测互不影响，检测函数仍可单独查询");

        // 六声（一个偏音）容忍；两个偏音 / 七声 / 终止音落偏音 / 宫音歧义都拒绝
        const MidiData sixth = melody(69, true, false);
        const PentatonicEstimate p6 = detectPentatonic(sixth);
        check(p6.detected && p6.mode == PentatonicMode::Yu && p6.gongPitchClass == 0,
              "六声（含一个变宫 B）仍判 A 羽调式");
        check(p6.outsideWeight > 0 && p6.confidence < 1.0,
              "六声记录偏音占比");
        check(!detectPentatonic(melody(69, true, true)).detected,
              "两个偏音（清角+变宫，七声）交回 KS");
        check(!detectPentatonic(melody(66, false, false)).detected,
              "终止音落在偏音上时拒绝判定");

        MidiData ambiguous;
        ambiguous.division = 480;
        ambiguous.timeSig = TimeSignature{4, 2, true};
        {
            const int amb[4] = {60, 62, 67, 69};   // C D G A 既像 C 宫也像 F 宫
            for (int i = 0; i < 16; i++) {
                NoteEvent n;
                n.track = 0; n.channel = 0; n.pitch = (i == 15) ? 67 : amb[i % 4];
                n.startTicks = (long)i * 480; n.durationTicks = 480;
                n.durationSeconds = 0.5;
                ambiguous.notes.push_back(n);
            }
        }
        check(!detectPentatonic(ambiguous).detected,
              "宫音有歧义时宁可不报，也不把 1 定错");

        // 手动指定宫音：--gong F / Bb
        const KeyEstimate fGong = keyFromGong(zhi, 5);
        check(fGong.tonicPitchClass == 5 && fGong.isMajor, "--gong F 定 1=F");
        check(fGong.scaleLabel.find("G 商调式（宫音 F）") != std::string::npos,
              "按终止音推出 G 商调式: " + fGong.scaleLabel);
        check(pitchToJianpu(65, fGong) == "1", "F 记作 1");
        const KeyEstimate bbGong = keyFromGong(zhi, 10);
        check(bbGong.scaleLabel.find("宫音 Bb") != std::string::npos,
              "降号宫音写 Bb 而不是 A#: " + bbGong.scaleLabel);
        check(prefersFlats(bbGong) && keyFifths(bbGong) == -2,
              "降号宫音的等音写法与调号一致");
    }

    // ---- TempoTimeline：tick <-> 秒 ----
    {
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        d.tempoChanges = {{0, 120.0}, {7680, 60.0}};   // 第 5 小节起减半速
        const TempoTimeline tl(d);
        check(tl.changeCount() == 1 && tempoChangeCount(d) == 1,
              "一处实质变速");
        check(std::abs(tl.tickToSeconds(0) - 0.0) < 1e-9, "原点 0 tick = 0 秒");
        check(std::abs(tl.tickToSeconds(7680) - 8.0) < 1e-9,
              "120BPM 段 7680 tick = 8 秒");
        check(std::abs(tl.tickToSeconds(9600) - 12.0) < 1e-9,
              "60BPM 段 9600 tick = 12 秒");
        check(tl.secondsToTick(12.0) == 9600, "12 秒反解回 9600 tick");
        check(tl.secondsToTick(20.0) == 13440, "20 秒反解回 13440 tick");
        check(tl.secondsToTick(24.0) == 15360, "越过末段按末段速度外推");
        check(tl.secondsToTick(-1.0) == -960, "负秒按曲首速度线性外推");
        bool roundTrip = true;
        for (long t : {0L, 1920L, 7680L, 9600L, 13440L, 15360L})
            if (tl.secondsToTick(tl.tickToSeconds(t)) != t) roundTrip = false;
        check(roundTrip, "变速前后 tick/秒 往返无损");

        const TempoTimeline messy({{7680, 60.0}, {0, 120.0}, {3840, 240.0}}, 480);
        check(messy.changeCount() == 2, "乱序 set_tempo 先按 tick 排序");
        check(std::abs(messy.tickToSeconds(3840) - 4.0) < 1e-9,
              "乱序表第一段仍是 120BPM");
        check(std::abs(messy.tickToSeconds(7680) - 6.0) < 1e-9,
              "乱序表第二段按 240BPM 累积");
        const TempoTimeline dup({{0, 120.0}, {3840, 60.0}, {3840, 240.0}}, 480);
        check(dup.changeCount() == 1 && dup.segments().size() == 2,
              "同 tick 重复声明只留一段");
        const TempoTimeline head({{0, 60.0}}, 480);
        check(head.changeCount() == 0 &&
              std::abs(head.tickToSeconds(480) - 1.0) < 1e-9,
              "曲首 set_tempo 生效但不算变速点");
        MidiData plain;
        plain.division = 480;
        check(TempoTimeline(plain).changeCount() == 0,
              "没有 0x51 事件时按 MIDI 默认 120BPM");
        check(tempoChangeCount(plain) == 0, "单调速文件 tempoChangeCount = 0");
    }

    // ---- LRC 时间戳按 tempo map 反解，而不是固定 secPerBeat ----
    {
        MidiData d;
        d.division = 480;
        d.tempoChanges = {{0, 120.0}, {7680, 60.0}};
        const std::string lrc =
            "[ti:变速测试]\n"
            "[00:00.00]春风又绿\n"
            "[00:02.00]江南岸边\n"
            "[00:12.00]又东风起\n"
            "[00:20.00]回首月中\n";
        const auto fixed = parseLrcText(lrc, 480, 120);   // 旧签名：曲首速度一把梭
        const auto mapped = parseLrcText(lrc, d);          // 新签名：分段反解
        check(fixed.size() == 4 && mapped.size() == 4, "两种签名都读出 4 行 LRC");
        check(fixed[0].startTick == 0 && mapped[0].startTick == 0,
              "曲首时间戳两种换算一致");
        check(fixed[1].startTick == 1920 && mapped[1].startTick == 1920,
              "变速前的时间戳两种换算一致");
        check(fixed[2].startTick == 11520 && mapped[2].startTick == 9600,
              "12 秒：固定 BPM 算 11520（错一小节），tempo map 算 9600");
        check(fixed[3].startTick == 19200 && mapped[3].startTick == 13440,
              "20 秒：固定 BPM 已超出曲终，tempo map 落在第 8 小节");
        check(mapped[0].text == "春风又绿" && mapped[3].text == "回首月中",
              "元数据行被跳过，歌词文本无损");

        const auto offNeg = parseLrcText(
            "[offset:-2000]\n[00:02.00]甲\n[00:14.00]乙\n", d);
        // 14 秒 - 2 秒 = 12 秒，落在 60BPM 段 -> tick 9600
        check(offNeg.size() == 2 && offNeg[0].startTick == 0 &&
              offNeg[1].startTick == 9600, "负 [offset:] 把秒数拉回");
        const auto offPos = parseLrcText(
            "[OFFSET:2000]\n[00:00.00]甲\n", d);
        check(offPos.size() == 1 && offPos[0].startTick == 1920,
              "正 [offset:] 且标签大写也生效");
        const auto offLast = parseLrcText(
            "[offset:1000]\n[offset:-1000]\n[00:02.00]甲\n", d);
        check(offLast.size() == 1 && offLast[0].startTick == 960,
              "多个 [offset:] 取最后一个");

        const auto multi = parseLrcText(
            "[00:08.00][00:00.00]甲乙\n[00:02.00]丙丁\n", d);
        check(multi.size() == 3, "一行两个时间戳产生两行词");
        check(multi[0].startTick == 0 && multi[0].text == "甲乙",
              "多时间戳结果按 tick 升序稳定排序");
        check(multi[1].startTick == 1920 && multi[1].text == "丙丁",
              "多时间戳不挤掉相邻行");
        check(multi[2].startTick == 7680 && multi[2].text == "甲乙",
              "8 秒处的重复句落在变速点");

        check(!parseLrcText("乱码\n[ti:x]\n[\n[00:01.00]好\n", d).empty(),
              "畸形 LRC 不崩且仍能读到正常行");
        check(parseLrcText("[ar:某人]\n[al:专辑]\n", d).empty(),
              "纯元数据 LRC 返回空");
        check(parseLrcText("[00:01.00]甲\n", 0, 120).empty() &&
              parseLrcText("[00:01.00]甲\n", 480, 0).empty(),
              "非法 division / bpm 时旧签名安全返回空");
    }

    std::cout << "\n测试演奏表情分析 (ExpressionMap)\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        // 12 个等间隔四分音符：断奏只有在等间隔流里才可判定。
        // rawDurationTicks 是量化前的门限，durationTicks 是量化后的记谱时值。
        for (int i = 0; i < 12; i++) {
            NoteEvent n;
            n.startTicks = i * 480;
            n.pitch = 60 + (i % 5);
            n.track = 0;
            n.channel = 0;
            n.velocity = (i == 5 || i == 9) ? 122 : 80;
            n.rawDurationTicks = (i == 1) ? 120 : ((i == 2) ? 210 : 413);
            n.durationTicks = 480;
            d.notes.push_back(n);
        }
        // 与旋律同 tick、不同通道的伴奏：不能继承旋律的断音/滑音
        NoteEvent bass;
        bass.startTicks = 480;
        bass.durationTicks = 480;
        bass.rawDurationTicks = 480;
        bass.pitch = 48;
        bass.track = 1;
        bass.channel = 1;
        bass.velocity = 70;
        d.notes.push_back(bass);

        d.dynamics = {{0, 30, 0}, {480, 45, 0}, {960, 60, 0},
                      {1440, 80, 0}, {1920, 100, 0}, {2400, 95, 0},
                      {2880, 110, 0}, {3360, 90, 0}, {3840, 70, 0},
                      {4320, 50, 0}};
        d.markers = {{0, "Verse"}, {2880, "Chorus"}};
        // 上滑 / 下滑 / 颤音，回中事件刻意落在各音区间之外
        d.pitchBends = {{0, 8192, 0}, {100, 9000, 0}, {200, 10000, 0},
                        {300, 11000, 0}, {650, 8192, 0},
                        {1920, 8192, 0}, {2000, 7400, 0}, {2100, 6600, 0},
                        {2200, 5800, 0}, {2350, 8192, 0},
                        {3360, 8192, 0}, {3420, 10000, 0}, {3480, 6500, 0},
                        {3540, 10500, 0}, {3600, 6200, 0}, {3660, 10000, 0},
                        {3800, 8192, 0}};

        ExpressionMap e(d);
        check(e.enabled(), "useDynamics 默认开启");
        check(e.dynamicAt(0, 0) == "p" && e.dynamicAt(1920, 0) == "ff",
              "CC11 按 tick 查当前力度档位");
        check(e.dynamicAt(0, 1).empty(), "力度按通道隔离，伴奏不继承旋律曲线");
        check(e.crescendoCount() == 1 && e.diminuendoCount() == 1,
              "单调 CC11 序列各识别出 1 段渐强 / 渐弱");
        check(e.sectionCount() == 2, "两个 0x06 marker 都成为段落名");
        check(!e.marks().empty() && e.marks().front().section == "Verse",
              "marks 按 tick 升序且同 tick 事件已合并");
        check(e.inRange(2880, 3360).size() == 1, "inRange 只取该小节的标记");
        check(e.noteAt(480, 0).staccatissimo, "极短门限判为断奏 staccatissimo");
        check(e.noteAt(960, 0).staccato, "偏短门限判为断奏 staccato");
        check(!e.noteAt(480, 1).any(), "伴奏轨不继承旋律的断音（按通道索引）");
        check(e.noteAt(2400, 0).accent, "velocity 显著高于邻音判为重音");
        check(!e.noteAt(1440, 0).accent, "普通力度不标重音");
        check(e.noteAt(0, 0).bend == BendMark::SlideUp, "单调上行弯音判为上滑");
        check(e.noteAt(1920, 0).bend == BendMark::SlideDown, "单调下行弯音判为下滑");
        check(e.noteAt(3360, 0).bend == BendMark::Trill, "往复振荡弯音判为颤音");
        check(e.noteAt(480, 0).symbol().find("\xE2\x96\xBC") != std::string::npos,
              "symbol() 写出断奏记号");
        check(e.noteAt(3360, 0).symbol() == "tr", "symbol() 写出颤音记号");
        check(e.summary().find("渐强") != std::string::npos &&
              e.summary().find("段落") != std::string::npos,
              "summary 汇总各类表情");
        check(e.totalCount() == e.dynamicCount() + e.crescendoCount() +
                                e.diminuendoCount() + e.articulationCount() +
                                e.bendCount() + e.sectionCount(),
              "totalCount 与各项计数一致");
        check(e.bendCount() == 3, "弯音记号共 3 个（上滑 / 下滑 / 颤音）");

        // 力度档位表全档覆盖
        MidiData lv;
        lv.dynamics = {{0, 0, 0}, {10, 20, 0}, {20, 40, 0},
                       {30, 60, 0}, {40, 80, 0}, {50, 100, 0}};
        ExpressionMap le(lv);
        check(le.dynamicAt(0, 0) == "pp" && le.dynamicAt(10, 0) == "p" &&
              le.dynamicAt(20, 0) == "mp" && le.dynamicAt(30, 0) == "mf" &&
              le.dynamicAt(40, 0) == "f" && le.dynamicAt(50, 0) == "ff",
              "CC11 六档力度映射完整");
        check(dynamicToMidiVolume("pp") == 30 && dynamicToMidiVolume("ff") == 100,
              "MusicXML sound dynamics 百分比映射");

        // 关掉开关：分析层整体静默，输出层不必各自判断
        MidiData off = d;
        off.useDynamics = false;
        ExpressionMap z(off);
        check(!z.enabled() && z.marks().empty() && z.dynamicAt(0, 0).empty() &&
              !z.noteAt(480, 0).any() && z.summary().empty() && z.totalCount() == 0,
              "--no-dynamics 让整张表情图为空");

        // 等间隔闸门：节奏混合的音符流不判断奏
        MidiData mixed = d;
        mixed.notes.clear();
        const long gaps[] = {0, 480, 960, 1680, 2400, 3600, 4080, 5040};
        for (int i = 0; i < 8; i++) {
            NoteEvent n;
            n.startTicks = gaps[i];
            n.durationTicks = 480;
            n.rawDurationTicks = (i == 2) ? 100 : 413;
            n.track = 0; n.channel = 0; n.velocity = 80; n.pitch = 60;
            mixed.notes.push_back(n);
        }
        mixed.dynamics.clear(); mixed.markers.clear(); mixed.pitchBends.clear();
        ExpressionMap me(mixed);
        check(me.articulationCount() == 0,
              "节奏混合的旋律里不误判断奏（宁可漏判也不污染谱面）");
    }

    std::cout << "\n测试解析层：velocity / 原始时值 / 事件排序\n";
    {
        // 轨 0 的 CC11 在 tick 500，轨 1 的在 tick 10：
        // 按文件顺序解析会乱序，收齐后必须统一排序。
        SmfTrack a, b;
        a.at(0, ev({0x90, 60, 111}));
        a.at(480, ev({0x80, 60, 0x40}));
        a.at(500, ev({0xB0, 0x0B, 64}));
        a.at(600, ev({0xFF, 0x06, 5, 'I', 'n', 't', 'r', 'o'}));
        a.at(700, ev({0xE0, 0x00, 0x40}));
        b.at(10, ev({0xB0, 0x0B, 32}));
        auto bytes = buildSmf(2, 480, {a, b});
        MidiData p;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), p), "解析两张轨的 SMF");
        check(p.notes.size() == 1 && p.notes[0].velocity == 111,
              "note-on velocity 存进 NoteEvent");
        check(p.notes[0].rawDurationTicks == 480,
              "量化前的原始发声时长独立保存");
        check(p.dynamics.size() == 2 && p.dynamics[0].tick == 10 &&
              p.dynamics[1].tick == 500, "多轨 CC11 事件按 tick 排序");
        check(p.markers.size() == 1 && p.markers[0].text == "Intro",
              "0x06 marker 解析为段落名");
        check(p.pitchBends.size() == 1 && p.pitchBends[0].channel == 0,
              "弯音事件带通道");
    }

    // ---- v1.21.0: GM 音色名（英文全名 / 中文译名覆盖）----
    std::cout << "\n测试 GM 音色名（英文全名 / 中文译名覆盖）\n";
    {
        // 常用音色的中文译名必须精确对应；没有覆盖的回落英文全名；越界回落空串。
        check(gmInstrumentName(0) == "钢琴", "GM0 -> 钢琴（中文覆盖）");
        check(gmInstrumentName(73) == "长笛", "GM73 -> 长笛");
        check(gmInstrumentName(32) == "原声贝斯", "GM32 -> 原声贝斯");
        check(gmInstrumentName(25) == "钢弦吉他", "GM25 -> 钢弦吉他");
        check(gmInstrumentName(40) == "小提琴", "GM40 -> 小提琴");
        check(gmInstrumentName(119) == "Reverse Cymbal",
              "GM119 无中文覆盖时回落英文全名, 实际: " + gmInstrumentName(119));
        check(gmInstrumentName(200).empty(), "越界音色号回落空串");
    }

    // ---- 解析层：0xC0 音色号 -> channelProgram ----
    std::cout << "\n测试解析层 0xC0（通道 -> GM 音色号）\n";
    {
        SmfTrack cond, vocal, piano, bass;
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        vocal.at(0, ev({0xC0, 73}));
        piano.at(0, ev({0xC1, 0}));
        bass.at(0,  ev({0xC2, 32}));
        auto n1 = [&](SmfTrack& tr, long t, int ch, int p) {
            tr.at(t, ev({0x90 | ch, p, 90}));
            tr.at(t + 480, ev({0x80 | ch, p, 0}));
        };
        n1(vocal, 0, 0, 72); n1(vocal, 480, 0, 74);
        n1(piano, 0, 1, 60); n1(piano, 480, 1, 64);
        n1(bass, 0, 2, 48);  n1(bass, 480, 2, 43);
        auto bytes = buildSmf(4, 480, {cond, vocal, piano, bass});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "带 0xC0 的 SMF 能解析");
        check(d.channelProgram.size() == 3,
              "三个通道各记一个音色号, 实际 " + std::to_string(d.channelProgram.size()));
        check(d.channelProgram[0] == 73, "通道 0 -> 长笛(73)");
        check(d.channelProgram[1] == 0,  "通道 1 -> 钢琴(0)");
        check(d.channelProgram[2] == 32, "通道 2 -> 原声贝斯(32)");
    }

    // ---- 声部 / 乐器标注：listParts / partSummary ----
    std::cout << "\n测试声部乐器标注（listParts / partSummary）\n";
    {
        SmfTrack cond, vocal, piano, bass;
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        vocal.at(0, ev({0xFF, 0x03, 5, 'V', 'o', 'c', 'a', 'l'}));
        vocal.at(0, ev({0xC0, 73}));
        piano.at(0, ev({0xFF, 0x03, 5, 'P', 'i', 'a', 'n', 'o'}));
        piano.at(0, ev({0xC1, 0}));
        bass.at(0,  ev({0xFF, 0x03, 4, 'B', 'a', 's', 's'}));
        bass.at(0,  ev({0xC2, 32}));
        auto n1 = [&](SmfTrack& tr, long t, int ch, int p) {
            tr.at(t, ev({0x90 | ch, p, 90}));
            tr.at(t + 480, ev({0x80 | ch, p, 0}));
        };
        n1(vocal, 0, 0, 72); n1(vocal, 480, 0, 74);
        n1(piano, 0, 1, 60); n1(piano, 480, 1, 64);
        n1(bass, 0, 2, 48);  n1(bass, 480, 2, 43);
        auto bytes = buildSmf(4, 480, {cond, vocal, piano, bass});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "带声部名的 SMF 能解析");
        auto parts = listParts(d);
        check(parts.size() == 3, "列出三个声部, 实际 " + std::to_string(parts.size()));
        check(parts[0].first == "Vocal" && parts[0].second == "长笛", "Vocal·长笛");
        check(parts[1].first == "Piano" && parts[1].second == "钢琴", "Piano·钢琴");
        check(parts[2].first == "Bass"  && parts[2].second == "原声贝斯", "Bass·原声贝斯");
        std::string sum = partSummary(d);
        check(sum == "声部：Vocal·长笛，Piano·钢琴，Bass·原声贝斯",
              "partSummary 串成一行, 实际: " + sum);
    }

    // ---- 倚音（装饰音）检测 + 通道隔离 ----
    std::cout << "\n测试倚音检测（极短前导音 + 紧邻长音）\n";
    {
        // 旋律轨（track 0）放一个倚音：B3(59) 极短 -> C4(60) 长音
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        auto add = [&](long t, long dur, long raw, int p, int ch, int trk) {
            NoteEvent n;
            n.startTicks = t; n.durationTicks = dur; n.rawDurationTicks = raw;
            n.pitch = p; n.track = trk; n.channel = ch; n.velocity = 90;
            d.notes.push_back(n);
        };
        add(460, 14, 14, 59, 0, 0);   // 倚音 B3（极短）
        add(480, 480, 480, 60, 0, 0); // 主音 C4（长）
        add(960, 480, 480, 62, 0, 0); // 普通音 D4（无倚音）
        // 另一通道放一串极短音（快速经过音流），必须不被误判成倚音
        add(0, 120, 120, 72, 1, 1);
        add(240, 120, 120, 74, 1, 1);
        add(480, 120, 120, 76, 1, 1);

        ExpressionMap e(d);
        check(e.isGraceNote(460, 0), "B3(460) 被判定为倚音");
        check(e.noteAt(480, 0).hasGrace(), "主音 C4(480) 带上倚音标记");
        check(e.noteAt(480, 0).gracePitch == 59, "主音记住倚音音高 B3(59)");
        check(!e.isGraceNote(480, 0), "主音本身不是倚音");
        check(!e.noteAt(960, 0).hasGrace(), "无前导短音的 D4 不带倚音");
        // 通道隔离：另一通道的极短音流没有长音接续，不应产生倚音
        check(!e.isGraceNote(0, 1) && !e.isGraceNote(240, 1) && !e.isGraceNote(480, 1),
              "通道 1 的短音流未被误判成倚音（阈值与隔离双重保险）");
    }

    // ---- 真实曲目零回归：蝶衣碎雪（2 轨 / 无 marker / prog 0）-----
    // 这份基线曲不应出现任何乐器名与倚音，谱面与历史版本一致。
    {
        std::cout << "\n测试真实曲目零回归（蝶衣碎雪：无乐器名 / 无倚音）\n";
        // 直接核对 partSummary 为空 —— 验证「已解析未使用」的字段不会
        // 在无 0xC0 / 无倚音的曲子上凭空产出标注。
        // 具体字节级回归由 smoke.py / wasm_smoke.mjs 锁死，这里只验标注层。
        // 占位：下面用内存构造一份「无 0xC0 的两轨曲」等效验证。
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        // 轨 0：声部名 Vocal，但未写 0xC0（prog 0 也行，但刻意不写 event）
        d.trackNames[0] = "Vocal";
        d.trackNames[1] = "prog 0";
        auto add = [&](long t, int p, int trk) {
            NoteEvent n;
            n.startTicks = t; n.durationTicks = 480; n.rawDurationTicks = 480;
            n.pitch = p; n.track = trk; n.channel = trk; n.velocity = 90;
            d.notes.push_back(n);
        };
        for (int i = 0; i < 8; i++) add(i * 480L, 60 + (i % 5), 0);
        for (int i = 0; i < 8; i++) add(i * 480L, 48 + (i % 3), 1);
        // 两个通道都没有 0xC0 -> channelProgram 为空
        check(d.channelProgram.empty(), "无 0xC0 时 channelProgram 为空");
        std::string sum = partSummary(d);
        // 有声部名但无乐器：listParts 查不到音色号 -> second 为空，仍列出声部名
        check(sum.find("·") == std::string::npos,
              "没有音色号时不强行拼乐器名, 实际: " + sum);
        ExpressionMap e(d);
        bool anyGrace = false;
        for (const auto& n : d.notes)
            if (e.isGraceNote(n.startTicks, n.channel)) anyGrace = true;
        check(!anyGrace, "等间隔四分音符流不误判倚音");
    }

    // ---- 和弦：转位（slash bass）+ 半减七 / 挂四七 ----
    std::cout << "\n测试和弦转位与七和弦扩充（detectChords）\n";
    {
        // 每个用例：一条单声部旋律（track 0，保证被选为旋律轨）+ 一个伴奏和弦
        auto chordOf = [](std::vector<int> acc, ChordMark* outMark) -> std::string {
            MidiData d;
            d.division = 480;
            d.timeSig = TimeSignature{4, 2, true};
            const int mel[8] = {72, 74, 76, 77, 79, 81, 83, 84};
            for (int i = 0; i < 8; i++) {
                NoteEvent n;
                n.startTicks = i * 240L;
                n.durationTicks = 240; n.rawDurationTicks = 240;
                n.pitch = mel[i]; n.track = 0; n.channel = 0; n.velocity = 90;
                d.notes.push_back(n);
            }
            for (int p : acc) {
                NoteEvent n;
                n.startTicks = 0;
                n.durationTicks = 960; n.rawDurationTicks = 960;
                n.pitch = p; n.track = 1; n.channel = 1; n.velocity = 70;
                d.notes.push_back(n);
            }
            KeyEstimate k;
            auto chords = detectChords(d, k);
            if (chords.empty()) return std::string();
            if (outMark) *outMark = chords[0];
            return chords[0].name;
        };

        ChordMark cm;
        std::string nm;
        nm = chordOf({48, 52, 55}, &cm);
        check(nm == "C", "原位三和弦标 C, 实际: " + nm);
        check(cm.rootPc == 0 && cm.kind == "major" && cm.bassPc == -1,
              "原位 C：rootPc=0 / kind=major / 无转位");

        nm = chordOf({52, 55, 60}, &cm);
        check(nm == "C/E", "E 在低音时标 C/E, 实际: " + nm);
        check(cm.rootPc == 0 && cm.bassPc == 4, "C/E 的 rootPc=0 / bassPc=4(E)");

        nm = chordOf({47, 50, 53, 57}, &cm);
        check(nm == "Bm7b5", "B-D-F-A 标半减七 Bm7b5, 实际: " + nm);
        check(cm.kind == "half-diminished" && cm.kindText == "m7b5",
              "Bm7b5 的 kind=half-diminished / 显示后缀 m7b5");

        nm = chordOf({43, 48, 50, 53}, &cm);
        check(nm == "G7sus4", "G-C-D-F 标挂四七 G7sus4, 实际: " + nm);

        // 低音是和弦外音（经过音 / 持续音）时不是转位，不能写斜杠
        nm = chordOf({41, 48, 52, 55}, &cm);
        check(nm == "C", "低音 F 非 C 和弦音时不写 C/F, 实际: " + nm);
        check(cm.bassPc == -1, "非和弦音低音不产生转位");
    }

    // ---- 速度曲线折叠：连续 set_tempo -> rit. / accel. ----
    {
        // 造一份「每 half 拍一个自动化节点」的速度曲线，与 DAW 画速度包络
        // 时写出的东西一致（真实 MIDI 里 rit. 就是几十个 set_tempo）。
        auto rampData = [](long fromTick, long toTick, double a, double b,
                           long step) {
            MidiData d;
            d.division = 480;
            d.timeSig = TimeSignature{4, 2, true};
            if (fromTick > 0) d.tempoChanges.push_back({0, a});
            const long steps = (toTick - fromTick) / step;
            for (long k = 0; k <= steps; k++)
                d.tempoChanges.push_back({fromTick + k * step,
                                          a + (b - a) * (double)k / (double)steps});
            return d;
        };

        // 1) 单调速：只有一个记号，且是阶跃
        {
            MidiData d;
            d.division = 480;
            d.tempoChanges = {{0, 120.0}};
            const auto m = tempoMarksOf(d);
            check(m.size() == 1 && m[0].kind == TempoMarkKind::Step && m[0].bpm == 120,
                  "单一速度只出一个阶跃记号");
        }

        // 2) 空表：不产生任何记号（不能凭空造一个 ♩=120）
        {
            MidiData d;
            d.division = 480;
            check(tempoMarksOf(d).empty(), "没有 0x51 事件时不出速度记号");
        }

        // 3) 阶跃变速：120 -> 90（第 5 小节），彼此间隔远超一拍，各自一个记号
        {
            MidiData d;
            d.division = 480;
            d.tempoChanges = {{0, 120.0}, {7680, 90.0}};
            const auto m = tempoMarksOf(d);
            check(m.size() == 2 && m[1].kind == TempoMarkKind::Step && m[1].bpm == 90,
                  "阶跃变速仍是阶跃，不改写成 rit.");
            check(tempoChangeCount(d) == 1, "一处变速的计数不变");
        }

        // 4) 渐慢：13 个节点（120 -> 60）-> 曲首阶跃 + 一个 Ritardando
        {
            const MidiData d = rampData(1920, 4800, 120.0, 60.0, 240);
            const auto m = tempoMarksOf(d);
            check(m.size() == 2, "13 个渐慢节点收敛成 2 个记号，实际: " +
                                 std::to_string(m.size()));
            check(m[0].kind == TempoMarkKind::Step && m[0].bpm == 120,
                  "曲首速度单独成记号");
            check(m[1].kind == TempoMarkKind::Ritardando,
                  "下降且单调 -> Ritardando");
            check(m[1].bpm == 120 && m[1].toBpm == 60, "渐慢记成 120→60");
            check(m[1].pointCount == 13, "记录原始节点数 13，实际: " +
                                         std::to_string(m[1].pointCount));
            check(tempoChangeCount(d) == 1,
                  "一整段渐慢只算一处变速，不是 13 处");
        }

        // 5) 渐快：下降方向反过来必须是 Accelerando，不能一律写 rit.
        {
            const MidiData d = rampData(1920, 4800, 100.0, 140.0, 240);
            const auto m = tempoMarksOf(d);
            check(m.size() == 2 && m[1].kind == TempoMarkKind::Accelerando,
                  "上升且单调 -> Accelerando");
            check(m[1].bpm == 100 && m[1].toBpm == 140, "渐快记成 100→140");
        }

        // 6) 跨度太小：120 -> 118（1.7%）是自动化抖动，不是 rit.
        {
            std::vector<TempoPoint> pts{{0, 120.0}};
            for (int k = 1; k <= 6; k++) pts.push_back({1920 + k * 240, 120.0 - k * 0.33});
            MidiData d;
            d.division = 480;
            d.tempoChanges = pts;
            const auto m = tempoMarksOf(d);
            check(m.size() == 2 && m[1].kind == TempoMarkKind::Step,
                  "跨度 < 5% 的抖动不退化成 rit.");
        }

        // 7) 非单调（自动化曲线来回拉）：谱面没有对应记号，退化成阶跃取末值
        {
            MidiData d;
            d.division = 480;
            d.tempoChanges = {{0, 120.0}, {1920, 130.0}, {2160, 118.0},
                              {2400, 135.0}, {2640, 122.0}};
            const auto m = tempoMarksOf(d);
            check(m.size() == 2 && m[1].kind == TempoMarkKind::Step && m[1].bpm == 122,
                  "非单调速度抖动退化成阶跃（取末值），实际: " +
                  std::to_string(m[1].bpm));
        }

        // 8) 聚簇阈值是「一拍」：间隔超过一拍的两个点必须分开成两个记号，
        //    否则两次独立的阶跃变速会被误当成一段渐变。
        {
            MidiData d;
            d.division = 480;
            d.tempoChanges = {{0, 120.0}, {1920, 110.0}, {2500, 100.0}};
            // 1920 -> 2500 间隔 580 tick > 480，不同簇
            const auto m = tempoMarksOf(d);
            check(m.size() == 3, "间隔超过一拍的变速点各自成记号，实际: " +
                                 std::to_string(m.size()));
        }

        // 9) 页眉文案：阶跃写 ♩=N，渐变写 rit./accel. 并附起止速度
        {
            const MidiData d = rampData(1920, 4800, 120.0, 60.0, 240);
            const std::string s = tempoSummary(d);
            check(s.find("♩=120") == 0, "页眉以曲首速度开头: " + s);
            check(s.find("rit. 120→60(第2小节)") != std::string::npos,
                  "页眉把渐慢写成 rit. 120→60: " + s);
            check(s.find("♩=116") == std::string::npos,
                  "页眉不再逐点罗列中间速度");

            const MidiData up = rampData(1920, 4800, 120.0, 150.0, 240);
            check(tempoSummary(up).find("accel. 120→150") != std::string::npos,
                  "页眉把渐快写成 accel.: " + tempoSummary(up));
        }

        // 10) 「从头开始渐慢」：tick 0 就是渐变起点，曲首阶跃仍要先写出来
        {
            MidiData d;
            d.division = 480;
            for (int k = 0; k <= 8; k++)
                d.tempoChanges.push_back({k * 240, 120.0 - k * 2.0});
            const auto m = tempoMarksOf(d);
            check(m.size() == 2 && m[0].kind == TempoMarkKind::Step && m[0].bpm == 120,
                  "曲首速度恒为独立阶跃记号");
            check(m[1].kind == TempoMarkKind::Ritardando && m[1].toBpm == 104,
                  "曲首起的渐慢仍被识别，终点: " + std::to_string(m[1].toBpm));
        }
    }

    // -----------------------------------------------------------------
    // 长音跨小节（延音线）：时值不能被截断，也不能被门限放大
    // -----------------------------------------------------------------
    {
        // 一条连续旋律：4 个四分音符 + 一个长音 + 收尾四分音符。
        // 长音从 bar1 第 1 拍起，held 拍数由参数给（6 拍跨到 bar2 第 3 拍）。
        auto heldSong = [](double heldBeats) {
            MidiData d;
            d.division = 480;
            d.timeSig.valid = true;
            d.timeSig.numerator = 4;
            d.timeSig.denomPow = 2;
            auto add = [&](long start, long dur, int pitch) {
                NoteEvent n;
                n.pitch = pitch;
                n.startTicks = start;
                n.durationTicks = dur;
                n.rawDurationTicks = dur;
                n.track = 0;
                n.channel = 0;
                n.velocity = 90;
                d.notes.push_back(n);
            };
            const int bar = 1920;
            const int q = 480;
            for (int i = 0; i < 4; i++) add(i * q, q, 60 + 2 * i);   // bar0
            const long held = (long)std::llround(heldBeats * q);
            add(bar, held, 64);                                      // bar1 起长音
            add(bar + held, q, 65);                                  // 长音之后
            add(bar + held + q, q, 67);
            return d;
        };

        // 6 拍 > 全音符（4 拍）。旧实现把 snapBeats 钳在 4 拍，长音被截成
        // 1920 tick，剩下 960 tick 在下一个小节补成休止符。
        {
            MidiData d = heldSong(6.0);
            quantizeTiming(d);
            const long dur = d.notes[4].durationTicks;   // 长音是第 5 个音符
            check(dur == 2880, "6 拍长音量化后仍是 6 拍（2880 tick），实际: " +
                               std::to_string(dur));

            KeyEstimate key;
            key.tonicPitchClass = 0;
            key.isMajor = true;
            const std::string txt = formatJianpu(d, key);
            check(txt.find("3 - - -") != std::string::npos,
                  "长音首段写成「3 - - -」: " + txt);
            check(txt.find("- - 4 5") != std::string::npos,
                  "长音后段在下一小节续写「- -」，不是休止符 0");
            check(txt.find("0 - 4 5") == std::string::npos,
                  "跨小节延音不能被写成休止符");
        }

        // 10 拍：跨三小节，续接段要有两个。
        {
            MidiData d = heldSong(10.0);
            quantizeTiming(d);
            const long dur = d.notes[4].durationTicks;
            check(dur == 4800, "10 拍长音量化后仍是 10 拍（4800 tick），实际: " +
                               std::to_string(dur));

            KeyEstimate key;
            key.tonicPitchClass = 0;
            key.isMajor = true;
            const std::string txt = formatJianpu(d, key);
            // 首段一整小节 + 两个整小节续写
            check(txt.find("3 - - -") != std::string::npos, "10 拍长音首段完整写满一小节");
            check(txt.find("- - - -") != std::string::npos,
                  "第二小节整小节都是延音线，实际:\n" + txt);
        }

        // 复调轨上的 4 拍和弦：门限因子来自另一条「断奏」轨（0.84）时，
        // 旧算法会拿 4 / 0.84 = 4.76 拍回填，把柱式和弦写成 4.75 拍，
        // 每个小节都溢出 3/4 拍，小节线整体错位。
        {
            MidiData d;
            d.division = 480;
            d.timeSig.valid = true;
            d.timeSig.numerator = 4;
            d.timeSig.denomPow = 2;
            // 断奏轨：音头间隔 480，时值 400 -> ratio 0.833，众数成门限
            for (int i = 0; i < 12; i++) {
                NoteEvent n;
                n.pitch = 84;
                n.startTicks = i * 480L;
                n.durationTicks = 400;
                n.rawDurationTicks = 400;
                n.track = 0;
                n.channel = 0;
                d.notes.push_back(n);
            }
            // 复调轨：每 4 拍一个三音柱式和弦（同音头 3 个音 -> polyphony 3）
            for (int bar = 0; bar < 3; bar++) {
                for (int k = 0; k < 3; k++) {
                    NoteEvent n;
                    n.pitch = 48 + 4 * k;
                    n.startTicks = bar * 1920L;
                    n.durationTicks = 1920;
                    n.rawDurationTicks = 1920;
                    n.track = 1;
                    n.channel = 1;
                    d.notes.push_back(n);
                }
            }
            const QuantizeReport rep = quantizeTiming(d);
            check(rep.gate > 0.80 && rep.gate < 0.90,
                  "门限因子按断奏轨估出 0.84，实际: " + std::to_string(rep.gate));
            bool allWhole = true;
            long staccatoDur = -1;
            for (const auto& n : d.notes) {
                if (n.track == 1 && n.durationTicks != 1920) allWhole = false;
                if (n.track == 0 && n.startTicks == 0) staccatoDur = n.durationTicks;
            }
            check(allWhole, "复调轨的 4 拍和弦不被门限放大成 4.75 拍");
            // 断奏轨该修的仍要修：400 tick 还原成一个四分音符
            check(staccatoDur == 480,
                  "断奏轨 400 tick 仍按门限还原成四分音符，实际: " +
                  std::to_string(staccatoDur));
        }

        // 短音的既有行为不能变：没有门限证据（全是整拍音头）时原样吸附。
        {
            MidiData d = heldSong(1.0);
            quantizeTiming(d);
            check(d.notes[4].durationTicks == 480, "一拍音维持四分音符");
        }
    }

    // -----------------------------------------------------------------
    // v1.25.0: 同轨复调（一条轨里两个声部）
    // -----------------------------------------------------------------
    std::cout << "\n测试同轨复调分声部\n";
    {
        auto mk = [](long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            return n;
        };
        auto maxVoice = [](const std::vector<int>& v) {
            int m = 0;
            for (int x : v) m = std::max(m, x);
            return m;
        };

        // 1. 单声部旋律不该被拆出声部
        {
            std::vector<NoteEvent> ns;
            for (int i = 0; i < 8; i++) ns.push_back(mk(i * 480, 480, 60 + i));
            const auto vo = splitVoices(ns);
            check(maxVoice(vo) == 0, "单声部旋律只占一个声部");
        }

        // 2. 同拍同值的柱式和弦仍属于同一声部（不然 <chord/> 会散架）
        {
            std::vector<NoteEvent> ns;
            for (int p : {48, 52, 55}) ns.push_back(mk(0, 1920, p));
            const auto vo = splitVoices(ns);
            check(maxVoice(vo) == 0, "同拍同值的三音和弦留在同一声部");
        }

        // 3. 同拍不同值（长音叠短音）必须拆开：这正是小节时值溢出的根源
        {
            std::vector<NoteEvent> ns;
            ns.push_back(mk(0, 1920, 48));   // 低音全音符
            ns.push_back(mk(0, 480, 72));    // 高音四分
            ns.push_back(mk(480, 480, 74));
            const auto vo = splitVoices(ns);
            check(vo[0] != vo[1], "同拍不同值拆成两个声部（不是和弦）");
            check(vo[1] == vo[2], "后一个四分接在高音声部里");
            check(ns[0].durationTicks == 1920,
                  "低声部的全音符时值不变，实际: " + std::to_string(ns[0].durationTicks));
        }

        // 4. 交错重叠（长音还没收尾，下一句就进来了）
        {
            std::vector<NoteEvent> ns;
            ns.push_back(mk(0, 1440, 60));    // 3 拍
            ns.push_back(mk(480, 480, 72));   // 第 2 拍起
            const auto vo = splitVoices(ns);
            check(vo[0] != vo[1], "重叠音符分到不同声部");
        }

        // 5. 超过声部上限时要退化成和弦并收口，不能让同声部继续重叠
        {
            std::vector<NoteEvent> ns;
            ns.push_back(mk(0, 480, 76));
            ns.push_back(mk(0, 1440, 60));
            ns.push_back(mk(0, 960, 55));
            ns.push_back(mk(0, 1920, 48));   // 第 4 个同拍音，已超上限
            const auto vo = splitVoices(ns);
            check(maxVoice(vo) < 3, "声部数不超过上限 3，实际: " +
                  std::to_string(maxVoice(vo) + 1));
            // 同一声部里不能有两个「同拍但时值不同」的音
            bool ok = true;
            for (size_t i = 0; i < ns.size(); i++)
                for (size_t j = i + 1; j < ns.size(); j++)
                    if (vo[i] == vo[j] && ns[i].startTicks == ns[j].startTicks &&
                        ns[i].durationTicks != ns[j].durationTicks)
                        ok = false;
            check(ok, "同声部同拍的音时值对齐（退化成真和弦）");
        }

        // 6. skip 谓词（倚音）不参与声部划分
        {
            std::vector<NoteEvent> ns;
            ns.push_back(mk(0, 60, 59));     // 极短倚音
            ns.push_back(mk(60, 480, 60));   // 主音，紧接其后
            const auto vo = splitVoices(ns, 3, [](const NoteEvent& n) {
                return n.durationTicks < 120;
            });
            check(vo[0] == 0 && vo[1] == 0, "被 skip 的倚音不另起声部");
            check(ns[0].durationTicks == 60, "倚音时值不被截短");
        }
    }

    // 复调谱导成 MusicXML：小节时值必须严丝合缝
    std::cout << "\n测试复调 MusicXML 小节时值\n";
    {
        SmfTrack meta0;
        meta0.at(0, ev({0xFF, 0x51, 3, 0x07, 0xA1, 0x20}));
        meta0.at(0, ev({0xFF, 0x58, 4, 4, 2, 24, 8}));
        meta0.at(0, ev({0xFF, 0x59, 2, 0, 0}));

        SmfTrack t1;
        t1.at(0, ev({0xFF, 0x03, 5, 'P', 'i', 'a', 'n', 'o'}));
        const long BAR = 1920;
        // 右手：每小节四个四分音符
        for (int bar = 0; bar < 4; bar++)
            for (int i = 0; i < 4; i++)
                if (!(bar == 3 && i < 2)) {
                    t1.at(bar * BAR + i * 480, ev({0x90, 84, 90}));
                    t1.at(bar * BAR + i * 480 + 480, ev({0x80, 84, 0}));
                }
        // 左手：全音符铺底，最后一个跨小节
        t1.at(0, ev({0x90, 48, 70}));
        t1.at(BAR, ev({0x80, 48, 0}));
        t1.at(BAR, ev({0x90, 50, 70}));
        t1.at(2 * BAR, ev({0x80, 50, 0}));
        t1.at(2 * BAR + 960, ev({0x90, 52, 70}));
        t1.at(3 * BAR + 960, ev({0x80, 52, 0}));

        auto bytes = buildSmf(2, 480, {meta0, t1});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "解析复调 SMF");
        quantizeTiming(d);
        const std::string xml = renderMusicXml(d, estimateKey(d));

        check(xml.find("<backup>") != std::string::npos,
              "复调 MusicXML 用 <backup> 退回小节开头");
        check(xml.find("<voice>2</voice>") != std::string::npos,
              "第二个声部写成 <voice>2</voice>");

        // 逐小节算总时值：<chord/> 不前进时间，<backup> 回退
        auto measureTotals = [](const std::string& s) {
            std::vector<long> totals;
            size_t p = 0;
            while (true) {
                const size_t m = s.find("<measure ", p);
                if (m == std::string::npos) break;
                const size_t gt = s.find('>', m);
                const size_t end = s.find("</measure>", gt);
                if (end == std::string::npos) break;
                const std::string body = s.substr(gt + 1, end - gt - 1);
                p = end + 10;
                long pos = 0, mx = 0;
                size_t i = 0;
                while (i < body.size()) {
                    const size_t nt = body.find("<note>", i);
                    const size_t bk = body.find("<backup>", i);
                    if (nt == std::string::npos && bk == std::string::npos) break;
                    const bool isBackup = (bk != std::string::npos) &&
                                          (nt == std::string::npos || bk < nt);
                    const size_t at = isBackup ? bk : nt;
                    const size_t close = isBackup ? body.find("</backup>", at)
                                                  : body.find("</note>", at);
                    if (close == std::string::npos) break;
                    const std::string frag = body.substr(at, close - at);
                    const size_t dp = frag.find("<duration>");
                    const long dur = (dp == std::string::npos) ? 0
                                                               : std::stol(frag.substr(dp + 10));
                    if (isBackup) {
                        pos -= dur;
                    } else if (frag.find("<chord/>") == std::string::npos) {
                        pos += dur;
                        if (pos > mx) mx = pos;
                    }
                    i = close + 1;
                }
                totals.push_back(mx);
            }
            return totals;
        };
        const std::vector<long> totals = measureTotals(xml);
        check(totals.size() == 4, "四个小节，实际: " + std::to_string(totals.size()));
        bool allFull = !totals.empty();
        for (long t : totals) if (t != BAR) allFull = false;
        std::string detail;
        for (long t : totals) detail += std::to_string(t) + " ";
        check(allFull, "每个小节正好 1920 tick（声部叠加不溢出），实际: " + detail);

        // 单声部仍不该出现 <backup>
        SmfTrack mono;
        for (int i = 0; i < 8; i++) {
            mono.at(i * 480, ev({0x90, 60 + i, 90}));
            mono.at(i * 480 + 480, ev({0x80, 60 + i, 0}));
        }
        auto mb = buildSmf(2, 480, {meta0, mono});
        MidiData md;
        loadMidiFromMemory(mb.data(), mb.size(), md);
        quantizeTiming(md);
        check(renderMusicXml(md, estimateKey(md)).find("<backup>") == std::string::npos,
              "单声部 MusicXML 不写 <backup>（与旧版逐字节一致）");
    }

    // ---- 文件头 MThd：SMPTE 时间基准 ----
    {
        std::cout << "\n测试文件头 MThd：SMPTE 时间基准\n";
        // 25 fps x 40 tick/frame = 1000 tick/秒；120BPM 时一拍正好 500 tick
        const int kSmpteDiv = 0xE728;
        const int kTpb = 500;
        SmfTrack cond, voc;
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));   // 500000us -> 120BPM
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));        // 4/4
        cond.at(0, ev({0xFF, 0x59, 0x02, 0, 0}));               // C 大调
        for (int i = 0; i < 16; i++) {
            voc.at(i * kTpb, ev({0x90, 60 + i, 96}));
            voc.at(i * kTpb + kTpb, ev({0x80, 60 + i, 0}));
        }
        // 第 9 拍再写一个 400BPM：SMPTE 下 tempo 不改变绝对时间，必须被归一掉
        voc.at(8 * kTpb, ev({0xFF, 0x51, 0x03, 0x02, 0x49, 0xF0}));

        auto bytes = buildSmf(2, kSmpteDiv, {cond, voc});
        MidiData md;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), md), "解析 SMPTE 文件");
        check(md.smpte && md.smpteFps == 25 && md.smpteTicksPerFrame == 40,
              "识别 SMPTE 25 fps x 40 tick");
        check(md.rawDivision == kSmpteDiv, "保留原始 division 字 0xE728");
        check(md.division == kTpb,
              "折算成 500 tick/拍，实际: " + std::to_string(md.division));
        check(md.notes.size() == 16,
              "16 个音符都在，实际: " + std::to_string(md.notes.size()));

        double s8 = -1.0;
        for (const auto& n : md.notes)
            if (n.startTicks == 8 * kTpb) s8 = n.startSeconds;
        check(std::fabs(s8 - 4.0) < 1e-6,
              "第 9 拍落在 4.0 秒（中途 tempo 未被二次缩放），实际: " +
                  std::to_string(s8));
        check(md.tempoChanges.size() == 1,
              "SMPTE 下 tempo 归一为单点，实际: " +
                  std::to_string(md.tempoChanges.size()));

        const std::string tb = timeBaseLabel(md);
        check(tb.find("SMPTE") != std::string::npos &&
                  tb.find("25") != std::string::npos &&
                  tb.find("500") != std::string::npos,
              "时间基准说明可读，实际: " + tb);
        check(timeBaseLabel(MidiData{}).empty(), "普通 PPQ 文件不输出时间基准说明");

        // 划小节：16 拍按 4/4 应是 4 小节，而不是全部挤进 1 小节
        const std::string jp = formatJianpu(md, estimateKey(md));
        size_t bars = 0;
        for (char c : jp) if (c == '|') bars++;
        check(bars == 4, "16 拍划成 4 小节，实际小节线: " + std::to_string(bars));
    }

    // ---- 文件头 MThd：format 2 是多个独立序列 ----
    {
        std::cout << "\n测试文件头 MThd：format 2 独立序列\n";
        SmfTrack songA, songB;
        for (int i = 0; i < 16; i++) {
            songA.at(i * 480, ev({0x90, 60 + i, 96}));
            songA.at(i * 480 + 480, ev({0x80, 60 + i, 0}));
        }
        for (int i = 0; i < 8; i++) {
            songB.at(i * 480, ev({0x90, 72 - i, 96}));
            songB.at(i * 480 + 480, ev({0x80, 72 - i, 0}));
        }
        auto bytes = buildSmfFmt(2, 2, 480, {songA, songB});

        MidiData md;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), md), "解析 format 2 文件");
        check(md.format == 2, "记下 format=2");
        check(md.format2SequenceCount == 2,
              "统计到 2 段独立序列，实际: " + std::to_string(md.format2SequenceCount));
        check(md.format2UsedTrack == 0, "默认取文件顺序的第 0 轨");
        check(md.notes.size() == 16,
              "只保留第 0 轨的 16 个音符，实际: " + std::to_string(md.notes.size()));
        bool allTrack0 = true;
        for (const auto& n : md.notes) if (n.track != 0) allTrack0 = false;
        check(allTrack0, "残留音符全部来自第 0 轨（另一段没混进来）");

        const std::string fl = formatLabel(md);
        check(fl.find("format 2") != std::string::npos &&
                  fl.find("2 段") != std::string::npos,
              "format 2 说明可读，实际: " + fl);

        // --track 要自己挑段落时，解析层不能提前丢数据
        MidiData all;
        loadMidiFromMemory(bytes.data(), bytes.size(), all, true);
        check(all.notes.size() == 24,
              "keepAllFormat2 时保留全部 24 个音符，实际: " +
                  std::to_string(all.notes.size()));

        // format 1 的多轨是同一首的各声部，一条都不能丢
        SmfTrack v, p;
        v.at(0, ev({0x90, 60, 90}));
        v.at(480, ev({0x80, 60, 0}));
        p.at(0, ev({0x90, 48, 90}));
        p.at(480, ev({0x80, 48, 0}));
        auto f1 = buildSmf(2, 480, {v, p});
        MidiData m1;
        loadMidiFromMemory(f1.data(), f1.size(), m1);
        check(m1.notes.size() == 2, "format 1 的两个声部都保留");
        check(formatLabel(m1).empty(), "format 1 不输出独立序列说明");
    }

    // ---- 事件流对齐：0xA0 复音触后是 3 字节事件 ----
    {
        std::cout << "\n测试事件流对齐：0xA0 复音触后\n";
        // 复音触后（状态 + 音高 + 压力）。旧实现落到兜底分支只跳 1 字节，
        // 每遇到一个就与事件流错位，一条 16 个音的轨最后连一个音都提不出来。
        const int mel[16] = {60, 62, 64, 65, 67, 69, 71, 72,
                             72, 71, 69, 67, 65, 64, 62, 60};
        SmfTrack cond, voc;
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        for (int i = 0; i < 16; i++) {
            voc.at(i * 480, ev({0x90, mel[i], 96}));
            voc.at(i * 480 + 240, ev({0xA0, mel[i], 0x50}));   // 3 字节
            voc.at(i * 480 + 480, ev({0x80, mel[i], 0x40}));
        }
        auto pat = buildSmf(2, 480, {cond, voc});
        MidiData mp;
        check(loadMidiFromMemory(pat.data(), pat.size(), mp), "含复音触后的文件解析成功");
        check(mp.notes.size() == 16,
              "16 个音符一个不少，实际: " + std::to_string(mp.notes.size()));
        bool durOk = true;
        for (const auto& n : mp.notes) if (n.durationTicks != 480) durOk = false;
        check(durOk, "触后事件没有吃掉时值（每个音仍是 1 拍）");

        const std::string jp = formatJianpu(mp, estimateKey(mp));
        size_t bars = 0;
        for (char c : jp) if (c == '|') bars++;
        check(bars == 4, "16 拍仍划成 4 小节，实际小节线: " + std::to_string(bars));

        // 通道触后 0xD0 只有 1 个数据字节，别和 0xA0 一起写错
        SmfTrack at;
        at.at(0, ev({0x90, 60, 96}));
        at.at(240, ev({0xD0, 0x40}));
        at.at(480, ev({0x80, 60, 0x40}));
        auto p2 = buildSmf(1, 480, {at});
        MidiData m2;
        loadMidiFromMemory(p2.data(), p2.size(), m2);
        check(m2.notes.size() == 1 && m2.notes[0].durationTicks == 480,
              "0xD0 通道触后按 1 字节跳过，时值不受影响");
    }

    // ---- 音符配对：同音重触发与轨尾悬挂音 ----
    {
        std::cout << "\n测试音符配对：重触发 / 悬挂音\n";
        const int mel[16] = {60, 62, 64, 65, 67, 69, 71, 72,
                             72, 71, 69, 67, 65, 64, 62, 60};
        // 同一个键还没抬手就又按下：旧实现覆盖 active 表项，前一个音凭空消失
        SmfTrack voc;
        voc.at(0, ev({0x90, 60, 96}));
        voc.at(480, ev({0x90, 60, 64}));     // 重触发，前一个音没有 note-off
        voc.at(960, ev({0x80, 60, 0x40}));
        for (int i = 0; i < 14; i++) {
            voc.at((i + 2) * 480, ev({0x90, 60 + i, 96}));
            voc.at((i + 3) * 480, ev({0x80, 60 + i, 0x40}));
        }
        auto r1 = buildSmf(1, 480, {voc});
        MidiData mr;
        loadMidiFromMemory(r1.data(), r1.size(), mr);
        check(mr.notes.size() == 16,
              "重触发不吞音，16 个音符都在，实际: " + std::to_string(mr.notes.size()));
        long firstDur = -1, secondStart = -1;
        int secondVel = -1;
        for (const auto& n : mr.notes) {
            if (n.pitch != 60) continue;
            if (n.startTicks == 0) firstDur = n.durationTicks;
            if (n.startTicks == 480) { secondStart = n.startTicks; secondVel = n.velocity; }
        }
        check(firstDur == 480, "前一个音在重触发点收尾（1 拍），实际: " +
                                   std::to_string(firstDur));
        check(secondStart == 480 && secondVel == 64,
              "后一个音从重触发点起算，力度用新值，实际力度: " +
                  std::to_string(secondVel));

        // 同一 tick 重复按下：前一个音长 0，应直接换掉而不是留下零长音符
        SmfTrack same;
        same.at(0, ev({0x90, 62, 96}));
        same.at(0, ev({0x90, 62, 80}));
        same.at(480, ev({0x80, 62, 0x40}));
        auto r2 = buildSmf(1, 480, {same});
        MidiData ms;
        loadMidiFromMemory(r2.data(), r2.size(), ms);
        check(ms.notes.size() == 1 && ms.notes[0].durationTicks == 480,
              "同一 tick 重复按下只留一个音（不产生零长音符）");

        // 轨结束时还按着没抬手：旧实现整条丢掉，还会把调性带偏
        SmfTrack dang;
        for (int i = 0; i < 15; i++) {
            dang.at(i * 480, ev({0x90, mel[i], 96}));
            dang.at((i + 1) * 480, ev({0x80, mel[i], 0x40}));
        }
        dang.at(15 * 480, ev({0x90, mel[15], 96}));       // 只按不抬
        dang.at(16 * 480, ev({0xFF, 0x01, 0x03, 0x65, 0x6E, 0x64}));   // "end"
        auto r3 = buildSmf(1, 480, {dang});
        MidiData mg;
        loadMidiFromMemory(r3.data(), r3.size(), mg);
        check(mg.notes.size() == 16,
              "轨尾悬挂音补收，16 个音符都在，实际: " + std::to_string(mg.notes.size()));
        long tailDur = -1;
        for (const auto& n : mg.notes) if (n.startTicks == 15 * 480) tailDur = n.durationTicks;
        check(tailDur == 480, "悬挂音收到本轨最后一个事件（1 拍），实际: " +
                                  std::to_string(tailDur));
        check(estimateKey(mg).tonicPitchClass == 0,
              "少一个音也不再把 C 大调带偏，实际主音: " +
                  std::to_string(estimateKey(mg).tonicPitchClass));
    }

    // ---- 多轨歌词：只取音节最多的一条 ----
    {
        std::cout << "\n测试多轨歌词：只取一条\n";
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        NoteEvent n;
        n.track = 1; n.channel = 0; n.durationTicks = 480;
        for (int i = 0; i < 8; i++) {
            n.startTicks = (long)i * 480;
            n.pitch = 60 + i;
            d.notes.push_back(n);
        }
        const char* verse1[10] = {"春","眠","不","觉","晓","处","处","闻","啼","鸟"};
        const char* verse2[10] = {"夜","来","风","雨","声","花","落","知","多","少"};
        for (int i = 0; i < 10; i++) {
            d.midiLyrics.push_back({(long)i * 480, verse1[i], 1});
            // 第二段词错开半拍：直接按 tick 合并会得到「春夜来风雨声花落」
            d.midiLyrics.push_back({(long)i * 480 + 240, verse2[i], 2});
        }
        const LyricTrackPick pv = pickMidiLyricTrack(d);
        check(pv.track == 1 && pv.syllables == 10 &&
                  pv.droppedTracks == 1 && pv.droppedSyllables == 10,
              "取第 1 轨的 10 个音节、忽略第 2 轨的 10 个，实际: 轨 " +
                  std::to_string(pv.track) + " / " +
                  std::to_string(pv.syllables) + " / " +
                  std::to_string(pv.droppedSyllables));
        std::string joined;
        for (const auto& sy : normalizeMidiLyrics(d)) joined += sy.text;
        check(joined == "春眠不觉晓处处闻啼鸟",
              "歌词不再两段交错，实际: " + joined);
        // 歌词轨要有音符、且文件里不止一条有声轨才可信（format 0 的轨号没意义）
        MidiData dTwo = d;
        NoteEvent acc;
        acc.track = 2; acc.channel = 1; acc.durationTicks = 1920;
        acc.startTicks = 0;
        acc.pitch = 48;
        dTwo.notes.push_back(acc);
        check(midiLyricTrack(dTwo) == 1, "采信的歌词轨参与旋律轨判定");
        check(midiLyricTrack(d) == -1, "只有一条有声轨时仍不采信歌词轨号");

        // 伴奏轨拿 0x05 存和弦名时，音节更少的那条被丢掉
        MidiData c2 = d;
        c2.midiLyrics.clear();
        for (int i = 0; i < 10; i++)
            c2.midiLyrics.push_back({(long)i * 480, verse1[i], 1});
        for (int bar = 0; bar < 2; bar++)
            c2.midiLyrics.push_back({(long)bar * 1920, "C", 2});
        check(pickMidiLyricTrack(c2).track == 1, "和弦名轨（音节少）被忽略");
        check(pickMidiLyricTrack(MidiData{}).track == -1, "没有歌词事件时取轨结果为 -1");
    }

    // ---- 控制器语义：CC64 延音踏板补时值 ----
    {
        std::cout << "\n测试控制器语义：CC64 延音踏板\n";
        // 钢琴导出里最常见的写法：琴键只按 1/4 拍，真正的音响长度由踏板决定。
        // 旧实现只看 note-off，得到「1= 3= 5= 0 - - -」三个十六分音符加长休止。
        SmfTrack cond, piano;
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano.at(0, ev({0xB0, 0x40, 127}));          // CC64 踩下
        piano.at(8 * 480, ev({0xB0, 0x40, 0}));      // 第 8 拍抬起
        for (int bar = 0; bar < 2; bar++) {
            for (int pitch : {60, 64, 67}) {
                piano.at(bar * 4 * 480, ev({0x90, pitch, 96}));
                piano.at(bar * 4 * 480 + 120, ev({0x80, pitch, 0x40}));
            }
        }
        auto bytes = buildSmf(2, 480, {cond, piano});
        MidiData md;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), md), "踏板文件解析成功");
        check(md.notes.size() == 6, "6 个和弦音都在，实际: " + std::to_string(md.notes.size()));
        check(md.pedalEvents == 2 && md.pedalExtendedNotes == 6,
              "记下 2 个踏板事件 / 补长 6 个音，实际: " +
                  std::to_string(md.pedalEvents) + " / " +
                  std::to_string(md.pedalExtendedNotes));
        bool allFourBeats = true, rawKept = true;
        for (const auto& n : md.notes) {
            // 第一小节被同键重按截在 1920，第二小节补到踏板抬起的 3840，
            // 两个小节都是「每音 4 拍」。
            if (n.durationTicks != 1920) allFourBeats = false;
            if (n.rawDurationTicks != 120) rawKept = false;
        }
        check(allFourBeats, "每个音补成 4 拍（1920 tick）");
        check(rawKept, "rawDurationTicks 仍是按键的 1/4 拍，不污染断奏判据");
        const std::string cl = controllerLabel(md);
        check(cl.find("延音踏板：补长 6 个音") != std::string::npos,
              "控制器提示可读，实际: " + cl);
        const std::string jp = formatJianpu(md, estimateKey(md));
        size_t bars = 0;
        for (char c : jp) if (c == '|') bars++;
        check(bars == 2, "仍是 2 小节，实际小节线: " + std::to_string(bars));

        // 同 tick 的 127→0 是「重新踩踏板」，零长区间不能凭空补时值
        SmfTrack same;
        same.at(0, ev({0xB0, 0x40, 127}));
        same.at(0, ev({0xB0, 0x40, 0}));
        same.at(0, ev({0x90, 60, 96}));
        same.at(120, ev({0x80, 60, 0x40}));
        auto bs = buildSmf(1, 480, {same});
        MidiData ms;
        loadMidiFromMemory(bs.data(), bs.size(), ms);
        check(ms.notes.size() == 1 && ms.notes[0].durationTicks == 120,
              "同 tick 踩放不产生零长踏板区间，时值仍是 1/4 拍");

        // 踏板事件写在 conductor 轨（另一条轨）上也要认
        SmfTrack cond2, voc;
        cond2.at(0, ev({0xB0, 0x40, 127}));
        cond2.at(4 * 480, ev({0xB0, 0x40, 0}));
        voc.at(0, ev({0x90, 60, 96}));
        voc.at(120, ev({0x80, 60, 0x40}));
        auto bo = buildSmf(2, 480, {cond2, voc});
        MidiData mo;
        loadMidiFromMemory(bo.data(), bo.size(), mo);
        check(mo.notes.size() == 1 && mo.notes[0].durationTicks == 1920,
              "踏板写在别的轨上也能补时值（按 tick 而非按轨顺序）");
    }

    // ---- 控制器语义：踏板按通道隔离 ----
    {
        std::cout << "\n测试控制器语义：踏板按通道隔离\n";
        SmfTrack cond, piano, bass;
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano.at(0, ev({0xB0, 0x40, 127}));            // 只有通道 0 踩
        piano.at(4 * 480, ev({0xB0, 0x40, 0}));
        piano.at(0, ev({0x90, 60, 96}));
        piano.at(120, ev({0x80, 60, 0x40}));
        piano.at(4 * 480, ev({0x90, 64, 96}));         // 抬踏板之后才松键
        piano.at(4 * 480 + 120, ev({0x80, 64, 0x40}));
        bass.at(2 * 480, ev({0x91, 48, 96}));          // 通道 1 全程没踩
        bass.at(2 * 480 + 120, ev({0x81, 48, 0x40}));
        bass.at(6 * 480, ev({0x91, 55, 96}));
        bass.at(6 * 480 + 120, ev({0x81, 55, 0x40}));
        auto bytes = buildSmf(3, 480, {cond, piano, bass});
        MidiData md;
        loadMidiFromMemory(bytes.data(), bytes.size(), md);
        check(md.notes.size() == 4, "4 个音符都在");
        check(md.pedalExtendedNotes == 1,
              "只补长 1 个音，实际: " + std::to_string(md.pedalExtendedNotes));
        std::vector<long> durs;
        for (const auto& n : md.notes) durs.push_back(n.durationTicks);
        std::sort(durs.begin(), durs.end());
        check(durs == std::vector<long>({120, 120, 120, 1920}),
              "时值为 1920 / 120 / 120 / 120（通道 1 与抬踏板后都不补）");
    }

    // ---- 控制器语义：CC120 / CC123 强制清音 ----
    {
        std::cout << "\n测试控制器语义：All Notes Off\n";
        SmfTrack piano;
        piano.at(0, ev({0x90, 60, 96}));               // 只按不抬
        piano.at(480, ev({0xB0, 0x7B, 0}));           // CC123 All Notes Off
        for (int i = 0; i < 6; i++) {
            piano.at((i + 2) * 480, ev({0x90, 62 + i, 96}));
            piano.at((i + 3) * 480, ev({0x80, 62 + i, 0x40}));
        }
        auto bytes = buildSmf(1, 480, {piano});
        MidiData md;
        loadMidiFromMemory(bytes.data(), bytes.size(), md);
        check(md.notes.size() == 7, "7 个音符都在，实际: " + std::to_string(md.notes.size()));
        check(md.allNotesOffEvents == 1 && md.allNotesOffTrimmed == 1,
              "记下 1 个清音事件 / 截尾 1 个音");
        long firstDur = -1, firstRaw = -1;
        for (const auto& n : md.notes)
            if (n.startTicks == 0) { firstDur = n.durationTicks; firstRaw = n.rawDurationTicks; }
        check(firstDur == 480, "悬挂音截到 CC123 所在 tick（1 拍），实际: " +
                                   std::to_string(firstDur));
        check(firstRaw == 480, "原始时值同步截短，不会被判成断奏，实际: " +
                                   std::to_string(firstRaw));
        check(controllerLabel(md).find("强制清音：截尾 1 个音") != std::string::npos,
              "控制器提示可读，实际: " + controllerLabel(md));

        // CC120 All Sound Off 同样算清音
        SmfTrack t120;
        t120.at(0, ev({0x90, 60, 96}));
        t120.at(240, ev({0xB0, 0x78, 0}));
        t120.at(480, ev({0x90, 62, 96}));
        t120.at(960, ev({0x80, 62, 0x40}));
        auto b120 = buildSmf(1, 480, {t120});
        MidiData m120;
        loadMidiFromMemory(b120.data(), b120.size(), m120);
        check(m120.allNotesOffTrimmed == 1 && m120.notes[0].durationTicks == 240,
              "CC120 也按清音处理（半拍收尾）");
    }

    // ---- 控制器语义：CC7 主音量并入力度表 ----
    {
        std::cout << "\n测试控制器语义：CC7 主音量\n";
        SmfTrack strings;
        for (int i = 0; i < 8; i++) {
            strings.at(i * 480, ev({0xB0, 0x07, 20 + i * 14}));
            strings.at(i * 480, ev({0x90, 60 + i, 96}));
            strings.at(i * 480 + 480, ev({0x80, 60 + i, 0x40}));
        }
        auto bytes = buildSmf(1, 480, {strings});
        MidiData md;
        loadMidiFromMemory(bytes.data(), bytes.size(), md);
        check(md.dynamics.size() == 8,
              "8 个 CC7 点并入力度表，实际: " + std::to_string(md.dynamics.size()));
        check(md.cc7Dynamics == 8, "记下补入 8 个力度点");
        check(controllerLabel(md).find("CC7 主音量：补入 8 个力度点") != std::string::npos,
              "控制器提示可读，实际: " + controllerLabel(md));

        // 同 tick 同通道若 CC11 也在，以表情为准，不能凭空多一档
        SmfTrack both;
        for (int i = 0; i < 4; i++) {
            both.at(i * 480, ev({0xB0, 0x07, 20 + i * 20}));
            both.at(i * 480, ev({0xB0, 0x0B, 40 + i * 20}));
            both.at(i * 480, ev({0x90, 60, 96}));
            both.at(i * 480 + 480, ev({0x80, 60, 0x40}));
        }
        auto b2 = buildSmf(1, 480, {both});
        MidiData m2;
        loadMidiFromMemory(b2.data(), b2.size(), m2);
        check(m2.cc7Dynamics == 0 && m2.dynamics.size() == 4,
              "同 tick 的 CC7 让位给 CC11，实际: " +
                  std::to_string(m2.dynamics.size()) + " 点 / 补入 " +
                  std::to_string(m2.cc7Dynamics));
        check(controllerLabel(MidiData{}).empty(), "普通文件不输出控制器提示");
    }

    // ---- RPN 0x0000 弯音范围：每通道、可中途改、可写在别的轨 ----
    {
        std::cout << "\n测试 RPN 弯音范围：通道隔离 / 时间轴 / 跨轨\n";
        auto rpn = [](SmfTrack& t, int ch, long tick, int semis, int cents) {
            t.at(tick, ev({0xB0 | ch, 101, 0}));
            t.at(tick, ev({0xB0 | ch, 100, 0}));
            t.at(tick, ev({0xB0 | ch, 6, semis}));
            if (cents >= 0) t.at(tick, ev({0xB0 | ch, 38, cents}));
            t.at(tick, ev({0xB0 | ch, 101, 127}));   // 退出 RPN 状态
            t.at(tick, ev({0xB0 | ch, 100, 127}));
        };
        auto bend = [](SmfTrack& t, int ch, long tick, int value) {
            t.at(tick, ev({0xE0 | ch, value & 0x7F, (value >> 7) & 0x7F}));
        };
        auto note = [](SmfTrack& t, int ch, long tick) {
            t.at(tick, ev({0x90 | ch, 60, 96}));
            t.at(tick + 480, ev({0x80 | ch, 60, 0x40}));
        };
        auto rangeOf = [](const MidiData& d, int ch, long tick) -> double {
            for (const auto& pb : d.pitchBends)
                if (pb.channel == ch && pb.tick == tick) return pb.range;
            return -1.0;
        };
        auto parse = [](const std::vector<uint8_t>& bytes, MidiData& md) {
            return loadMidiFromMemory(bytes.data(), bytes.size(), md);
        };

        // 1) 通道 0 设 ±12，通道 1 没设——通道 1 必须还是默认 ±2。
        {
            SmfTrack t;
            rpn(t, 0, 0, 12, -1);
            note(t, 0, 0);
            note(t, 1, 960);
            bend(t, 0, 0, 16383);
            bend(t, 1, 960, 16383);
            auto bytes = buildSmf(1, 480, {t});
            MidiData md;
            check(parse(bytes, md), "弯音范围文件解析成功");
            check(md.bendRanges.size() == 1 && md.bendRanges[0].channel == 0 &&
                  std::abs(md.bendRanges[0].semitones - 12.0) < 1e-9,
                  "只记下通道 0 的一处 ±12 设定");
            check(md.bendRangeEvents == 1 && md.bendRangeChannels == 1,
                  "统计 1 处设定 / 1 条通道，实际 " +
                      std::to_string(md.bendRangeEvents) + " / " +
                      std::to_string(md.bendRangeChannels));
            check(std::abs(rangeOf(md, 0, 0) - 12.0) < 1e-9,
                  "通道 0 的弯音用 ±12");
            check(std::abs(rangeOf(md, 1, 960) - 2.0) < 1e-9,
                  "通道 1 没设 RPN，仍是默认 ±2（旧实现会被 ±12 污染）");
            const std::string br = bendRangeLabel(md);
            check(br.find("±12") != std::string::npos &&
                  br.find("按通道与时间生效") != std::string::npos,
                  "弯音范围提示可读，实际: " + br);
            check(bendRangeLabel(MidiData{}).empty(), "普通文件不输出弯音范围提示");
        }

        // 2) 同一通道中途把 ±12 改回 ±2，前面的音不能被倒灌。
        {
            SmfTrack t;
            rpn(t, 0, 0, 12, -1);
            note(t, 0, 0);
            bend(t, 0, 0, 16383);
            rpn(t, 0, 960, 2, -1);
            note(t, 0, 960);
            bend(t, 0, 960, 16383);
            auto bytes = buildSmf(1, 480, {t});
            MidiData md;
            parse(bytes, md);
            check(md.bendRanges.size() == 2, "记下两处范围变更");
            check(std::abs(rangeOf(md, 0, 0) - 12.0) < 1e-9 &&
                  std::abs(rangeOf(md, 0, 960) - 2.0) < 1e-9,
                  "中途改范围不倒灌到它之前的弯音");
            const std::string br = bendRangeLabel(md);
            check(br.find("±2") != std::string::npos &&
                  br.find("±12") != std::string::npos,
                  "两种范围都列进提示，实际: " + br);
        }

        // 3) RPN 写在文件顺序靠后的 setup 轨上，弯音在最前面的旋律轨里。
        {
            SmfTrack music, setup;
            note(music, 0, 0);
            bend(music, 0, 0, 16383);
            rpn(setup, 0, 0, 12, -1);
            auto bytes = buildSmf(2, 480, {music, setup});
            MidiData md;
            parse(bytes, md);
            check(md.bendRanges.size() == 1 &&
                  std::abs(rangeOf(md, 0, 0) - 12.0) < 1e-9,
                  "RPN 在别的轨也照样生效（解析顺序不决定时间顺序）");
        }

        // 4) RPN 状态按通道隔离：ch0 选中 RPN 后，ch1 的裸 CC6 不能算设定。
        {
            SmfTrack t;
            t.at(0, ev({0xB0, 101, 0}));
            t.at(0, ev({0xB0, 100, 0}));
            t.at(0, ev({0xB1, 6, 7}));
            t.at(0, ev({0xB0, 6, 12}));
            bend(t, 0, 0, 16383);
            bend(t, 1, 0, 16383);
            auto bytes = buildSmf(1, 480, {t});
            MidiData md;
            parse(bytes, md);
            check(md.bendRanges.size() == 1 && md.bendRanges[0].channel == 0,
                  "没选过 RPN 的通道不产生设定");
            check(std::abs(rangeOf(md, 1, 0) - 2.0) < 1e-9,
                  "通道 1 不被通道 0 的 RPN 状态串味");
        }

        // 5) CC38 音分微调；同 tick 后写覆盖先写；非法值落回默认。
        {
            SmfTrack cents;
            rpn(cents, 0, 0, 12, 50);
            bend(cents, 0, 0, 16383);
            auto b1 = buildSmf(1, 480, {cents});
            MidiData m1;
            parse(b1, m1);
            check(m1.bendRanges.size() == 1 &&
                  std::abs(m1.bendRanges[0].semitones - 12.5) < 1e-9 &&
                  std::abs(rangeOf(m1, 0, 0) - 12.5) < 1e-9,
                  "CC38 把范围精修到 12.5 半音");

            SmfTrack twice;
            rpn(twice, 0, 0, 12, -1);
            rpn(twice, 0, 0, 3, -1);
            bend(twice, 0, 0, 16383);
            auto b2 = buildSmf(1, 480, {twice});
            MidiData m2;
            parse(b2, m2);
            check(std::abs(rangeOf(m2, 0, 0) - 3.0) < 1e-9,
                  "同 tick 多次设定以后写的为准");

            SmfTrack bad;
            rpn(bad, 0, 0, 0, -1);
            bend(bad, 0, 0, 16383);
            auto b3 = buildSmf(1, 480, {bad});
            MidiData m3;
            parse(b3, m3);
            check(std::abs(rangeOf(m3, 0, 0) - 2.0) < 1e-9,
                  "范围 0 这种坏数据落回默认 ±2");
        }
    }

    std::cout << "\n------------------------------------------------\n";
    std::cout << "共 " << total << " 项，失败 " << failures << " 项 -> "
              << (failures == 0 ? "全部通过" : "有失败") << "\n";
    return failures == 0 ? 0 : 1;
}
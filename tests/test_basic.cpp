// test_basic.cpp - 核心逻辑单元测试
// 覆盖：音高->简谱、八度基准、升降号、时值后缀、三连音、调性识别、
//       和弦检测、同音合并、MusicXML 相关的基础数据结构、
//       鼓轨过滤、旋律轨判定、变速（tempo map）换算。
#include "midi2jianpu/notation.hpp"

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

static std::vector<uint8_t> buildSmf(int ntrks, int division, std::vector<SmfTrack> tracks) {
    std::vector<uint8_t> out;
    const char hdr[] = {'M','T','h','d'};
    out.insert(out.end(), hdr, hdr + 4);
    put32(out, 6);
    put16(out, 1);
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
        MidiData d4 = d;
        d4.midiLyrics[1].track = 2;
        check(midiLyricTrack(d4) == -1, "歌词分散在多轨时不采信歌词轨号");

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

    std::cout << "\n------------------------------------------------\n";
    std::cout << "共 " << total << " 项，失败 " << failures << " 项 -> "
              << (failures == 0 ? "全部通过" : "有失败") << "\n";
    return failures == 0 ? 0 : 1;
}
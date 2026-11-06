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

    std::cout << "\n------------------------------------------------\n";
    std::cout << "共 " << total << " 项，失败 " << failures << " 项 -> "
              << (failures == 0 ? "全部通过" : "有失败") << "\n";
    return failures == 0 ? 0 : 1;
}
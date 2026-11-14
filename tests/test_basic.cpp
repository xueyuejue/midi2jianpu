// test_basic.cpp - 核心逻辑单元测试
// 覆盖：音高->简谱、八度基准、升降号、时值后缀、三连音、调性识别、
//       和弦检测、同音合并、MusicXML 相关的基础数据结构、
//       鼓轨过滤、旋律轨判定、变速（tempo map）换算、内置歌词规整、
//       拍号 map 与逐小节网格、调号 map（中途转调 / KeyMap）、
//       五声调式（宫商角徵羽）宫音判定与校正、tempo map 时间轴与 LRC 对齐、
//       SysEx 里的 GS/XG 鼓模式与 GM/GS/XG Reset、--track 按轨隔离、
//       截断 / 损坏 MIDI 的边界安全、SMF chunk 级边界重同步、
//       MusicXML 休止符时值与 <type>/<dot/> 的严格一致。
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "../src/text_encoding.hpp"
#include "../src/cli_args.hpp"

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

// 统计子串出现次数：多处断言用它确认「标志只写一次 / 每个档位都写出来」。
// 局部同名 lambda 会遮蔽它，不影响既有用例。
static size_t occurrences(const std::string& s, const std::string& sub) {
    size_t c = 0, p = 0;
    while ((p = s.find(sub, p)) != std::string::npos) { c++; p += sub.size(); }
    return c;
}

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
    {
        // --pickup 的手动值必须在入口就拦下越界数。0 拍不是「零长度弱起」；
        // 等于或超过整小节的值会被 barGridOf 钳成 0（谱面当没有弱起），而
        // 命令行那侧仍会报「弱起 N 拍」并把「（手动指定）」打出来——两处口径
        // 相反，用户看到的是一个被静默丢弃的参数。
        TimeSignature s44;
        s44.numerator = 4;
        s44.denomPow = 2;                   // 4/4
        check(pickupArgError(1.0, s44).empty(), "4/4 的 1 拍弱起合法");
        check(pickupArgError(3.5, s44).empty(), "4/4 的 3.5 拍弱起合法");
        check(pickupArgError(3.9, s44).empty(), "4/4 的 3.9 拍弱起合法");
        check(pickupArgError(4.0, s44).find("不小于整小节") != std::string::npos,
              "4/4 里 4 拍等于整小节，必须报错");
        check(pickupArgError(99.0, s44).find("不小于整小节") != std::string::npos,
              "4/4 里 99 拍必须报错");
        check(pickupArgError(3.999999999, s44).find("不小于整小节") != std::string::npos,
              "浮点误差不能放过「其实就是整小节」的值");
        check(pickupArgError(0.0, s44).find("--no-pickup") != std::string::npos,
              "0 拍要引导用户改用 --no-pickup");
        check(pickupArgError(-1.0, s44).find("大于 0") != std::string::npos,
              "负数同样拦下（防止绕过 parseBeats 的调用方）");
        TimeSignature s68;
        s68.numerator = 6;
        s68.denomPow = 3;                   // 6/8：拍 = 八分音符
        check(pickupArgError(5.5, s68).empty(), "6/8 的 5.5 拍弱起合法");
        check(pickupArgError(6.0, s68).find("不小于整小节") != std::string::npos,
              "6/8 的 6 拍等于整小节，必须报错");
        TimeSignature s34;
        s34.numerator = 3;
        s34.denomPow = 2;                   // 3/4
        check(pickupArgError(2.0, s34).empty(), "3/4 的 2 拍弱起合法");
        check(pickupArgError(3.0, s34).find("不小于整小节") != std::string::npos,
              "3/4 的 3 拍等于整小节，必须报错");
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
        // v1.85.0：表尾外推必须封顶。旧版 barIndex 无上限外推，畸形 delta-time
        // 把 tick 推到数亿时会返回上千万的小节号，调用方「补空小节」循环随之爆炸。
        check(g.barIndex(500000000L) <= (long)g.starts.size() + BarGrid::kMaxExtrapBars,
              "巨额 tick 的表尾外推被封顶（不再放大到千万级）");
        check(g.barIndex(3840) == 2, "封顶不影响表内小节号");
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

    // ---- 外部歌词时间字段越界（v1.100.0）：32 位 tick 不能环绕 ----
    {
        MidiData d;
        d.division = 480;
        d.tempoChanges = {{0, 120.0}};
        const TempoTimeline tl(d);
        long tick = -1;

        check(tl.secondsToTickChecked(1.0, tick) && tick == 960,
              "1 秒有界换算成 960 tick");
        check(tl.secondsToTickChecked(-1.0, tick) && tick == -960,
              "负秒有界换算成 -960 tick");
        check(!tl.secondsToTickChecked(1e12, tick),
              "1e12 秒超出 long 值域时返回 false（旧版会绕成负数）");
        check(!tl.secondsToTickChecked(-1e12, tick),
              "负 1e12 秒同样被判越界");
        check(tl.secondsToTick(1e12) > 0 && tl.secondsToTick(-1e12) < 0,
              "secondsToTick 越界时饱和到端点，时间轴前后不颠倒");
        check(tl.secondsToTick(1.0) == 960 && tl.secondsToTick(2.0) == 1920,
              "正常秒数换算零回归");

        size_t dropped = 99;   // 故意给脏值：dropped 是输出参数，入口必须清零
        const auto normal = parseLrcText(
            "[00:00.00]你好\n[00:01.00]世界\n", d, &dropped);
        check(normal.size() == 2 && dropped == 0,
              "正常 LRC 不丢任何行（dropped 入口清零）");
        check(normal[0].startTick == 0 && normal[1].startTick == 960 &&
              normal[0].text == "你好" && normal[1].text == "世界",
              "正常 LRC 的 tick 与文本零回归");

        dropped = 0;
        const auto bigOff = parseLrcText(
            "[offset:999999999999]\n[00:00.00]你好\n[00:01.00]世界\n",
            d, &dropped);
        check(bigOff.empty() && dropped == 2,
              "巨大 [offset:] 的两行都被丢弃且计数为 2");
        dropped = 0;
        const auto bigStamp = parseLrcText(
            "[99999999:00.00]你好\n[99999999:01.00]世界\n", d, &dropped);
        check(bigStamp.empty() && dropped == 2,
              "巨大时间戳的两行都被丢弃且计数为 2");
        dropped = 0;
        const auto negOff = parseLrcText(
            "[offset:-999999999999]\n[00:00.00]你好\n[00:01.00]世界\n",
            d, &dropped);
        check(negOff.empty() && dropped == 2,
              "巨大负 [offset:] 不会被环绕成曲首歌词");
        dropped = 0;
        const auto hugeOff = parseLrcText(
            "[offset:999999999999999999999999]\n[00:00.00]甲\n", d, &dropped);
        check(hugeOff.empty() && dropped == 1,
              "超出 llround 值域的 [offset:] 也不崩、不产词");

        dropped = 0;
        const auto mixed = parseLrcText(
            "[00:00.00]甲乙\n[99999999:00.00]丙丁\n", d, &dropped);
        check(mixed.size() == 1 && dropped == 1,
              "混合时间戳只丢越界那一行");
        check(mixed[0].startTick == 0 && mixed[0].text == "甲乙",
              "保留下来的歌词行 tick 不被越界行带偏");

        dropped = 0;
        const auto oldSig = parseLrcText(
            "[offset:999999999999]\n[00:00.00]甲\n", 480, 120, &dropped);
        check(oldSig.empty() && dropped == 1,
              "旧 (division,bpm) 签名同样走有界换算");
        dropped = 0;
        const auto wasmSig = parseLrcText("[00:00.00]甲\n", d, &dropped);
        check(wasmSig.size() == 1 && dropped == 0,
              "WASM 使用的 MidiData 签名不误伤正常歌词");
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
        {
            // v1.40.0 起同 tick 的全局段落名与通道级渐变分列，避免不同通道的
            // wedge 在合并时互相覆盖；区间过滤本身的行为不变。
            auto inBar = e.inRange(2880, 3360);
            bool hasChorus = false;
            for (const auto& m : inBar) if (m.section == "Chorus") hasChorus = true;
            check(hasChorus && !inBar.empty(),
                  "inRange 只取该小节的标记（段落名与通道渐变分列）");
        }
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

    // ---- 控制器语义：CC64 踏板迟滞（hysteresis）----
    {
        std::cout << "\n测试控制器语义：CC64 踏板迟滞\n";
        // 真实连续踏板包络常在 33..63 之间停留（半踩 / 释放途中）。
        // 旧实现用二值 value>=64 判定：一旦落到 63 以下就当抬起，
        // 导致 127→50→0 这种平滑释放在 50 处提前断开，音只延到 50 那一拍。
        // 迟滞：踩下判 >=64，抬起判 <=32，中间 33..63 保持前态。
        SmfTrack cond, piano;
        cond.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano.at(0, ev({0xB0, 0x40, 127}));   // 踩下
        piano.at(480, ev({0xB0, 0x40, 50}));  // 死区：保持踩下
        piano.at(960, ev({0xB0, 0x40, 0}));   // 真正释放
        piano.at(0, ev({0x90, 60, 96}));
        piano.at(120, ev({0x80, 60, 0x40}));  // 键盘只按 1/4 拍
        auto bytes = buildSmf(2, 480, {cond, piano});
        MidiData md;
        loadMidiFromMemory(bytes.data(), bytes.size(), md);
        check(md.notes.size() == 1, "1 个音符在，实际: " + std::to_string(md.notes.size()));
        check(md.pedalEvents == 3, "记下 3 个踏板事件，实际: " + std::to_string(md.pedalEvents));
        check(md.pedalExtendedNotes == 1, "补长 1 个音，实际: " + std::to_string(md.pedalExtendedNotes));
        check(md.notes[0].durationTicks == 960,
              "死区不中断：音延到真正释放点 960，实际: " + std::to_string(md.notes[0].durationTicks));
        check(md.notes[0].rawDurationTicks == 120, "rawDurationTicks 仍是按键 1/4 拍");

        // 死区下方（value<=32）仍是真正的释放：127→20→0 在 20 处断开
        SmfTrack cond2, piano2;
        cond2.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano2.at(0, ev({0xB0, 0x40, 127}));
        piano2.at(480, ev({0xB0, 0x40, 20}));
        piano2.at(960, ev({0xB0, 0x40, 0}));
        piano2.at(0, ev({0x90, 60, 96}));
        piano2.at(120, ev({0x80, 60, 0x40}));
        auto b2 = buildSmf(2, 480, {cond2, piano2});
        MidiData m2;
        loadMidiFromMemory(b2.data(), b2.size(), m2);
        check(m2.notes[0].durationTicks == 480,
              "value=20 是真正释放（<=32），音延到 480，实际: " + std::to_string(m2.notes[0].durationTicks));

        // 纯 0/127 文件行为不变（零回归）：与旧二值判定完全一致
        SmfTrack cond3, piano3;
        cond3.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
        piano3.at(0, ev({0xB0, 0x40, 127}));
        piano3.at(480, ev({0xB0, 0x40, 0}));
        piano3.at(0, ev({0x90, 60, 96}));
        piano3.at(120, ev({0x80, 60, 0x40}));
        auto b3 = buildSmf(2, 480, {cond3, piano3});
        MidiData m3;
        loadMidiFromMemory(b3.data(), b3.size(), m3);
        check(m3.notes[0].durationTicks == 480,
              "纯 0/127 行为不变，时值仍是 480，实际: " + std::to_string(m3.notes[0].durationTicks));
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

    // ---- running status：系统事件取消、实时消息不改变、省略字节宽容补回 ----
    {
        std::cout << "\n测试 running status：系统事件取消 / 实时消息不改变 / 省略字节补回\n";
        const std::vector<uint8_t> lyric = ev({0xFF, 0x05, 0x01, 'a'});
        const std::vector<uint8_t> sysex = ev({0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7});

        auto load = [](std::vector<SmfTrack> tracks) {
            auto bytes = buildSmf(1, 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            return std::make_pair(ok, std::move(d));
        };
        // C4 八分 -> 中间系统事件 -> D4 八分；后半段刻意省略状态字节
        auto melody = [](std::vector<uint8_t> middle, bool fullStatus, int ch) {
            SmfTrack t;
            t.at(0, ev({0x90 | ch, 60, 100}));
            t.at(240, ev({60, 0}));
            t.at(240, std::move(middle));
            t.at(480, fullStatus ? ev({0x90 | ch, 62, 100}) : ev({62, 100}));
            t.at(720, fullStatus ? ev({0x90 | ch, 62, 0}) : ev({62, 0}));
            return t;
        };
        auto twoNotes = [](const MidiData& d) {
            return d.notes.size() == 2 && d.notes[0].pitch == 60 &&
                   d.notes[1].pitch == 62 && d.notes[0].durationTicks == 240 &&
                   d.notes[1].durationTicks == 240;
        };

        auto a = load({melody(lyric, false, 0)});
        check(a.first, "歌词 meta 之后省略状态字节：文件仍能解析");
        check(twoNotes(a.second), "歌词 meta 之后省略状态字节：C4/D4 两个八分音符都在");
        check(a.second.runningStatusRecovered == 1, "补回处数 = 1（第一个裸数据字节）");
        check(runningStatusLabel(a.second).find("补回 1 处") != std::string::npos,
              "CLI 提示写明补回 1 处");
        check(runningStatusLabel(MidiData{}).empty(), "普通文件不输出省略状态字节提示");

        auto b = load({melody(sysex, false, 0)});
        check(twoNotes(b.second), "SysEx 之后省略状态字节：C4/D4 都在");
        check(b.second.runningStatusRecovered == 1, "SysEx 同样补回 1 处");

        auto c = load({melody(ev({0xF8}), false, 0)});
        check(twoNotes(c.second), "0xF8 时钟不改变 running status：C4/D4 都在");
        check(c.second.runningStatusRecovered == 0, "实时消息不需要补回");

        auto d = load({melody(ev({0xFE}), false, 0)});
        check(twoNotes(d.second), "0xFE active sensing 不改变 running status");

        auto e = load({melody(lyric, true, 0)});
        check(twoNotes(e.second), "规范写法（重写状态字节）不受影响");
        check(e.second.runningStatusRecovered == 0, "规范写法不触发补回");

        SmfTrack multi;
        multi.at(0, ev({0x90, 60, 100}));
        multi.at(240, ev({60, 0}));
        multi.at(240, lyric);
        multi.at(240, sysex);
        multi.at(480, ev({62, 100}));
        multi.at(720, ev({62, 0}));
        auto f = load({multi});
        check(twoNotes(f.second), "meta + SysEx 连续出现：仍读出两个音");

        // System Common 也取消 running status，且不能把它的两字节数据当状态
        auto g = load({melody(ev({0xF2, 0x00, 0x00}), false, 0)});
        check(twoNotes(g.second), "0xF2 Song Position Pointer 之后省略状态字节：两个音都在");

        // 补回的是「最后一条通道状态」，不是硬编码 0x90
        auto h = load({melody(lyric, false, 1)});
        check(twoNotes(h.second), "通道 1 的省略状态字节也能补回");
        check(h.second.notes.size() == 2 && h.second.notes[1].channel == 1,
              "补回的音符仍归原通道，不是通道 0");

        // 最后通道状态是 0xB0（CC，两字节数据）时，裸数据按 CC 解释，
        // 不会多吃/少吃字节导致后面的音符错位
        SmfTrack cc;
        cc.at(0, ev({0x90, 60, 100}));
        cc.at(240, ev({60, 0}));
        cc.at(240, ev({0xB0, 7, 100}));
        cc.at(240, lyric);
        cc.at(480, ev({11, 80}));
        cc.at(480, ev({0x90, 62, 100}));
        cc.at(720, ev({62, 0}));
        auto i = load({cc});
        check(twoNotes(i.second), "最后通道状态是 CC 时，省略状态字节按两字节数据解释");
        // 同轨先写的 CC7（tick 240）也会进力度表，所以这里查「有没有」而不是
        // 断言总数：CC11 是 tick 480、值 80 的那一点。
        bool hasCc11 = false;
        for (const auto& p : i.second.dynamics)
            if (p.tick == 480 && p.value == 80) hasCc11 = true;
        check(hasCc11, "被补回的 CC11 进了力度表");
    }

    // ---- MIDI Port (FF 21) / Channel Prefix (FF 20) ----
    {
        std::cout << "\n测试 MIDI Port：端口隔离 / 不跨轨继承 / FF 20 不重定向通道\n";
        auto load = [](std::vector<SmfTrack> tracks) {
            const int n = (int)tracks.size();
            auto bytes = buildSmfFmt(1, n, 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            return std::make_pair(ok, std::move(d));
        };
        auto portMeta = [](int p) { return ev({0xFF, 0x21, 0x01, p}); };
        auto prefixMeta = [](int ch) { return ev({0xFF, 0x20, 0x01, ch}); };
        auto rpn = [](SmfTrack& t, long tick, int ch, int semis) {
            t.at(tick, ev({0xB0 | ch, 101, 0}));
            t.at(tick, ev({0xB0 | ch, 100, 0}));
            t.at(tick, ev({0xB0 | ch, 6, semis}));
        };

        // 1) 踏板：port 0 踩、port 1 不踩、第三条轨不写端口（应重置回 0）
        {
            SmfTrack p0, p1, p2;
            p0.at(0, portMeta(0));
            p0.at(0, ev({0xB0, 64, 127}));
            p0.at(0, ev({0x90, 60, 96}));
            p0.at(480, ev({0x80, 60, 0}));
            p0.at(1920, ev({0xB0, 64, 0}));
            p1.at(0, portMeta(1));
            p1.at(480, ev({0x90, 62, 96}));
            p1.at(960, ev({0x80, 62, 0}));
            p2.at(960, ev({0x90, 64, 96}));
            p2.at(1440, ev({0x80, 64, 0}));
            auto r = load({p0, p1, p2});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 3, "端口踏板素材解析出 3 个音");
            check(d.midiPortEvents == 2 && d.midiPortCount == 2 && d.midiPortMax == 1,
                  "FF 21 计数：2 处声明、2 个端口、最大端口 1");
            check(d.notes.size() == 3 && d.notes[0].channel == 0 &&
                  d.notes[1].channel == 16 && d.notes[2].channel == 0,
                  "逻辑通道分别是 0 / 16 / 0（第三条轨端口重置）");
            check(d.pedalExtendedNotes == 2,
                  "只有 port 0 的两个音被踏板补长，port 1 的音没被串");
            check(d.notes.size() == 3 && d.notes[1].durationTicks == 480,
                  "port 1 的 D4 仍是 1 拍（480 tick）");
            check(d.notes.size() == 3 && d.notes[0].durationTicks == 1920 &&
                  d.notes[2].durationTicks == 960,
                  "port 0 的 C4 / E4 分别补长到 1920 / 960 tick");
            check(portLabel(d).find("MIDI Port") != std::string::npos &&
                  portLabel(d).find("已隔离") != std::string::npos,
                  "portLabel 说明端口数与隔离结果");
        }

        // 2) 弯音范围：两个端口复用 ch0，一个 ±12、一个用默认 ±2
        {
            SmfTrack p0, p1;
            p0.at(0, portMeta(0));
            rpn(p0, 0, 0, 12);
            p0.at(0, ev({0xE0, 0x7F, 0x7F}));
            p0.at(0, ev({0x90, 60, 96}));
            p0.at(480, ev({0x80, 60, 0}));
            p1.at(0, portMeta(1));
            p1.at(480, ev({0xE0, 0x7F, 0x7F}));
            p1.at(480, ev({0x90, 62, 96}));
            p1.at(960, ev({0x80, 62, 0}));
            auto r = load({p0, p1});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 2, "端口弯音素材解析出 2 个音");
            check(d.bendRanges.size() == 1 && d.bendRanges[0].channel == 0 &&
                  d.bendRangeChannels == 1,
                  "RPN 只落在 port 0 的通道，不覆盖 port 1");
            check(d.pitchBends.size() == 2 &&
                  std::abs(d.pitchBends[0].range - 12.0) < 1e-9 &&
                  std::abs(d.pitchBends[1].range - 2.0) < 1e-9,
                  "两个端口的弯音轮各自解算成 ±12 / ±2 半音");
        }

        // 3) RPN 状态按端口隔离：两个端口都设范围，不能互相覆盖
        {
            SmfTrack p0, p1;
            p0.at(0, portMeta(0));
            rpn(p0, 0, 0, 12);
            p1.at(0, portMeta(1));
            rpn(p1, 0, 0, 5);
            auto r = load({p0, p1});
            const MidiData& d = r.second;
            check(d.bendRanges.size() == 2 && d.bendRangeChannels == 2,
                  "两个端口各设一次 RPN，状态表互不覆盖");
            bool chanOk = d.bendRanges.size() == 2;
            if (chanOk)
                chanOk = d.bendRanges[0].channel != d.bendRanges[1].channel;
            check(chanOk, "两次 RPN 的逻辑通道分别是 0 与 16");
        }

        // 4) FF 20 不重定向通道消息；同轨中途改端口立即生效
        {
            SmfTrack t;
            t.at(0, prefixMeta(2));
            t.at(0, ev({0x90, 60, 96}));
            t.at(480, ev({0x80, 60, 0}));
            t.at(480, portMeta(3));
            t.at(480, ev({0x90, 62, 96}));
            t.at(960, ev({0x80, 62, 0}));
            auto r = load({t});
            const MidiData& d = r.second;
            check(d.channelPrefixEvents == 1 && d.midiPortEvents == 1 &&
                  d.midiPortMax == 3,
                  "FF 20 / FF 21 计数与最大端口号");
            check(d.notes.size() == 2 && d.notes[0].channel == 0 &&
                  d.notes[1].channel == 48,
                  "FF 20 没把 ch0 重定向成 ch2；FF 21 后逻辑通道是 3*16=48");
            check(portLabel(d).find("通道前缀") != std::string::npos &&
                  portLabel(d).find("不影响通道消息") != std::string::npos,
                  "portLabel 如实说明 FF 20 不改变通道消息");
        }

        // 5) 最大端口 / 鼓轨 / 音色 / 力度都跟着逻辑通道走
        {
            SmfTrack t;
            t.at(0, portMeta(255));
            t.at(0, ev({0xC0, 20}));
            t.at(0, ev({0xB0, 11, 80}));
            t.at(0, ev({0x99, 36, 100}));
            t.at(0, ev({0x90, 60, 96}));
            t.at(480, ev({0x80, 60, 0}));
            auto r = load({t});
            const MidiData& d = r.second;
            check(d.midiPortMax == 255 && d.notes.size() == 1 &&
                  d.notes[0].channel == 4080,
                  "端口 255 的逻辑通道 4080 不越界");
            check(d.drumNotesSkipped == 1,
                  "port 255 的 ch9 仍判为鼓轨（鼓按低 4 位通道号）");
            check(d.channelProgram.count(4080) == 1 &&
                  d.channelProgram.at(4080) == 20,
                  "GM 音色按 (port, channel) 记录，不覆盖默认端口");
            check(!d.dynamics.empty() && d.dynamics[0].channel == 4080,
                  "CC11 力度点归到 port 255 的通道");
        }

        // 6) 零回归：没有 FF 21 / FF 20 的文件一切照旧
        {
            SmfTrack t;
            t.at(0, ev({0x90, 60, 96}));
            t.at(480, ev({0x80, 60, 0}));
            auto r = load({t});
            const MidiData& d = r.second;
            check(d.midiPortEvents == 0 && d.channelPrefixEvents == 0 &&
                  d.midiPortCount == 1 && d.midiPortMax == 0,
                  "无端口声明时计数全是默认值");
            check(d.notes.size() == 1 && d.notes[0].channel == 0,
                  "无端口时逻辑通道与旧值完全一致");
            check(portLabel(d).empty(), "无 FF 21 / FF 20 时 portLabel 为空串");
        }
    }

    // ---- SysEx 鼓模式（GS/XG Part Mode / GM-GS-XG Reset）----
    {
        std::cout << "\n测试 SysEx 鼓模式：任意 part 设为鼓 / Reset 恢复默认 / 端口隔离\n";
        auto load = [](std::vector<SmfTrack> tracks) {
            const int n = (int)tracks.size();
            auto bytes = buildSmfFmt(1, n, 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            return std::make_pair(ok, std::move(d));
        };
        // SMF 的 SysEx 事件：F0 <长度> <完整包 F0 ... F7>
        auto sx = [](std::vector<int> payload) {
            std::vector<uint8_t> v = {0xF0, (uint8_t)payload.size()};
            for (int b : payload) v.push_back((uint8_t)b);
            return v;
        };
        auto gsMode = [&](int ch, int mode) {
            return sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10 + ch, 0x15,
                       mode, 0x00, 0xF7});
        };
        auto xgMode = [&](int ch, int mode) {
            return sx({0xF0, 0x43, 0x10, 0x4C, 0x08, ch, 0x07, mode, 0xF7});
        };
        auto gmReset = [&]() {
            return sx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
        };
        auto gsReset = [&]() {
            return sx({0xF0, 0x41, 0x10, 0x42, 0x12,
                       0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
        };
        auto xgReset = [&]() {
            return sx({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7});
        };
        // 鼓件 36 / 38 与旋律 C4 / D4 分列两条轨，tick 完全相同。
        auto drumTrack = [](int ch) {
            SmfTrack t;
            t.at(0,   ev({0x90 | ch, 36, 100}));
            t.at(240, ev({0x80 | ch, 36, 0}));
            t.at(480, ev({0x90 | ch, 38, 100}));
            t.at(720, ev({0x80 | ch, 38, 0}));
            return t;
        };
        auto melodyTrack = [](int ch) {
            SmfTrack t;
            t.at(0,   ev({0x90 | ch, 60, 90}));
            t.at(480, ev({0x80 | ch, 60, 0}));
            t.at(480, ev({0x90 | ch, 62, 90}));
            t.at(960, ev({0x80 | ch, 62, 0}));
            return t;
        };
        auto setup = [&](std::vector<uint8_t> mode, std::vector<uint8_t> reset,
                         long resetTick, bool lastTrack) {
            SmfTrack s;
            s.at(0, std::move(mode));
            if (!reset.empty()) s.at(resetTick, std::move(reset));
            SmfTrack d = drumTrack(0), m = melodyTrack(1);
            if (lastTrack) return load({std::move(d), std::move(m), std::move(s)});
            return load({std::move(s), std::move(d), std::move(m)});
        };

        // 1) GS / XG 把 ch0 设成鼓：两个鼓件音丢掉，两个旋律音留下
        for (int kind = 0; kind < 2; ++kind) {
            auto r = setup(kind == 0 ? gsMode(0, 1) : xgMode(0, 1), {}, 0, false);
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 2 &&
                  d.notes[0].pitch == 60 && d.notes[1].pitch == 62,
                  kind == 0 ? "GS Part Mode：只留下 C4 / D4 两个旋律音"
                            : "XG Part Mode：只留下 C4 / D4 两个旋律音");
            check(d.sysExEvents == 1 && d.sysExModeEvents == 1 &&
                  d.sysExDrumChannels == 1 && d.sysExDrumNotesSkipped == 2 &&
                  d.drumNotesSkipped == 2,
                  "SysEx 统计：1 个包 / 1 处设定 / 1 条鼓通道 / 跳过 2 个鼓件音");
            check(sysExLabel(d).find("已跳过 2 个鼓件音") != std::string::npos,
                  "sysExLabel 报出跳过的鼓件音数量");
        }

        // 2) mode=0 是普通乐器：对照，一个音都不能丢
        {
            auto r = setup(gsMode(0, 0), {}, 0, false);
            const MidiData& d = r.second;
            check(d.notes.size() == 4 && d.sysExDrumNotesSkipped == 0 &&
                  d.drumNotesSkipped == 0,
                  "GS mode=0：普通乐器不丢音，也不算鼓");
            check(d.sysExNormalChannels == 1 &&
                  sysExLabel(d).find("恢复普通乐器") != std::string::npos,
                  "GS mode=0：如实记为「恢复普通乐器」");
        }

        // 3) 三种 Reset 在 tick 480 撤销设定：前面的鼓丢，后面的鼓留
        const char* resetNames[3] = {"GM Reset", "GS Reset", "XG Reset"};
        for (int kind = 0; kind < 3; ++kind) {
            auto rst = kind == 0 ? gmReset() : (kind == 1 ? gsReset() : xgReset());
            auto r = setup(gsMode(0, 1), std::move(rst), 480, false);
            const MidiData& d = r.second;
            check(d.notes.size() == 3 && d.sysExDrumNotesSkipped == 1 &&
                  d.sysExResetEvents == 1 && d.drumNotesSkipped == 1,
                  std::string(resetNames[kind]) + "：只撤销其后的鼓设定");
            check(sysExLabel(d).find("Reset 恢复默认") != std::string::npos,
                  std::string(resetNames[kind]) + "：sysExLabel 报出 Reset");
        }

        // 4) SysEx 写在最后一条轨：解析顺序不能决定时间顺序
        {
            auto r = setup(gsMode(0, 1), {}, 0, true);
            check(r.second.notes.size() == 2 &&
                  r.second.sysExDrumNotesSkipped == 2,
                  "SysEx 在最后一条轨（tick 0）仍对前面的轨生效");
        }

        // 5) 通道 10 改回普通乐器 / 显式设回鼓
        {
            SmfTrack s, t9, m = melodyTrack(1);
            s.at(0, gsMode(9, 0));
            t9.at(0,   ev({0x99, 60, 90}));
            t9.at(480, ev({0x89, 60, 0}));
            auto r = load({std::move(s), std::move(t9), std::move(m)});
            const MidiData& d = r.second;
            check(d.notes.size() == 3 && d.drumNotesSkipped == 0,
                  "GS 把通道 10 改回钢琴：ch9 的旋律音全部留下");
            check(d.sysExNormalChannels == 1,
                  "通道 10 的 mode=0 记为恢复普通乐器");
        }
        {
            SmfTrack s, t9;
            s.at(0, gsMode(9, 1));
            t9.at(0,   ev({0x99, 60, 90}));
            t9.at(480, ev({0x89, 60, 0}));
            auto r = load({std::move(s), std::move(t9)});
            check(r.second.notes.empty() && r.second.drumNotesSkipped == 1,
                  "显式把通道 10 设为鼓：与默认判定一致");
        }

        // 6) 无关 SysEx + 默认鼓轨零回归
        {
            SmfTrack s, t9, m = melodyTrack(1);
            s.at(0, sx({0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7}));
            t9.at(0,   ev({0x99, 36, 100}));
            t9.at(240, ev({0x89, 36, 0}));
            auto r = load({std::move(s), std::move(t9), std::move(m)});
            const MidiData& d = r.second;
            check(d.sysExEvents == 1 && d.sysExModeEvents == 0 &&
                  d.notes.size() == 2 && d.drumNotesSkipped == 1,
                  "不相干的 SysEx 零回归：仍只按通道 9 判鼓");
            check(sysExLabel(d).empty(),
                  "没有 part mode 设定时 sysExLabel 为空串");
        }

        // 7) 同 tick 多次设定按文件顺序（seq）定序
        {
            SmfTrack s;
            s.at(0, gsMode(0, 1));
            s.at(0, gsMode(0, 0));
            auto r = load({std::move(s), drumTrack(0), melodyTrack(1)});
            check(r.second.notes.size() == 4,
                  "同 tick 先设鼓后设普通：后写覆盖，4 个音都留");
            SmfTrack s2;
            s2.at(0, gsMode(0, 0));
            s2.at(0, gsMode(0, 1));
            auto r2 = load({std::move(s2), drumTrack(0), melodyTrack(1)});
            check(r2.second.notes.size() == 2,
                  "同 tick 先设普通后设鼓：后写覆盖，2 个鼓件音丢掉");
        }

        // 8) 端口隔离：port 1 ch0 设鼓，port 0 ch0 仍是旋律
        {
            SmfTrack p0, p1;
            p0.at(0,   ev({0x90, 60, 90}));
            p0.at(480, ev({0x80, 60, 0}));
            p1.at(0,   ev({0xFF, 0x21, 0x01, 0x01}));   // FF 21 port 1
            p1.at(0,   gsMode(0, 1));
            p1.at(0,   ev({0x90, 36, 100}));
            p1.at(240, ev({0x80, 36, 0}));
            auto r = load({std::move(p0), std::move(p1)});
            const MidiData& d = r.second;
            check(d.notes.size() == 1 && d.notes[0].channel == 0 &&
                  d.notes[0].pitch == 60,
                  "鼓模式按 (port, channel) 隔离：只丢 port 1 的鼓件音");
            check(d.sysExDrumNotesSkipped == 1 && d.drumNotesSkipped == 1,
                  "端口鼓通道的跳过数单独计入总数");
        }

        // 9) format 2 的首轨只有鼓：段判定不能被新收进来的鼓音带偏
        {
            SmfTrack drums, a, b;
            drums.at(0,   ev({0x99, 36, 100}));
            drums.at(240, ev({0x89, 36, 0}));
            a.at(0,   ev({0x90, 60, 90}));
            a.at(480, ev({0x80, 60, 0}));
            b.at(0,   ev({0x91, 67, 90}));
            b.at(480, ev({0x81, 67, 0}));
            auto bytes = buildSmfFmt(2, 3, 480, {drums, a, b});
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            check(ok && d.format2UsedTrack == 1 && d.notes.size() == 1 &&
                  d.notes[0].pitch == 60,
                  "format 2 首轨只有鼓：仍挑中真正含旋律的第一条轨");
            check(d.drumNotesSkipped == 1 && d.format2SequenceCount == 2,
                  "format 2 的鼓音仍计数，独立序列数不含纯鼓轨");
        }
    }


    // ---- Bank Select / Program Change 时间轴（XG 鼓组 / 中途换音色）----
    {
        std::cout << "\n测试 Bank Select 与音色时间轴：XG 鼓组 / 中途换音色\n";
        auto load = [](std::vector<SmfTrack> tracks) {
            const int n = (int)tracks.size();
            auto bytes = buildSmfFmt(1, n, 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            return std::make_pair(ok, std::move(d));
        };
        // CC0 (Bank MSB) + CC32 (Bank LSB) + 0xC0 是三个独立事件，
        // 同一 tick 靠 SmfTrack 的 stable_sort 保持写入顺序。
        auto bank = [](SmfTrack& t, long tick, int ch, int msb, int prog) {
            t.at(tick, ev({0xB0 | ch, 0x00, msb}));
            t.at(tick, ev({0xB0 | ch, 0x20, 0x00}));
            t.at(tick, ev({0xC0 | ch, prog}));
        };
        auto tone = [](SmfTrack& t, long tick, int ch, int pitch) {
            t.at(tick,       ev({0x90 | ch, pitch, 90}));
            t.at(tick + 120, ev({0x80 | ch, pitch, 0}));
        };
        // F0 <长度 6> <GM System On：F0 7E 7F 09 01 F7>
        auto gmReset = []() {
            return ev({0xF0, 0x06, 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
        };

        // 1) CC0=127 把 ch0 设为 XG 鼓组：鼓件音被删，ch1 旋律保留
        {
            SmfTrack setup, drums, mel;
            bank(setup, 0, 0, 127, 0);
            tone(drums, 0, 0, 36);
            tone(drums, 240, 0, 38);
            tone(mel, 0, 1, 60);
            auto r = load({std::move(setup), std::move(drums), std::move(mel)});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 1 && d.notes[0].pitch == 60,
                  "XG Bank MSB 127 指定的鼓通道不再输出鼓件音");
            check(d.bankSelectEvents == 2 && d.programChangeEvents == 1 &&
                  d.bankModeEvents == 1 && d.bankDrumChannels == 1,
                  "Bank Select / Program Change / 鼓组切换统计正确");
            check(d.sysExModeEvents == 0 && d.sysExEvents == 0 &&
                  sysExLabel(d).empty() && !bankLabel(d).empty(),
                  "Bank 鼓组不污染 SysEx 统计，由 bankLabel 单独说明");
            check(d.drumNotesSkipped == 2, "被丢的鼓件音计入总数");
        }

        // 2) CC0=0 是普通音色库：对照基线一个音都不能少
        {
            SmfTrack setup, ch0, mel;
            bank(setup, 0, 0, 0, 0);
            tone(ch0, 0, 0, 60);
            tone(ch0, 240, 0, 62);
            tone(mel, 0, 1, 64);
            auto r = load({std::move(setup), std::move(ch0), std::move(mel)});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 3 && d.bankModeEvents == 0 &&
                  d.drumNotesSkipped == 0,
                  "普通 GM 音色库不触发鼓组判定");
            check(d.programChangeEvents == 1 &&
                  bankLabel(d).find("Bank Select") != std::string::npos,
                  "普通音色库仍如实记录 Bank Select");
        }

        // 3) tick 960 把 CC0 改回 0：前半鼓、后半旋律
        {
            SmfTrack setup, part;
            bank(setup, 0, 0, 127, 0);
            bank(setup, 960, 0, 0, 10);
            tone(part, 0, 0, 36);        // 鼓组区间 -> 丢
            tone(part, 960, 0, 60);      // 恢复普通乐器 -> 留
            auto r = load({std::move(setup), std::move(part)});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 1 && d.notes[0].pitch == 60,
                  "Bank 时间轴：改回普通库之后的音不再被当鼓");
            check(d.programTimeline.size() == 2 && d.programSwitchChannels == 1,
                  "两次 0xC0 记成两个音色点，同通道中途换音色被识别");
            check(programAt(d, 0, 0) == 0 && programAt(d, 0, 960) == 10 &&
                  d.channelProgram.at(0) == 10,
                  "programAt 按 tick 取音色，channelProgram 仍保留最后一个");
        }

        // 4) Bank 设定写在最后一条轨（tick 0）仍生效
        {
            SmfTrack drums, mel, setup;
            tone(drums, 0, 0, 36);
            tone(mel, 0, 1, 60);
            bank(setup, 0, 0, 127, 0);
            auto r = load({std::move(drums), std::move(mel), std::move(setup)});
            check(r.first && r.second.notes.size() == 1,
                  "Bank 设定写在文件末尾的 setup 轨仍对前面的轨生效");
        }

        // 5) GM Reset 撤销鼓组
        {
            SmfTrack setup, part;
            bank(setup, 0, 0, 127, 0);
            setup.at(480, gmReset());
            tone(part, 0, 0, 36);        // Reset 之前 -> 丢
            tone(part, 480, 0, 60);      // Reset 之后 -> 留
            auto r = load({std::move(setup), std::move(part)});
            check(r.first && r.second.notes.size() == 1 &&
                  r.second.notes[0].pitch == 60,
                  "GM Reset 把 Bank 状态清回默认，之后的音恢复旋律");
        }

        // 6) 端口隔离：port 1 的 ch0 设鼓，port 0 的 ch0 仍是旋律
        {
            SmfTrack p0, p1, setup;
            p0.at(0, ev({0xFF, 0x21, 0x01, 0x00}));
            tone(p0, 0, 0, 60);
            p1.at(0, ev({0xFF, 0x21, 0x01, 0x01}));
            tone(p1, 0, 0, 36);
            setup.at(0, ev({0xFF, 0x21, 0x01, 0x01}));
            bank(setup, 0, 0, 127, 0);
            auto r = load({std::move(p0), std::move(p1), std::move(setup)});
            check(r.first && r.second.notes.size() == 1 &&
                  r.second.notes[0].channel == 0,
                  "Bank 鼓组按 (port, channel) 隔离，port 0 的旋律保留");
        }

        // 7) 只有 CC0=127、没有随后的 0xC0：Bank Select 只是预备状态
        {
            SmfTrack setup, part;
            setup.at(0, ev({0xB0, 0x00, 127}));
            tone(part, 0, 0, 60);
            auto r = load({std::move(setup), std::move(part)});
            check(r.first && r.second.notes.size() == 1 &&
                  r.second.bankModeEvents == 0,
                  "缺少 Program Change 时 CC0 不切鼓（Bank 只是预备状态）");
        }

        // 8) 通道 10 发普通 Bank Select + 0xC0，仍是默认鼓通道
        {
            SmfTrack setup, drums, mel;
            bank(setup, 0, 9, 0, 0);
            tone(drums, 0, 9, 36);
            tone(mel, 0, 1, 60);
            auto r = load({std::move(setup), std::move(drums), std::move(mel)});
            check(r.first && r.second.notes.size() == 1 &&
                  r.second.notes[0].pitch == 60 &&
                  r.second.drumNotesSkipped == 1,
                  "通道 10 的普通音色设定不能把它从鼓改回旋律");
        }

        // 9) programAt 回退 channelProgram；普通 GM 文件不显示 Bank 提示
        {
            MidiData d;
            d.channelProgram[3] = 10;
            check(programAt(d, 3, 0) == 10 && programAt(d, 4, 0) == -1,
                  "没有时间轴记录时 programAt 回退 channelProgram");
            check(bankLabel(d).empty() && sysExLabel(d).empty(),
                  "普通 GM 文件不显示 Bank / SysEx 提示");
            SmfTrack t;
            t.at(0, ev({0xC0, 0}));
            auto r = load({std::move(t)});
            check(r.first && bankLabel(r.second).empty(),
                  "只有 0xC0、没有 CC0/CC32 时 bankLabel 仍为空");
        }
    }

    // ---- v1.37.0：--list-tracks 诊断与谱面标注一致性 ----
    // 旧实现里 listParts() 走 programAt(首音 tick) 时间轴，printTrackList()
    // 却读 channelProgram（只留最后一个 0xC0）；GS/XG 切出来的鼓组也没按轨
    // 记进 drumNotesByTrack。两者都会让「选轨前看的诊断」和真实谱面对不上。
    {
        std::cout << "\n测试诊断一致性：partPrograms 时间轴 / 鼓件音按轨统计\n";
        auto load = [](std::vector<SmfTrack> tracks) {
            const int n = (int)tracks.size();
            auto bytes = buildSmfFmt(1, n, 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
            return std::make_pair(ok, std::move(d));
        };
        auto tone = [](SmfTrack& t, long tick, int ch, int pitch) {
            t.at(tick,       ev({0x90 | ch, pitch, 90}));
            t.at(tick + 120, ev({0x80 | ch, pitch, 0}));
        };

        // 1) 中途换音色：partPrograms 取首音乐器，channelProgram 留末值。
        //    诊断与谱面都必须报「钢琴」，不能一个说小提琴一个说钢琴。
        {
            SmfTrack cond, lead, mel;
            cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            lead.at(0, ev({0xFF, 0x03, 0x04, 'L', 'e', 'a', 'd'}));
            lead.at(0, ev({0xC1, 0}));                 // 钢琴
            tone(lead, 0, 1, 60); tone(lead, 240, 1, 62);
            lead.at(960, ev({0xC1, 40}));               // 小提琴
            tone(lead, 960, 1, 64); tone(lead, 1200, 1, 65);
            mel.at(0, ev({0xFF, 0x03, 0x06, 'M', 'e', 'l', 'o', 'd', 'y'}));
            tone(mel, 0, 2, 67);
            auto r = load({std::move(cond), std::move(lead), std::move(mel)});
            const MidiData& d = r.second;
            const auto progs = partPrograms(d);
            check(r.first && progs.at(1) == 0,
                  "partPrograms 按首音取钢琴(0)，不是末值小提琴(40)");
            check(d.channelProgram.at(1) == 40,
                  "channelProgram 仍保留最后一个音色号（旧字段语义不变）");
            const auto parts = listParts(d);
            check(parts.size() == 2 && parts[0].first == "Lead" &&
                  parts[0].second == "钢琴",
                  "谱面 Lead 标钢琴，与 partPrograms 同源");
            check(gmInstrumentName(progs.at(1)) == parts[0].second,
                  "诊断与谱面共用同一张音色表，不再分叉");
            check(progs.at(2) == -1 && parts[1].second.empty(),
                  "没有 0xC0 的轨音色号为 -1，不臆造乐器名");
        }

        // 2) XG Part Mode 把 ch0 设为鼓组：被删的鼓件音要按轨补记，
        //    否则 --list-tracks 会把整条鼓轨显示成「旋律 0 音」。
        {
            auto sx = [](std::vector<int> payload) {
                std::vector<uint8_t> v = {0xF0, (uint8_t)payload.size()};
                for (int b : payload) v.push_back((uint8_t)b);
                return v;
            };
            SmfTrack cond, drums, mel;
            cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            cond.at(0, sx({0xF0, 0x43, 0x10, 0x4C, 0x08, 0, 0x07, 1, 0xF7}));
            tone(drums, 0, 0, 36); tone(drums, 240, 0, 38);
            tone(mel, 0, 1, 60);
            auto r = load({std::move(cond), std::move(drums), std::move(mel)});
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 1 && d.notes[0].pitch == 60,
                  "XG 鼓组的鼓件音被删、旋律保留");
            check(d.drumNotesByTrack.at(1) == 2,
                  "XG 鼓组的 2 个鼓件音按轨补记进 drumNotesByTrack");
            check(d.drumNotesByTrack.find(2) == d.drumNotesByTrack.end(),
                  "旋律轨不被误记鼓件音");
            const auto xgProgs = partPrograms(d);
            check(xgProgs.find(1) == xgProgs.end(),
                  "整条鼓轨不参与声部乐器标注");
        }

        // 3) Bank MSB 127 切出的鼓组同样按轨补记（与 SysEx 共用时间轴）。
        {
            SmfTrack setup, drums, mel;
            setup.at(0, ev({0xB0, 0x00, 127}));
            setup.at(0, ev({0xB0, 0x20, 0x00}));
            setup.at(0, ev({0xC0, 0}));
            tone(drums, 0, 0, 36); tone(drums, 240, 0, 38);
            tone(mel, 0, 1, 60);
            auto r = load({std::move(setup), std::move(drums), std::move(mel)});
            const MidiData& d = r.second;
            check(r.first && d.drumNotesByTrack.at(1) == 2,
                  "XG Bank 鼓组的鼓件音也按轨补记");
            check(d.drumNotesSkipped == 2,
                  "drumNotesSkipped 页眉计数不受补记影响");
        }

        // 4) 默认鼓通道 9 的旧语义零回归：解析阶段照旧按所有轨统计，
        //    鼓过滤处不重复累加。
        {
            SmfTrack cond, drums, mel;
            cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            tone(drums, 0, 9, 36); tone(drums, 240, 9, 38);
            tone(mel, 0, 1, 60);
            auto r = load({std::move(cond), std::move(drums), std::move(mel)});
            const MidiData& d = r.second;
            check(r.first && d.drumNotesByTrack.at(1) == 2 &&
                  d.drumNotesSkipped == 2,
                  "默认鼓通道 9 仍按轨统计 2 个鼓件音且只计一次");
            const auto gmProgs = partPrograms(d);
            check(gmProgs.find(1) == gmProgs.end(),
                  "默认鼓轨不出现在 partPrograms 里");
        }
    }

    // ---- v1.34.0：--track / 网页段落选择的按轨隔离 ----
    // 旧实现只在 main.cpp 末尾按 track 丢音符，歌词 / 力度 / 踏板 / 弯音
    // 事件表仍来自全部轨，而且踏板等语义修正在解析阶段就改过时值了。
    // 这里直接调 loadMidiFromMemory 的 trackFilter 参数，锁住解析层行为。
    {
        auto loadTrack = [](std::vector<SmfTrack> tracks, int track) {
            auto bytes = buildSmf((int)tracks.size(), 480, std::move(tracks));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d,
                                              track >= 0, track);
            return std::make_pair(ok, std::move(d));
        };
        // 两条轨**故意用同一个通道 0**：按 liveChannels 兜底是挡不住串味的，
        // 只有解析阶段真的按轨门控才能把这些事件全隔开。
        auto makeLeak = []() {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x03, 0x05, 'V','o','c','a','l'}));
            v[0].at(0, ev({0x90, 60, 100}));
            v[0].at(240, ev({0x80, 60, 0}));
            v[0].at(480, ev({0x90, 62, 100}));
            v[0].at(720, ev({0x80, 62, 0}));
            v[1].at(0, ev({0xFF, 0x03, 0x06, 'G','u','i','t','a','r'}));
            v[1].at(0, ev({0x90, 64, 100}));
            v[1].at(240, ev({0x80, 64, 0}));
            v[1].at(480, ev({0x90, 65, 100}));
            v[1].at(720, ev({0x80, 65, 0}));
            v[1].at(0, ev({0xFF, 0x05, 0x01, 'A'}));
            v[1].at(0, ev({0xB0, 0x0B, 20}));       // CC11
            v[1].at(0, ev({0xB0, 0x07, 20}));       // CC7
            v[1].at(0, ev({0xB0, 0x40, 127}));      // 踏板踩下
            v[1].at(2000, ev({0xB0, 0x40, 0}));     // 踏板抬起
            v[1].at(0, ev({0xE0, 0x7F, 0x7F}));     // 弯音推到顶
            return v;
        };

        // 1) 不指定轨：行为与旧版一致，四音个都在，提示为空
        auto all = loadTrack(makeLeak(), -1);
        check(all.first && all.second.notes.size() == 4 &&
              all.second.selectedTrack == -1 && trackLabel(all.second).empty(),
              "不指定轨时四音个全在、selectedTrack 为 -1、trackLabel 为空");

        // 2) 选轨 0：歌词 / 力度 / 踏板 / 弯音一个都不许串
        auto t0 = loadTrack(makeLeak(), 0);
        check(t0.first && t0.second.notes.size() == 2 &&
              t0.second.notes[0].track == 0 && t0.second.notes[1].track == 0,
              "--track 0 只剩该轨的 2 个音符");
        check(t0.second.midiLyrics.empty() && t0.second.dynamics.empty() &&
              t0.second.pitchBends.empty() && t0.second.pedalEvents == 0,
              "--track 0 隔离掉别轨的歌词 / CC11 / CC7 / 踏板 / 弯音");
        check(t0.second.selectedTrack == 0 && t0.second.trackFilteredNotes == 2 &&
              trackLabel(t0.second).find("轨 0") != std::string::npos,
              "--track 0 记下选中轨与丢弃音符数，trackLabel 如实说明");

        // 3) 选轨 1：该轨自己的歌词 / 力度 / 弯音必须完整保留
        auto t1 = loadTrack(makeLeak(), 1);
        check(t1.first && t1.second.notes.size() == 2 &&
              t1.second.midiLyrics.size() == 1 && !t1.second.dynamics.empty() &&
              t1.second.pitchBends.size() == 1,
              "--track 1 保留该轨自己的歌词 / 力度 / 弯音事件");

        // 4) 踏板在别轨且同通道：选中的轨不该被补长时值
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0x90, 60, 100}));
            v[0].at(120, ev({0x80, 60, 0}));
            v[1].at(0, ev({0xB0, 0x40, 127}));
            v[1].at(1920, ev({0xB0, 0x40, 0}));
            auto full = loadTrack(v, -1);
            auto only = loadTrack(v, 0);
            check(full.second.pedalExtendedNotes > 0 &&
                  only.second.pedalExtendedNotes == 0 && only.second.pedalEvents == 0,
                  "别轨的 CC64 踏板不再给选中轨补长时值");
        }

        // 5) 全局设定写在 conductor 轨：选别的轨也要照常生效
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));   // 120 BPM
            v[0].at(0, ev({0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08})); // 3/4
            v[0].at(0, ev({0xFF, 0x59, 0x02, 0x02, 0x00}));          // D 大调
            v[0].at(0, ev({0xFF, 0x06, 0x05, 'V','e','r','s','e'}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            auto r = loadTrack(v, 1);
            check(r.first && r.second.tempoChanges.size() == 1 &&
                  r.second.timeSig.numerator == 3 &&
                  r.second.keySig.sharpsFlats == 2 && r.second.markers.size() == 1,
                  "--track 1 仍读到别轨 conductor 的速度 / 拍号 / 调号 / Marker");
        }

        // 6) RPN 写在 setup 轨：选中的轨仍按 ±12 半音解算弯音
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xB0, 0x65, 0}));
            v[0].at(0, ev({0xB0, 0x64, 0}));
            v[0].at(0, ev({0xB0, 0x06, 12}));
            v[1].at(0, ev({0xE0, 0x7F, 0x7F}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            auto r = loadTrack(v, 1);
            check(r.first && r.second.bendRangeEvents == 1 &&
                  r.second.pitchBends.size() == 1 &&
                  std::fabs(r.second.pitchBends[0].range - 12.0) < 1e-6,
                  "--track 1 仍读到别轨 setup 的 RPN ±12 半音");
        }

        // 7) format 2：段数按整份文件报，实际转换轨跟着 --track 改口
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x03, 0x05, 'S','o','n','g','A'}));
            v[0].at(0, ev({0x90, 60, 100}));
            v[0].at(480, ev({0x80, 60, 0}));
            v[1].at(0, ev({0xFF, 0x03, 0x05, 'S','o','n','g','B'}));
            v[1].at(0, ev({0x90, 67, 100}));
            v[1].at(480, ev({0x80, 67, 0}));
            auto bytes = buildSmfFmt(2, 2, 480, std::move(v));
            MidiData d;
            const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d, true, 1);
            check(ok && d.format2SequenceCount == 2 && d.format2UsedTrack == 1 &&
                  d.notes.size() == 1 && d.notes[0].track == 1,
                  "format 2 选第 2 段：段数仍是 2、只转换轨 1");
        }
    }
    // -------------------------------------------------------------------
    // 截断 / 损坏 MIDI：MTrk 声明长度 > 文件实际字节，或轨尾只剩半个事件。
    // 旧解析器会越过 d+size 读堆里相邻内存，把垃圾字节当成新事件继续解析，
    // 一份 2 音素材能读出 10 个幻影音符、9 个乱码歌词音节。
    // -------------------------------------------------------------------
    std::cout << "\n测试 MTrk 截断文件的安全解析\n";
    {
        auto makeBase = []() {
            SmfTrack t;
            t.at(0, ev({0xFF, 0x03, 0x05, 'V','o','c','a','l'}));
            t.at(0, ev({0x90, 60, 100}));
            t.at(480, ev({0x80, 60, 0}));
            t.at(480, ev({0x90, 62, 100}));
            t.at(960, ev({0x80, 62, 0}));
            return buildSmf(1, 480, {t});
        };
        auto inflate = [](std::vector<uint8_t> b, std::initializer_list<int> tail,
                          uint32_t extra, int whichTrack = 0) {
            // MThd 固定 14 字节；第 N 轨的长度字段 = 前一轨终点 + 4。
            size_t p = 18;
            if (whichTrack > 0) {
                const uint32_t first = ((uint32_t)b[18] << 24) |
                                       ((uint32_t)b[19] << 16) |
                                       ((uint32_t)b[20] << 8) | (uint32_t)b[21];
                p = 14 + 8 + first + 4;
            }
            const uint32_t len = ((uint32_t)b[p] << 24) | ((uint32_t)b[p+1] << 16) |
                                 ((uint32_t)b[p+2] << 8) | (uint32_t)b[p+3];
            const uint32_t big = len + extra;
            b[p] = (uint8_t)(big >> 24); b[p+1] = (uint8_t)(big >> 16);
            b[p+2] = (uint8_t)(big >> 8);  b[p+3] = (uint8_t)big;
            for (int x : tail) b.push_back((uint8_t)x);
            return b;
        };
        auto parse = [](const std::vector<uint8_t>& b) {
            MidiData d;
            const bool ok = loadMidiFromMemory(b.data(), b.size(), d);
            return std::make_pair(ok, std::move(d));
        };

        {
            auto r = parse(makeBase());
            check(r.first && r.second.notes.size() == 2 &&
                  r.second.truncatedTracks == 0 && r.second.truncatedEvents == 0 &&
                  truncationLabel(r.second).empty(),
                  "正常文件：2 个音、无截断计数、无告警");
        }
        {
            auto r = parse(inflate(makeBase(), {0x00, 0x90, 0x3C}, 200));
            check(r.first && r.second.notes.size() == 2,
                  "轨尾半个 note-on：只读出 2 个真音，不产生幻影音符");
            check(r.second.truncatedTracks == 1 && r.second.truncatedEvents >= 1,
                  "截断计数：1 条轨道越界 + 至少 1 个不完整事件");
            check(truncationLabel(r.second).find("文件数据不完整") == 0,
                  "页眉提示「文件数据不完整」");
            check(r.second.notes.size() == 2 &&
                  r.second.notes[0].pitch == 60 && r.second.notes[0].durationTicks == 480 &&
                  r.second.notes[1].pitch == 62,
                  "截断前的两个音音高 / 时值不受影响");
        }
        {
            // lyric meta 声明 32 字节，实际只剩 3 字节：旧实现读进乱码歌词
            auto r = parse(inflate(makeBase(), {0x00, 0xFF, 0x05, 0x20, 'a','b','c'}, 200));
            check(r.first && r.second.notes.size() == 2 && r.second.midiLyrics.empty(),
                  "被截断的 lyric meta 整条丢弃，不再冒出乱码歌词");
        }
        {
            // SysEx 声明 24 字节，实际只剩 3 字节
            auto r = parse(inflate(makeBase(), {0x00, 0xF0, 0x18, 0x7E, 0x7F, 0x09}, 200));
            check(r.first && r.second.notes.size() == 2 && r.second.sysExEvents == 0,
                  "被截断的 SysEx 整条丢弃，不产生鼓模式误判");
        }
        {
            // 只剩 delta，连状态字节都没有
            auto r = parse(inflate(makeBase(), {0x00}, 200));
            check(r.first && r.second.notes.size() == 2 && r.second.truncatedEvents >= 1,
                  "只剩 delta 的不完整事件停在 buffer 末尾");
        }
        {
            // 1 字节事件（0xC0 program）缺数据字节
            auto r = parse(inflate(makeBase(), {0x00, 0xC0}, 200));
            check(r.first && r.second.notes.size() == 2 &&
                  r.second.programChangeEvents == 0,
                  "缺 program 号的 0xC0 被丢弃，音色时间轴不受污染");
        }
        {
            // 两轨：只有第二轨的长度声明越过文件末尾
            SmfTrack a, b2;
            a.at(0, ev({0x90, 60, 100}));  a.at(480, ev({0x80, 60, 0}));
            b2.at(0, ev({0x90, 64, 100})); b2.at(480, ev({0x80, 64, 0}));
            auto r = parse(inflate(buildSmf(2, 480, {a, b2}),
                                   {0x00, 0x90, 0x3C}, 200, 1));
            check(r.first && r.second.notes.size() == 2 &&
                  r.second.truncatedTracks == 1,
                  "多轨文件：完整轨的音符保留，只有越界轨被截断");
        }
        {
            // 长度字段把残片也算进去了（trkEnd == size），只是这半个事件
            // 本身缺一个数据字节：记不完整事件，但不误报「整份文件被截断」。
            auto r = parse(inflate(makeBase(), {0x00, 0x90, 0x3C}, 3));
            check(r.first && r.second.notes.size() == 2 &&
                  r.second.truncatedTracks == 0 && r.second.truncatedEvents >= 1,
                  "长度字段正确时只记不完整事件，不误报轨道越界");
        }
    }

    // -------------------------------------------------------------------
    // CC66 持音踏板（Sostenuto）：只保持踩下那一刻正按着的键。
    // 与 CC64 的判定方向相反，绝不能把两种踏板合并进一条时间轴。
    // -------------------------------------------------------------------
    std::cout << "\n测试控制器语义：CC66 持音踏板\n";
    {
        auto durOf = [](const MidiData& d, int pitch) -> long {
            for (const auto& n : d.notes)
                if (n.pitch == pitch) return n.durationTicks;
            return -1;
        };
        auto load = [](const std::vector<uint8_t>& bytes, int filter = -1) {
            MidiData d;
            bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d, false, filter);
            return std::make_pair(ok, d);
        };
        auto chord = [](bool sost, bool sustain, int rel1, int rel2) {
            SmfTrack t;
            t.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
            for (int p : {60, 64, 67}) t.at(0, ev({0x90, p, 96}));
            if (sost) t.at(240, ev({0xB0, 0x42, 127}));
            for (int p : {60, 64, 67}) t.at(480, ev({0x80, p, 0x40}));
            t.at(480, ev({0x90, 62, 96}));       // 踩下之后才弹的音
            t.at(960, ev({0x80, 62, 0x40}));
            if (sustain) {
                t.at(600, ev({0xB0, 0x40, 127}));
                t.at(rel2, ev({0xB0, 0x40, 0}));
            }
            if (sost) t.at(rel1, ev({0xB0, 0x42, 0}));
            return t;
        };

        // 1) 基本语义：3 个和弦音被保持到 1920，之后新弹的 D4 仍是 480
        {
            auto b = buildSmf(1, 480, {chord(true, false, 1920, 1920)});
            auto r = load(b);
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 4, "CC66 基本样本读出 4 个音");
            check(d.sostenutoEvents == 2, "记下 2 个 CC66 事件");
            check(d.sostenutoExtendedNotes == 3, "持音踏板保持 3 个音");
            check(d.pedalEvents == 0 && d.pedalExtendedNotes == 0,
                  "纯 CC66 文件不误算成 CC64 延音踏板");
            check(durOf(d, 60) == 1920 && durOf(d, 64) == 1920 &&
                  durOf(d, 67) == 1920, "踩下时正按着的和弦延到抬起 tick 1920");
            check(durOf(d, 62) == 480, "踩下之后新弹的 D4 仍是 480（不被保持）");
            for (const auto& n : d.notes)
                if (n.pitch == 60)
                    check(n.rawDurationTicks == 480, "rawDurationTicks 仍是按键时长 480");
            check(controllerLabel(d).find("持音踏板") == 0,
                  "页眉诊断写明「持音踏板：保持 3 个音」");
        }

        // 2) 没有 CC66 的同一份文件：四个音全是 480（零回归）
        {
            auto b = buildSmf(1, 480, {chord(false, false, 1920, 1920)});
            auto r = load(b);
            check(r.second.sostenutoEvents == 0 &&
                  r.second.sostenutoExtendedNotes == 0 &&
                  durOf(r.second, 60) == 480 && durOf(r.second, 62) == 480 &&
                  durOf(r.second, 64) == 480 && durOf(r.second, 67) == 480,
                  "无 CC66 的对照样本时值逐字节不变");
        }

        // 3) CC66 与 CC64 同曲叠加：一个只用持音、一个只用延音
        {
            auto b = buildSmf(1, 480, {chord(true, true, 1920, 1920)});
            auto r = load(b);
            const MidiData& d = r.second;
            check(d.sostenutoExtendedNotes == 3 && d.pedalExtendedNotes == 1,
                  "两种踏板各自统计：持音 3 个、延音 1 个");
            check(durOf(d, 60) == 1920 && durOf(d, 62) == 1440,
                  "和弦由 CC66 延到 1920，D4 由 CC64 从 480 延到 1920（1440）");
            check(controllerLabel(d).find("持音踏板") != std::string::npos &&
                  controllerLabel(d).find("延音踏板") != std::string::npos,
                  "页眉同时报出两种踏板");
        }

        // 4) 迟滞：127→50→0 的 50 落在死区，音延到真正的 960 释放点
        {
            SmfTrack t;
            t.at(0, ev({0xB0, 0x42, 127}));
            t.at(480, ev({0xB0, 0x42, 50}));
            t.at(960, ev({0xB0, 0x42, 0}));
            t.at(0, ev({0x90, 60, 96}));
            t.at(120, ev({0x80, 60, 0x40}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.notes.size() == 1 && r.second.notes[0].durationTicks == 960,
                  "CC66 也走迟滞：死区 50 不提前抬起，延到 960");
        }
        {
            SmfTrack t;
            t.at(0, ev({0xB0, 0x42, 127}));
            t.at(480, ev({0xB0, 0x42, 20}));
            t.at(960, ev({0xB0, 0x42, 0}));
            t.at(0, ev({0x90, 60, 96}));
            t.at(120, ev({0x80, 60, 0x40}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.notes.size() == 1 && r.second.notes[0].durationTicks == 480,
                  "CC66 死区下方的 20 仍是真正的抬起（延到 480）");
        }

        // 5) 通道隔离：ch0 踩 CC66，ch1 的音不能被保持
        {
            SmfTrack t;
            t.at(0, ev({0x90, 60, 96}));   t.at(120, ev({0x80, 60, 0x40}));
            t.at(0, ev({0x91, 64, 96}));   t.at(120, ev({0x81, 64, 0x40}));
            t.at(60, ev({0xB0, 0x42, 127}));
            t.at(960, ev({0xB0, 0x42, 0}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(durOf(r.second, 60) == 960 && durOf(r.second, 64) == 120,
                  "CC66 按逻辑通道隔离：ch0 保持、ch1 不变");
        }

        // 6) 同键重新击打：被保持的音也要在重击处断开
        {
            SmfTrack t;
            t.at(0, ev({0x90, 60, 96}));   t.at(120, ev({0x80, 60, 0x40}));
            t.at(60, ev({0xB0, 0x42, 127}));
            t.at(960, ev({0x90, 60, 96})); t.at(1080, ev({0x80, 60, 0x40}));
            t.at(1920, ev({0xB0, 0x42, 0}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.notes.size() == 2 &&
                  r.second.notes[0].durationTicks == 960 &&
                  r.second.notes[1].durationTicks == 120,
                  "同键重击把 CC66 保持音截到重击点");
        }

        // 7) 强制清音 CC123：优先于踏板，把保持音截到清音点
        {
            SmfTrack t;
            t.at(0, ev({0x90, 60, 96}));   t.at(120, ev({0x80, 60, 0x40}));
            t.at(60, ev({0xB0, 0x42, 127}));
            t.at(960, ev({0xB0, 0x7B, 0}));
            t.at(1920, ev({0xB0, 0x42, 0}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.notes.size() == 1 &&
                  r.second.notes[0].durationTicks == 960 &&
                  r.second.allNotesOffEvents == 1,
                  "CC123 把 CC66 保持音截到清音点");
        }

        // 8) 到曲尾都没抬踏板：用最后一个音符的结尾收束，不无限拖长
        {
            SmfTrack t;
            t.at(0, ev({0x90, 60, 96}));   t.at(120, ev({0x80, 60, 0x40}));
            t.at(60, ev({0xB0, 0x42, 127}));
            t.at(960, ev({0x90, 64, 96})); t.at(1440, ev({0x80, 64, 0x40}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(durOf(r.second, 60) == 1440 && durOf(r.second, 64) == 480,
                  "未抬起的 CC66 用 maxTick 1440 收束");
        }

        // 9) 写在别轨的 CC66 仍然按全文件时间轴生效；--track 0 则隔离掉
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0x90, 60, 96}));
            v[0].at(120, ev({0x80, 60, 0x40}));
            v[1].at(60, ev({0xB0, 0x42, 127}));
            v[1].at(1920, ev({0xB0, 0x42, 0}));
            auto b = buildSmf(2, 480, v);
            auto full = load(b, -1);
            auto only = load(b, 0);
            check(full.second.sostenutoExtendedNotes == 1 &&
                  full.second.notes[0].durationTicks == 1920,
                  "别轨的 CC66 按全文件时间轴生效");
            check(only.second.sostenutoEvents == 0 &&
                  only.second.sostenutoExtendedNotes == 0 &&
                  only.second.notes.size() == 1 &&
                  only.second.notes[0].durationTicks == 120,
                  "--track 0 隔离掉别轨的 CC66，不补长选中轨");
        }
    }


    std::cout << "\n测试渐强渐弱记号的通道归属 (v1.40.0)\n";
    {
        MidiData d;
        d.division = 480;
        d.timeSig = TimeSignature{4, 2, true};
        for (int i = 0; i < 8; i++) {
            NoteEvent n;
            n.startTicks = i * 480;
            n.durationTicks = 480;
            n.rawDurationTicks = 480;
            n.pitch = 60 + (i % 4);
            n.track = 0;
            n.channel = 0;
            n.velocity = 96;
            d.notes.push_back(n);
        }
        NoteEvent bass;
        bass.startTicks = 0;
        bass.durationTicks = 3840;
        bass.rawDurationTicks = 3840;
        bass.pitch = 48;
        bass.track = 1;
        bass.channel = 1;
        bass.velocity = 64;
        d.notes.push_back(bass);

        // ch0 旋律渐弱、ch1 伴奏渐强，两条曲线的起点与终点都落在同一 tick。
        // 旧实现把这两条 wedge 合并进一个 ExpressionMark，后写覆盖先写，
        // 于是旋律谱会印 [cresc.]，力度档位却写 [ff]→[pp]，自相矛盾。
        const int down[7] = {120, 100, 80, 60, 40, 20, 10};
        const int up[7]   = {20, 40, 60, 80, 100, 120, 127};
        for (int i = 0; i < 7; i++) {
            d.dynamics.push_back({i * 240, down[i], 0});
            d.dynamics.push_back({i * 240, up[i], 1});
        }
        d.markers.push_back({0, "Verse"});

        ExpressionMap e(d);
        check(e.crescendoCount() == 1 && e.diminuendoCount() == 1,
              "两条通道的渐变各自识别出来");
        auto hasWedge = [](const std::vector<ExpressionMark>& v, int w) {
            for (const auto& m : v) if (m.wedge == w) return true;
            return false;
        };
        auto hasSection = [](const std::vector<ExpressionMark>& v,
                             const std::string& s) {
            for (const auto& m : v) if (m.section == s) return true;
            return false;
        };
        const auto mel = e.inRange(0, 1920, 0);
        const auto acc = e.inRange(0, 1920, 1);
        const auto all = e.inRange(0, 1920);
        check(hasWedge(mel, -1) && !hasWedge(mel, +1),
              "旋律通道只拿到渐弱，伴奏的渐强不再串进来");
        check(hasWedge(acc, +1) && !hasWedge(acc, -1),
              "伴奏通道只拿到自己的渐强");
        check(hasWedge(all, +1) && hasWedge(all, -1),
              "同 tick 的相反渐变分列，不再在合并时互相覆盖");
        check(hasSection(mel, "Verse") && hasSection(acc, "Verse"),
              "全局段落名对每个通道都可见");
        check(all.size() >= mel.size() && all.size() >= acc.size(),
              "不传通道时保留全量语义（向后兼容）");
        check(e.dynamicAt(0, 0) == "ff" && e.dynamicAt(0, 1) == "p",
              "力度档位仍按通道取，与渐变归属一致");
        check(e.inRange(2880, 3360, 0).empty(),
              "inRange 的区间边界不受通道参数影响");
        bool sectionFirst = true, sawChannel = false;
        for (const auto& m : all) {
            if (m.channel >= 0) sawChannel = true;
            else if (sawChannel) sectionFirst = false;
        }
        check(sectionFirst, "全局段落名排在通道级标记之前（旧输出顺序不变）");
    }
    // -------------------------------------------------------------------
    // CC67 软踏板（Soft Pedal）：只衰减踩下期间起音音符的 velocity，
    // 不动时值，也不改 CC11 / CC7 力度表。与 CC64/CC66 共用迟滞配对。
    // -------------------------------------------------------------------
    // -------------------------------------------------------------------
    // v1.43.0：通道级楔形记号不再只印第一声部。三个输出层原先分别用
    // if (chords) / if (showTempo) / if (partIdx == 1) 把 wedge 和全局
    // 段落名一起门控住，第二轨（或同轨第二声部）的 cresc. 整条丢失。
    // 输出层取标记的口径统一为 dominantChannel(该声部音符)。
    // -------------------------------------------------------------------
    std::cout << "\n测试声部主导通道与逐声部楔形记号 (v1.43.0)\n";
    {
        auto occurrences = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        {
            // 主导通道：音符数最多者胜出，并列时取最早出现的通道
            std::vector<NoteEvent> v;
            NoteEvent n;
            n.track = 0; n.pitch = 48; n.startTicks = 0; n.startSeconds = 0;
            n.durationTicks = 1920; n.durationSeconds = 2.0; n.velocity = 64;
            n.channel = 1;                       // 低音伴奏排在最前
            v.push_back(n);
            n.channel = 0; n.pitch = 60; n.durationTicks = 480;
            n.durationSeconds = 0.5;
            for (int i = 0; i < 3; i++) {
                n.startTicks = i * 480; n.startSeconds = i * 0.5;
                v.push_back(n);
            }
            check(dominantChannel(v) == 0,
                  "主导通道取音符数最多的通道，不是 ns.front().channel");
            std::vector<NoteEvent> tied;
            n.channel = 5; tied.push_back(n);
            n.channel = 2; tied.push_back(n);
            check(dominantChannel(tied) == 5, "并列时取最早出现的通道");
            std::vector<NoteEvent> none;
            check(dominantChannel(none) == -1, "空声部返回 -1（不过滤）");
        }
        {
            MidiData d;
            d.division = 480;
            d.timeSig.valid = true;
            d.tempoChanges.push_back({0, 120.0});
            d.trackNames[0] = "Vocal";
            d.trackNames[1] = "Pad";
            d.markers.push_back({0, "Verse"});
            NoteEvent m;
            m.track = 0; m.channel = 0; m.velocity = 96;
            m.durationTicks = 480; m.durationSeconds = 0.5;
            for (int i = 0; i < 8; i++) {
                m.pitch = 60 + (i % 4);
                m.startTicks = i * 480; m.startSeconds = i * 0.5;
                d.notes.push_back(m);
            }
            NoteEvent p = m;
            p.track = 1; p.channel = 1; p.pitch = 48; p.velocity = 64;
            p.durationTicks = 1920; p.durationSeconds = 2.0;
            p.startTicks = 0; p.startSeconds = 0.0;
            d.notes.push_back(p);
            p.startTicks = 1920; p.startSeconds = 2.0;
            d.notes.push_back(p);
            const int down[7] = {120, 100, 80, 60, 40, 20, 10};
            const int up[7]   = {20, 40, 60, 80, 100, 120, 127};
            for (int i = 0; i < 7; i++) {
                d.dynamics.push_back({i * 240, down[i], 0});
                d.dynamics.push_back({i * 240, up[i], 1});
            }
            KeyEstimate key{0, true};
            const std::string jp = formatJianpu(d, key);
            const size_t iV = jp.find("[Vocal]");
            const size_t iP = jp.find("[Pad]");
            check(iV != std::string::npos && iP != std::string::npos && iP > iV,
                  "两条独立轨各占一行谱");
            const std::string lineV = jp.substr(iV, iP - iV);
            const std::string lineP = jp.substr(iP);
            check(lineV.find("[dim.]") != std::string::npos &&
                  lineV.find("[cresc.]") == std::string::npos,
                  "文本谱 Vocal 行只印自己通道的渐弱");
            check(lineP.find("[cresc.]") != std::string::npos &&
                  lineP.find("[dim.]") == std::string::npos,
                  "文本谱 Pad 行补印自己通道的渐强（旧版整条丢失）");
            check(occurrences(jp, "[Verse]") == 1,
                  "全局段落名只印一次，不跟着第二行谱重复");

            const std::string xml = renderMusicXml(d, key);
            const size_t p1 = xml.find("<part id=\"P1\"");
            const size_t p2 = xml.find("<part id=\"P2\"");
            check(p1 != std::string::npos && p2 != std::string::npos && p2 > p1,
                  "MusicXML 写成两个 part");
            const std::string part1 = xml.substr(p1, p2 - p1);
            const std::string part2 = xml.substr(p2);
            check(part1.find("diminuendo") != std::string::npos &&
                  part1.find("crescendo") == std::string::npos,
                  "MusicXML P1 只写旋律通道的渐弱");
            check(part2.find("crescendo") != std::string::npos &&
                  part2.find("diminuendo") == std::string::npos,
                  "MusicXML P2 补写伴奏通道的渐强（旧版整条丢失）");
            check(occurrences(xml, "<words>Verse</words>") == 1,
                  "MusicXML 全局段落名只写一次");
        }
    }
    std::cout << "\n测试控制器语义：CC67 软踏板 (v1.41.0)\n";
    {
        auto load = [](const std::vector<uint8_t>& bytes, int filter = -1, double scale = -1.0) {
            MidiData d;
            if (scale >= 0.0) d.softPedalScale = scale;
            bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d, false, filter);
            return std::make_pair(ok, d);
        };
        auto velOf = [](const MidiData& d, long tick) -> int {
            for (const auto& n : d.notes)
                if (n.startTicks == tick) return n.velocity;
            return -1;
        };
        auto durOf = [](const MidiData& d, long tick) -> long {
            for (const auto& n : d.notes)
                if (n.startTicks == tick) return n.durationTicks;
            return -1;
        };
        const int vels[8] = {60, 110, 60, 110, 60, 60, 60, 60};
        auto melody = [&](bool soft, int softCh = 0, int deadZone = -1,
                          bool release = true) {
            SmfTrack t;
            t.at(0, ev({0xFF, 0x58, 0x04, 4, 2, 24, 8}));
            if (soft) t.at(1200, ev({0xB0 | softCh, 0x43, 127}));
            if (deadZone >= 0) t.at(1500, ev({0xB0 | softCh, 0x43, deadZone}));
            if (soft && release) t.at(2000, ev({0xB0 | softCh, 0x43, 0}));
            for (int i = 0; i < 8; i++) {
                t.at(i * 480, ev({0x90, 60 + i, vels[i]}));
                t.at(i * 480 + 400, ev({0x80, 60 + i, 0x40}));
            }
            return t;
        };

        // 1) 基本语义：区间内起音的 2 个音力度 ×0.7，时值一个都不动
        {
            auto b = buildSmf(1, 480, {melody(true)});
            auto r = load(b);
            const MidiData& d = r.second;
            check(r.first && d.notes.size() == 8, "CC67 样本读出 8 个音");
            check(d.softPedalEvents == 2, "记下 2 个 CC67 事件");
            check(d.softPedalNotes == 2, "只有踩下区间内起音的 2 个音被衰减");
            check(velOf(d, 1440) == 77 && velOf(d, 1920) == 42,
                  "110→77、60→42（×0.7 四舍五入）");
            check(velOf(d, 480) == 110 && velOf(d, 0) == 60,
                  "区间外的强音与弱音力度原样保留");
            check(durOf(d, 1440) == 400 && durOf(d, 480) == 400,
                  "软踏板不改按键时值");
            check(d.pedalEvents == 0 && d.sostenutoEvents == 0 &&
                  d.pedalExtendedNotes == 0 && d.sostenutoExtendedNotes == 0,
                  "纯 CC67 不误算成 CC64 / CC66");
            check(controllerLabel(d).find("软踏板") == 0,
                  "页眉诊断写明「软踏板：衰减 ... 个音的力度」");
            check(d.dynamics.empty(), "CC11 / CC7 力度表不被软踏板改写");
        }
        // 2) 没有 CC67 的同一份文件：力度逐字节不变（零回归）
        {
            auto b = buildSmf(1, 480, {melody(false)});
            auto r = load(b);
            const MidiData& d = r.second;
            check(d.softPedalEvents == 0 && d.softPedalNotes == 0,
                  "无 CC67 时不记任何软踏板统计（零回归）");
            check(velOf(d, 1440) == 110 && velOf(d, 1920) == 60 &&
                  velOf(d, 480) == 110, "无 CC67 的对照样本力度逐字节不变");
            check(controllerLabel(d).find("软踏板") == std::string::npos,
                  "无 CC67 时诊断不出现软踏板");
        }
        // 3) 通道隔离：ch1 踩 CC67 不该衰减 ch0 的音
        {
            auto b = buildSmf(1, 480, {melody(true, 1)});
            auto r = load(b);
            check(r.second.softPedalNotes == 0 && velOf(r.second, 1440) == 110,
                  "CC67 按逻辑通道隔离：ch1 的踏板不影响 ch0");
        }
        // 4) 迟滞：33..63 保持前态，<=32 才真的抬起
        {
            auto bh = buildSmf(1, 480, {melody(true, 0, 50)});
            auto rh = load(bh);
            check(rh.second.softPedalNotes == 2 && velOf(rh.second, 1440) == 77,
                  "CC67 也走迟滞：死区 50 不提前抬踏板");
            auto bl = buildSmf(1, 480, {melody(true, 0, 20)});
            auto rl = load(bl);
            check(rl.second.softPedalNotes == 1 && velOf(rl.second, 1920) == 60,
                  "死区下方的 20 是真抬起，之后的音不再衰减");
        }
        // 5) 到曲尾都没抬：用 maxTick 收束，不能让衰减无限延续
        {
            auto b = buildSmf(1, 480, {melody(true, 0, -1, false)});
            auto r = load(b);
            check(r.second.softPedalNotes == 5,
                  "未抬起的 CC67 用最后一个音的结尾收束（5 个音）");
        }
        // 6) 别轨的 CC67 按全文件时间轴生效；--track 0 则隔离掉
        {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0x90, 60, 110}));
            v[0].at(400, ev({0x80, 60, 0x40}));
            v[1].at(0, ev({0xB0, 0x43, 127}));
            v[1].at(2000, ev({0xB0, 0x43, 0}));
            auto b = buildSmf(2, 480, v);
            auto full = load(b, -1);
            auto only = load(b, 0);
            check(full.second.softPedalNotes == 1 &&
                  full.second.notes[0].velocity == 77,
                  "别轨的 CC67 按全文件时间轴生效");
            check(only.second.softPedalEvents == 0 &&
                  only.second.softPedalNotes == 0 &&
                  only.second.notes.size() == 1 &&
                  only.second.notes[0].velocity == 110,
                  "--track 0 隔离掉别轨的 CC67");
        }
        // 7) 与 CC64 同曲：一个只改力度，一个只改时值，互不干扰
        {
            SmfTrack t = melody(true);
            t.at(600, ev({0xB0, 0x40, 127}));
            t.at(1920, ev({0xB0, 0x40, 0}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.softPedalNotes == 2 &&
                  r.second.pedalExtendedNotes >= 1 &&
                  velOf(r.second, 1440) == 77,
                  "CC67 与 CC64 同曲时各管各的：力度 / 时值分开");
        }
        // 8) 力度下限：vel=1 已在地板，不再重复记软踏板、也不出现 0 力度
        {
            SmfTrack t;
            t.at(0, ev({0xB0, 0x43, 127}));
            t.at(0, ev({0x90, 60, 1}));
            t.at(400, ev({0x80, 60, 0x40}));
            t.at(480, ev({0xB0, 0x43, 0}));
            auto b = buildSmf(1, 480, {t});
            auto r = load(b);
            check(r.second.notes[0].velocity == 1 &&
                  r.second.softPedalNotes == 0,
                  "velocity 已是下限(1)不衰减、不记软踏板，绝不出现 0 力度");
        }
        // 9) 系数可配：scale=1.0 不衰减（还原被强制压暗的弱奏曲）
        {
            auto b = buildSmf(1, 480, {melody(true)});
            auto r = load(b, -1, 1.0);
            const MidiData& d = r.second;
            check(d.softPedalEvents == 2 && d.softPedalNotes == 0,
                  "scale=1.0 仍记 CC67 事件，但不衰减任何音符");
            check(velOf(d, 1440) == 110 && velOf(d, 1920) == 60,
                  "scale=1.0 力度逐字节还原（×1.0）");
            check(controllerLabel(d).find("软踏板") == std::string::npos,
                  "scale=1.0 没有实际衰减时诊断不报软踏板");
        }
        // 10) scale=0.5 半衰减：110→55、60→30（整数公式零漂移）
        {
            auto b = buildSmf(1, 480, {melody(true)});
            auto r = load(b, -1, 0.5);
            const MidiData& d = r.second;
            check(d.softPedalNotes == 2,
                  "scale=0.5 仍衰减区间内起音的 2 个音");
            check(velOf(d, 1440) == 55 && velOf(d, 1920) == 30,
                  "110→55、60→30（×0.5 四舍五入，纯整数公式）");
        }
        // 11) scale=0.0 完全关闭：连事件计数都归零，谱面彻底不体现软踏板
        {
            auto b = buildSmf(1, 480, {melody(true)});
            auto r = load(b, -1, 0.0);
            const MidiData& d = r.second;
            check(d.softPedalEvents == 0 && d.softPedalNotes == 0,
                  "scale=0.0 = --no-soft-pedal，软踏板完全关闭");
            check(velOf(d, 1440) == 110 && velOf(d, 1920) == 60,
                  "关闭后区间内的音力度原样保留");
            check(controllerLabel(d).find("软踏板") == std::string::npos,
                  "关闭后诊断不出现软踏板");
        }
    }

    // -------------------------------------------------------------------
    // SMF chunk 级边界重同步：轨间私有块、MTrk 长度少写 / 多写。
    // 旧实现的 while 要求每个 chunk 都是 MTrk，且轨尾强制 pos = trkEnd；
    // 任一处失准就停掉整个循环，后面整条旋律轨丢失（CLI 退出码 3），
    // 或多写时把下一轨的字节当成本轨事件读出幻影音符。
    // -------------------------------------------------------------------
    std::cout << "\n测试 SMF chunk 级边界重同步\n";
    {
        auto twoTracks = []() {
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            v[0].at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
            v[1].at(0, ev({0xFF, 0x03, 0x06, 'M','e','l','o','d','y'}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            v[1].at(480, ev({0x90, 62, 100}));
            v[1].at(960, ev({0x80, 62, 0}));
            return buildSmf(2, 480, std::move(v));
        };
        auto lenAt = [](const std::vector<uint8_t>& b, size_t p) {
            return (uint32_t(b[p]) << 24) | (uint32_t(b[p+1]) << 16) |
                   (uint32_t(b[p+2]) << 8) | uint32_t(b[p+3]);
        };
        auto putLenAt = [](std::vector<uint8_t>& b, size_t p, uint32_t v) {
            b[p] = uint8_t(v >> 24); b[p+1] = uint8_t(v >> 16);
            b[p+2] = uint8_t(v >> 8); b[p+3] = uint8_t(v);
        };
        auto privChunk = [](const char* id, std::vector<uint8_t> payload) {
            std::vector<uint8_t> c;
            for (int i = 0; i < 4; ++i) c.push_back((uint8_t)id[i]);
            put32(c, (uint32_t)payload.size());
            c.insert(c.end(), payload.begin(), payload.end());
            return c;
        };
        auto parse = [](const std::vector<uint8_t>& b) {
            MidiData d;
            const bool ok = loadMidiFromMemory(b.data(), b.size(), d);
            return std::make_pair(ok, std::move(d));
        };

        {
            auto r = parse(twoTracks());
            check(r.first && r.second.notes.size() == 2 &&
                  r.second.skippedChunks == 0 && r.second.resyncedTracks == 0 &&
                  truncationLabel(r.second).empty(),
                  "合法两轨：2 个音、无 chunk 修复计数、无告警");
        }
        {
            auto b = twoTracks();
            auto c = privChunk("XFIH", {1, 2, 3, 4});
            b.insert(b.begin() + 14, c.begin(), c.end());
            auto r = parse(b);
            check(r.second.notes.size() == 2 && r.second.skippedChunks == 1 &&
                  r.second.resyncedTracks == 0,
                  "MThd 后的私有 chunk 被跳过，旋律轨照常读出");
            check(truncationLabel(r.second).find("非 MTrk 扩展块") != std::string::npos,
                  "页眉写明跳过了非 MTrk 扩展块");
        }
        {
            auto b = twoTracks();
            const size_t p = 14 + 8 + lenAt(b, 18);
            auto c = privChunk("XFIH", {0xAA, 0xBB, 0xCC, 0xDD});
            b.insert(b.begin() + p, c.begin(), c.end());
            auto r = parse(b);
            check(r.second.notes.size() == 2 && r.second.skippedChunks == 1 &&
                  r.second.resyncedTracks == 0,
                  "两轨之间的私有 chunk 被跳过，旋律轨照常读出");
        }
        {
            auto b = twoTracks();
            const size_t p = 14 + 8 + lenAt(b, 18);
            auto c2 = privChunk("meta", {9});
            b.insert(b.begin() + p, c2.begin(), c2.end());
            auto c1 = privChunk("XFIH", {1, 2});
            b.insert(b.begin() + 14, c1.begin(), c1.end());
            auto r = parse(b);
            check(r.second.notes.size() == 2 && r.second.skippedChunks == 2,
                  "连续两个私有 chunk 都按声明长度跳过");
        }
        for (int delta : {-4, -12}) {
            auto b = twoTracks();
            putLenAt(b, 18, (uint32_t)((int64_t)lenAt(b, 18) + delta));
            auto r = parse(b);
            check(r.second.notes.size() == 2 && r.second.resyncedTracks == 1 &&
                  r.second.truncatedTracks == 0,
                  "首轨长度少写 " + std::to_string(-delta) +
                  " 字节：按 EOT 锚点重同步，旋律轨不丢");
        }
        {
            auto b = twoTracks();
            putLenAt(b, 18, (uint32_t)(b.size() - 22));  // 声明到 EOF，吞掉下一轨
            auto r = parse(b);
            check(r.second.notes.size() == 2 && r.second.resyncedTracks == 1,
                  "首轨长度多写吞掉下一轨：边界收紧，不读幻影音符");
            check(truncationLabel(r.second).find("重同步") != std::string::npos,
                  "页眉写明轨道边界已重同步");
        }
        {
            auto b = twoTracks();
            putLenAt(b, 18, lenAt(b, 18) - 4);
            MidiData d;
            const bool ok = loadMidiFromMemory(b.data(), b.size(), d, false, 1);
            check(ok && d.notes.size() == 2 && d.resyncedTracks == 1,
                  "--track 1 选中的轨在边界重同步后仍完整");
        }
        {
            // 歌词里的字面 "MTrk" 不能被当成轨头（缺 EOT 前缀 + 长度不可信）
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            v[1].at(0, ev({0xFF, 0x05, 19,
                           'M','T','r','k',' ','i','s',' ','n','o','t',' ',
                           'a',' ','c','h','u','n','k'}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            auto b = buildSmf(2, 480, std::move(v));
            auto r = parse(b);
            check(r.second.notes.size() == 1 && r.second.resyncedTracks == 0 &&
                  r.second.skippedChunks == 0,
                  "歌词里出现字面 MTrk 不会被误判成新轨头");
        }
        {
            // SysEx 数据同理：里面出现 MTrk 也不能切轨
            std::vector<SmfTrack> v(2);
            v[0].at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            v[1].at(0, ev({0xF0, 6, 'M','T','r','k', 0x7E, 0xF7}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            auto b = buildSmf(2, 480, std::move(v));
            auto r = parse(b);
            check(r.second.notes.size() == 1 && r.second.resyncedTracks == 0,
                  "SysEx 里出现字面 MTrk 不会被误判成新轨头");
        }
        {
            // v1.38 的真·截断不能被误报成「长度字段失准」
            std::vector<SmfTrack> v(1);
            v[0].at(0, ev({0x90, 60, 100}));
            v[0].at(480, ev({0x80, 60, 0}));
            auto b = buildSmf(1, 480, std::move(v));
            putLenAt(b, 18, lenAt(b, 18) + 200);
            auto r = parse(b);
            check(r.second.truncatedTracks == 1 && r.second.resyncedTracks == 0,
                  "真·截断文件仍按 v1.38 硬边界处理，不算 chunk 修复");
            check(truncationLabel(r.second).find("文件数据不完整") == 0,
                  "截断告警文案保持原样");
        }
        {
            // 截断 + 私有块同时出现：两段提示都要给全
            std::vector<SmfTrack> v(1);
            v[0].at(0, ev({0x90, 60, 100}));
            v[0].at(480, ev({0x80, 60, 0}));
            auto b = buildSmf(1, 480, std::move(v));
            putLenAt(b, 18, lenAt(b, 18) + 200);
            auto c = privChunk("XFIH", {1, 2, 3, 4});
            b.insert(b.begin() + 14, c.begin(), c.end());
            auto r = parse(b);
            const std::string lab = truncationLabel(r.second);
            check(r.second.truncatedTracks == 1 && r.second.skippedChunks == 1 &&
                  lab.find("文件数据不完整") == 0 &&
                  lab.find("非 MTrk 扩展块") != std::string::npos,
                  "截断与私有块同时存在时两段提示都保留");
        }
        {
            // v1.44.0：长度字段荒谬地大（越过文件末尾）不等于文件被截断。
            // 只要后面还有 EOT 锚定的完整 MTrk，就该按真实边界切开。
            auto b = twoTracks();              // 2 轨，第二轨 2 个音
            putLenAt(b, 18, 0x00010000u);      // 首轨声明 65536 字节
            auto r = parse(b);
            check(r.first && r.second.notes.size() == 2,
                  "越界长度字段：后续旋律轨的 2 个音照常读出");
            check(r.second.truncatedTracks == 0 && r.second.truncatedEvents == 0,
                  "越界长度字段：不再误判成文件数据被截断");
            check(r.second.resyncedTracks == 1,
                  "越界长度字段：记一次轨道边界重同步");
            const std::string lab = truncationLabel(r.second);
            check(lab.find("文件结构提示") == 0 &&
                  lab.find("重同步 1 条轨道边界") != std::string::npos &&
                  lab.find("文件数据不完整") == std::string::npos,
                  "越界长度字段：页眉只报结构修复，不报数据不完整");
        }
        {
            // 三轨 + 首轨越界：重同步后第二、第三轨仍要独立成轨
            std::vector<SmfTrack> v(3);
            v[0].at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            v[1].at(0, ev({0x90, 60, 100}));
            v[1].at(480, ev({0x80, 60, 0}));
            v[2].at(0, ev({0x90, 64, 100}));
            v[2].at(480, ev({0x80, 64, 0}));
            auto b = buildSmf(3, 480, std::move(v));
            putLenAt(b, 18, 0x00010000u);
            auto r = parse(b);
            check(r.first && r.second.notes.size() == 2,
                  "三轨首轨越界：后续两轨的音都读到");
            check(r.second.resyncedTracks == 1 && r.second.truncatedTracks == 0,
                  "三轨首轨越界：只重同步一次，不算截断");
            check(r.second.notes[0].track == 1 && r.second.notes[1].track == 2,
                  "三轨首轨越界：音符仍各自归属第 1 / 第 2 轨");
        }
        {
            // 后续轨头长度也不可信时锚点校验失败，仍按真·截断硬边界处理
            auto b = twoTracks();
            const uint32_t realLen = lenAt(b, 18);
            const size_t p2 = 14 + 8 + realLen;   // 第二轨 "MTrk" 的起点
            putLenAt(b, p2 + 4, 0x00010000u);
            putLenAt(b, 18, 0x00010000u);
            auto r = parse(b);
            check(r.second.truncatedTracks == 1 && r.second.resyncedTracks == 0,
                  "后续轨头长度也不可信：仍按 v1.38 真·截断处理");
        }
    }

    // -------------------------------------------------------------------
    // v1.45.0：MusicXML 一个 part 里的多个 <voice> 各自写 direction。
    // 旧实现整份 part 只取一个主导通道，副声部的 crescendo 与 p→ff 全丢；
    // 修复后按 splitVoices 分组各算一次 dominantChannel，direction 带
    // <voice> 归属、wedge number 按声部递增；单声部输出仍逐字节不变。
    // -------------------------------------------------------------------
    std::cout << "\n测试 MusicXML 逐声部 direction (v1.45.0)\n";
    {
        auto occurrences = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const int up[7]   = {20, 40, 60, 80, 100, 120, 127};
        const int down[7] = {120, 100, 80, 60, 40, 20, 10};
        const int mid[7]  = {30, 45, 60, 75, 90, 105, 120};
        const KeyEstimate key{0, true};

        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        d.trackNames[0] = "Piano";
        d.markers.push_back({0, "Verse"});
        NoteEvent n;
        n.track = 0; n.velocity = 96;
        n.startSeconds = 0; n.durationSeconds = 0.5;
        for (int i = 0; i < 8; i++) {
            n.channel = 0; n.pitch = 76 + (i % 4);
            n.startTicks = i * 480; n.durationTicks = 480;
            d.notes.push_back(n);
        }
        n.channel = 1; n.pitch = 60; n.durationTicks = 1920;
        n.startTicks = 0; d.notes.push_back(n);
        n.pitch = 62; n.startTicks = 1920; d.notes.push_back(n);
        n.channel = 2; n.pitch = 48; n.durationTicks = 3840;
        n.startTicks = 0; d.notes.push_back(n);
        for (int i = 0; i < 7; i++) {
            d.dynamics.push_back({i * 240, up[i], 0});
            d.dynamics.push_back({i * 240, down[i], 1});
            d.dynamics.push_back({i * 240, mid[i], 2});
        }
        const std::string xml = renderMusicXml(d, key);
        check(occurrences(xml, "crescendo") == 2 &&
              occurrences(xml, "diminuendo") == 1,
              "三个声部各自写出渐强 / 渐弱，副声部不再丢失");
        check(xml.find("number=\"3\"") != std::string::npos,
              "wedge number 按声部递增，两个渐强不抢同一个编号");
        check(xml.find("        <voice>3</voice>") != std::string::npos,
              "direction 带 <voice>3 归属");
        check(occurrences(xml, "<words>Verse</words>") == 1,
              "全局段落名仍只写一次");

        // 单声部零回归：direction 里不该出现缩进的 <voice> 子元素
        MidiData one;
        one.division = 480;
        one.timeSig.valid = true;
        one.tempoChanges.push_back({0, 120.0});
        one.trackNames[0] = "Piano";
        one.markers.push_back({0, "Verse"});
        NoteEvent m;
        m.track = 0; m.channel = 0; m.velocity = 96;
        m.startSeconds = 0; m.durationSeconds = 0.5;
        m.durationTicks = 480;
        for (int i = 0; i < 8; i++) {
            m.pitch = 60 + (i % 4); m.startTicks = i * 480;
            one.notes.push_back(m);
        }
        for (int i = 0; i < 7; i++)
            one.dynamics.push_back({i * 240, up[i], 0});
        const std::string oneXml = renderMusicXml(one, key);
        check(occurrences(oneXml, "        <voice>") == 0,
              "单声部 part 的 direction 保持旧版逐字节输出");
    }

    // -------------------------------------------------------------------
    // 键盘大谱表（v1.47.0）
    // 旧实现整份 part 只按平均音高挑一个谱号，钢琴左手被迫挤在高音谱号里；
    // 修复后键盘族乐器且音域够宽并跨中音 C 时排成上下两行谱，音符带
    // <staff> 归属，小节内用 <backup> 切谱表游标。
    // -------------------------------------------------------------------
    std::cout << "\n测试 MusicXML 键盘大谱表 (v1.47.0)\n";
    {
        auto occurrences = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const KeyEstimate key{0, true};

        // 宽音域钢琴：C2(36)..C6(84)，跨度 48 半音且跨中音 C
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        d.trackNames[0] = "Piano";
        d.channelProgram[0] = 0;
        NoteEvent n;
        n.track = 0; n.channel = 0; n.velocity = 90;
        n.startSeconds = 0; n.durationSeconds = 0.5;
        const int rh[4] = {72, 74, 76, 77};
        for (int i = 0; i < 4; i++) {
            n.pitch = rh[i]; n.startTicks = i * 480; n.durationTicks = 480;
            d.notes.push_back(n);
        }
        n.pitch = 36; n.startTicks = 0; n.durationTicks = 960;
        d.notes.push_back(n);
        n.pitch = 84; n.startTicks = 1920; n.durationTicks = 960;
        d.notes.push_back(n);

        const std::string gx = renderMusicXml(d, key);
        check(gx.find("<staves>2</staves>") != std::string::npos,
              "宽音域键盘声部声明两行谱");
        check(gx.find("<clef number=\"1\"><sign>G</sign><line>2</line></clef>") != std::string::npos,
              "上行谱用高音谱号");
        check(gx.find("<clef number=\"2\"><sign>F</sign><line>4</line></clef>") != std::string::npos,
              "下行谱用低音谱号");
        check(occurrences(gx, "<staff>2</staff>") >= 1 &&
              occurrences(gx, "<staff>1</staff>") >= 1,
              "音符按音高分流到上下谱表");
        check(gx.find("<backup>") != std::string::npos,
              "小节内在两行谱之间用 <backup> 切游标");

        // 反例 1：不是键盘音色（小提琴 40），即使音域宽也不排大谱表
        MidiData v;
        v.division = 480;
        v.timeSig.valid = true;
        v.tempoChanges.push_back({0, 120.0});
        v.trackNames[0] = "Violin";
        v.channelProgram[0] = 40;
        NoteEvent vn;
        vn.track = 0; vn.channel = 0; vn.velocity = 90;
        vn.startSeconds = 0; vn.durationSeconds = 0.5;
        vn.pitch = 36; vn.startTicks = 0; vn.durationTicks = 480;
        v.notes.push_back(vn);
        vn.pitch = 84; vn.startTicks = 480; vn.durationTicks = 480;
        v.notes.push_back(vn);
        check(renderMusicXml(v, key).find("<staves>") == std::string::npos,
              "非键盘音色的宽音域声部不排大谱表");

        // 反例 2：键盘音色但音域窄（C4..G4，跨度 7 半音），保持单谱表
        MidiData c;
        c.division = 480;
        c.timeSig.valid = true;
        c.tempoChanges.push_back({0, 120.0});
        c.trackNames[0] = "Piano";
        c.channelProgram[0] = 0;
        NoteEvent cn;
        cn.track = 0; cn.channel = 0; cn.velocity = 90;
        cn.startSeconds = 0; cn.durationSeconds = 0.5;
        cn.durationTicks = 480;
        for (int i = 0; i < 4; i++) {
            cn.pitch = 60 + i * 2; cn.startTicks = i * 480;
            c.notes.push_back(cn);
        }
        const std::string cx = renderMusicXml(c, key);
        check(cx.find("<staves>") == std::string::npos &&
              cx.find("<staff>") == std::string::npos,
              "键盘音色但音域窄的声部保持单谱表");
    }

    // -------------------------------------------------------------------
    // 小节中途变速的位置（v1.48.0）
    // 旧实现按小节把速度记号收进 map<bar, mark>：同一小节的多个阶跃只留
    // 最后一个（120→90→60 被吞成 60），且一律写在小节开头，MuseScore 从
    // 第一拍就按新速度播放。修复后每个阶跃各写一条 direction，落点用
    // <offset>（单位 = divisions）标回小节内位置；正好在小节线上的速度
    // 不写 offset，旧排版逐字节保留。
    // -------------------------------------------------------------------
    std::cout << "\n测试 MusicXML 小节中途变速 (v1.48.0)\n";
    {
        auto occurrences = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const KeyEstimate key{0, true};

        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        d.tempoChanges.push_back({480, 90.0});    // 第 1 小节第 2 拍
        d.tempoChanges.push_back({1440, 60.0});   // 第 1 小节第 4 拍
        d.tempoChanges.push_back({3840, 120.0});  // 第 3 小节线
        d.trackNames[0] = "Melody";
        d.channelProgram[0] = 73;
        NoteEvent n;
        n.track = 0; n.channel = 0; n.velocity = 96;
        n.startSeconds = 0; n.durationSeconds = 0.5;
        n.durationTicks = 480;
        for (int i = 0; i < 12; i++) {
            n.pitch = 72 + (i % 4); n.startTicks = i * 480;
            d.notes.push_back(n);
        }

        const std::string mx = renderMusicXml(d, key);
        check(mx.find("<offset>480</offset>") != std::string::npos,
              "第 2 拍的速度记号写 offset 480，不再吸附到小节开头");
        check(mx.find("<offset>1440</offset>") != std::string::npos,
              "第 4 拍的速度记号写 offset 1440");
        check(occurrences(mx, "<per-minute>90</per-minute>") == 1 &&
              occurrences(mx, "<per-minute>60</per-minute>") == 1,
              "同一小节的多个阶跃全部保留，不再只留最后一个");
        check(occurrences(mx, "<offset>") == 2,
              "只有中途变速的点写 offset，小节线上的速度不多写");
        check(mx.find("</direction-type><sound tempo=\"120\"/>") != std::string::npos,
              "正好落在小节线的速度保持旧排版（sound 紧贴 direction-type）");

        // 渐变落定点在小节中途：rit. 的终点速度要标到真实拍位
        MidiData r;
        r.division = 480;
        r.timeSig.valid = true;
        r.tempoChanges.push_back({0, 120.0});
        for (int i = 1; i <= 8; i++)          // 一段连续渐慢：8 个节点
            r.tempoChanges.push_back({i * 240L, 120.0 - i * 5.0});
        r.trackNames[0] = "Melody";
        NoteEvent rn;
        rn.track = 0; rn.channel = 0; rn.velocity = 96;
        rn.startSeconds = 0; rn.durationSeconds = 0.5;
        rn.durationTicks = 480;
        for (int i = 0; i < 8; i++) {
            rn.pitch = 72 + (i % 4); rn.startTicks = i * 480;
            r.notes.push_back(rn);
        }
        const std::string rx = renderMusicXml(r, key);
        check(rx.find("<words>rit.</words>") != std::string::npos &&
              rx.find("<offset>") != std::string::npos,
              "渐慢落定点不在小节线时也带 offset");
    }

    // MusicXML：小节中途换力度档（v1.49.0）
    {
        // 旧实现只在每小节开头取 dynamicAt(barStart)，中途的 p 被 f 顶掉，
        // 整条档位直接消失，f 也被挤到下一小节。
        const KeyEstimate key{0, true};
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Piano";
        NoteEvent n;
        n.track = 0; n.channel = 0; n.velocity = 96;
        n.durationSeconds = 0.5; n.durationTicks = 480;
        for (int i = 0; i < 12; i++) {
            n.pitch = 60 + (i % 4); n.startTicks = i * 480;
            n.startSeconds = i * 0.5;
            d.notes.push_back(n);
        }
        d.dynamics.push_back({0, 100, 0});      // ff（第 1 小节头）
        d.dynamics.push_back({480, 20, 0});     // p（第 1 小节第 2 拍）
        d.dynamics.push_back({1440, 80, 0});    // f（第 1 小节第 4 拍）
        const std::string dx = renderMusicXml(d, key);
        check(occurrences(dx, "<dynamics><ff/></dynamics>") == 1 &&
              occurrences(dx, "<dynamics><p/></dynamics>") == 1 &&
              occurrences(dx, "<dynamics><f/></dynamics>") == 1,
              "小节中途的每个力度档位都写出来，不被整条吞掉");
        check(occurrences(dx, "<dynamics><f/></dynamics>") == 1,
              "被推迟到下一小节的档位只写一次");
        const size_t pp = dx.find("<dynamics><p/></dynamics>");
        check(pp != std::string::npos &&
              dx.find("<offset>480</offset>", pp) < pp + 300,
              "中途换档的 p 带 offset 480");
        const size_t pf = dx.find("<dynamics><f/></dynamics>");
        check(pf != std::string::npos &&
              dx.find("<offset>1440</offset>", pf) < pf + 300,
              "中途换档的 f 带 offset 1440");
        check(occurrences(dx, "<offset>") == 2,
              "只有小节中途的换档写 offset，小节线的力度不多写");

        // 连续渐强：中间档位由楔形表达，不能再逐个印成力度记号。
        MidiData r;
        r.division = 480;
        r.timeSig.valid = true;
        r.trackNames[0] = "Piano";
        for (int i = 0; i < 12; i++) {
            n.pitch = 60 + (i % 4); n.startTicks = i * 480;
            r.notes.push_back(n);
        }
        for (int i = 0; i < 8; i++)             // 40 -> 110 的一段渐强
            r.dynamics.push_back({i * 240L, 40 + i * 10, 0});
        const std::string cx = renderMusicXml(r, key);
        check(cx.find("<wedge type=\"crescendo\"") != std::string::npos,
              "连续渐强仍写楔形");
        check(occurrences(cx, "<offset>") == 0,
              "渐变段内部的中间档位不再逐个印成力度记号");
        check(occurrences(cx, "<dynamics>") == 2,
              "渐强只在小节头写起止档位，不给每个采样点补 <direction>");
    }

    // 文本谱 / HTML：小节中途换力度档要落到真实拍位（v1.50.0）
    {
        // 旧实现只在 switchToBar / openBar 里取一次 dynamicAt，小节中途的
        // 换档在这两层同样没有出口：p 整条消失、f 被推迟到下一小节。
        const KeyEstimate key{0, true};
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Piano";
        NoteEvent n;
        n.track = 0; n.channel = 0; n.velocity = 96;
        n.durationSeconds = 0.5; n.durationTicks = 480;
        for (int i = 0; i < 12; i++) {
            n.pitch = 60 + (i % 4); n.startTicks = i * 480;
            n.startSeconds = i * 0.5;
            d.notes.push_back(n);
        }
        d.dynamics.push_back({0, 100, 0});      // ff（第 1 小节头）
        d.dynamics.push_back({480, 20, 0});     // p（第 1 小节第 2 拍）
        d.dynamics.push_back({1440, 80, 0});    // f（第 1 小节第 4 拍）

        const std::string txt = formatJianpu(d, key);
        check(occurrences(txt, "[ff]") == 1 && occurrences(txt, "[p]") == 1 &&
              occurrences(txt, "[f]") == 1,
              "文本谱写全小节中途的每个档位，不被整条吞掉");
        const size_t barLine = txt.find('|');
        const size_t pAt = txt.find("[p]");
        const size_t ffAt = txt.find("[ff]");
        check(ffAt != std::string::npos && pAt != std::string::npos &&
              barLine != std::string::npos && ffAt < pAt && pAt < barLine,
              "文本谱的 [p] 落在第 1 小节内、[ff] 之后");
        // 第 2 小节开头档位仍是 f，没有变化就不该再标一次。
        check(txt.find("[f]", barLine) == std::string::npos,
              "档位没变的小节开头不重复标力度");

        const std::string htm = renderHtml(d, key, "t");
        check(htm.find("class='dynx'>p<") != std::string::npos &&
              htm.find("class='dynx'>f<") != std::string::npos,
              "HTML 把中途换档浮在出事那个音头上");
        check(htm.find(".dynx{") != std::string::npos,
              "用到力度浮标时随页带 dynx 样式");

        // 没有中途换档的谱子不能凭空多出样式：既有素材的 HTML 逐字节不变
        // 全靠这条（CSS 是内嵌的，多一段就整份文件都变）。
        MidiData plain = d;
        plain.dynamics.clear();
        plain.dynamics.push_back({0, 100, 0});
        check(renderHtml(plain, key, "t").find("dynx") == std::string::npos,
              "没有中途换档时不输出 dynx 样式");

        // v1.72.0 试听播放器（--player）：默认不生成任何东西，开了才带数据。
        // 这也是零回归的护栏——不带开关的页面必须一个字节都不多。
        {
            MidiData mp;
            mp.division = 480;
            mp.timeSig.valid = true;
            NoteEvent e0;
            e0.track = 1; e0.channel = 0; e0.startTicks = 0;
            e0.durationTicks = 480; e0.pitch = 60;
            mp.notes.push_back(e0);
            e0.startTicks = 480; e0.pitch = 64;
            mp.notes.push_back(e0);
            const KeyEstimate mpk = estimateKey(mp);
            const std::string off = renderHtml(mp, mpk, "t");
            check(off.find("data-t=") == std::string::npos &&
                  off.find("<script") == std::string::npos &&
                  off.find("playerbar") == std::string::npos,
                  "不带 --player 时页面没有任何播放器痕迹");
            const std::string on = renderHtml(mp, mpk, "t", nullptr, true);
            check(on.find("M2J_PLAY") != std::string::npos &&
                  on.find("data-t='0'") != std::string::npos &&
                  on.find("id='playerbar'") != std::string::npos,
                  "带 --player 时内嵌播放数据与高亮锚点");
            check(on.find("\"notes\":[[0,480,60,1,96],[480,480,64,1,96]]")
                      != std::string::npos,
                  "播放数据按「起点/时长/音高/轨号/力度」逐音导出");
            check(on.find("#playerbar{display:none}") != std::string::npos,
                  "打印时隐藏播放器");

            // v1.73.0：力度进播放数据，且是每个音自己的 note-on velocity
            // （以前只拿它判重音、不送进播放层，重音听着和弱起一样响）。
            MidiData vd = mp;
            vd.notes[0].velocity = 24;
            vd.notes[1].velocity = 120;
            const std::string von = renderHtml(vd, estimateKey(vd), "t", nullptr, true);
            check(von.find("[0,480,60,1,24]") != std::string::npos &&
                  von.find("[480,480,64,1,120]") != std::string::npos,
                  "每个音的力度都进播放数据（不是整份一个常数）");
            // 0 与 200 都是非法力度，必须钳到 1 / 127，否则播放端按 0 除或算出负增益。
            MidiData cd = mp;
            cd.notes[0].velocity = 0;
            cd.notes[1].velocity = 200;
            const std::string con = renderHtml(cd, estimateKey(cd), "t", nullptr, true);
            check(con.find("[0,480,60,1,1]") != std::string::npos &&
                  con.find("[480,480,64,1,127]") != std::string::npos,
                  "越界力度被钳到 1 / 127");
            check(on.find("点谱面上任一音符") != std::string::npos &&
                  on.find("addEventListener('click'") != std::string::npos &&
                  on.find(".note[data-t]{cursor:pointer}") != std::string::npos,
                  "点击谱面音符即可从该处起播（事件委托 + 手型光标）");
        }

        // v1.74.0 每行小节数（--bars N）：默认仍按「一行 16 拍」折算，传了才定死。
        {
            MidiData bd;
            bd.division = 480;
            bd.timeSig.valid = true;
            for (int i = 0; i < 8; i++) {
                NoteEvent e;
                e.track = 1; e.channel = 0; e.startTicks = i * 1920L;
                e.durationTicks = 480; e.pitch = 60;
                bd.notes.push_back(e);
            }
            const KeyEstimate bk = estimateKey(bd);
            const std::string d0 = renderHtml(bd, bk, "t");
            const std::string d1 = renderHtml(bd, bk, "t", nullptr, false, 0);
            check(d0 == d1, "barsPerRow=0 与不传参数逐字节一致（自动折算）");
            check(occurrences(d0, "<div class='row'>") == 2,
                  "4/4 八小节按自动折算排成 2 行");
            check(d0.find(".row{break-inside:avoid") == std::string::npos,
                  "默认不加打印整行保护（零回归）");
            const std::string b2 = renderHtml(bd, bk, "t", nullptr, false, 2);
            check(occurrences(b2, "<div class='row'>") == 4,
                  "--bars 2 把八小节排成 4 行");
            check(b2.find(".row{break-inside:avoid") != std::string::npos,
                  "--bars 顺带加上「一行不被分页撕开」");
            const std::string b3 = renderHtml(bd, bk, "t", nullptr, false, 3);
            check(occurrences(b3, "<div class='row'>") == 3 &&
                  occurrences(b3, "<span class='measure'>") == 8,
                  "--bars 3 排成 3 行（末行 2 小节），小节总数仍为 8");
        }

        // v1.75.0 弱起 × 变拍号的网格边界：弱起小节永远按曲首拍号（它短于一小节，
        // 不存在「按新拍号重算」的问题），换拍号从它之后的那一整小节起生效。
        {
            MidiData d;
            d.division = 480;
            d.timeSig.valid = true;                     // 4/4
            NoteEvent e;
            e.track = 1; e.channel = 0; e.pitch = 60; e.durationTicks = 240;
            d.notes.push_back(e);                       // 弱起那一拍
            for (int i = 1; i < 10; i++) {
                e.startTicks = 480 * i; e.pitch = 60 + (i % 3);
                d.notes.push_back(e);
            }
            d.pickupTicks = 480;                        // 手动 1 拍弱起
            TimeSignature threeFour{3, 2, true};         // 3/4
            d.timeSigChanges.push_back({960, threeFour});   // 落在第 1 小节内
            const BarGrid g = barGridOf(d);
            check(g.pickup == 480 && g.barStart(1) == 480,
                  "弱起小节仍在 0~480，第一个完整小节从 480 起");
            check(g.sigForBar(0).numerator == 4 && g.sigForBar(0).denomPow == 2,
                  "弱起小节按曲首拍号（4/4），不受后面的换拍号影响");
            // 换拍号落在第 1 小节中段，按「就在事件 tick 处截断」处理：
            // 第 1 小节被切成 480~960 与 960~2400 两段，后一段起改用 3/4。
            check(g.barIndex(960) > g.barIndex(480),
                  "换拍号 tick 之后算新的一小节");
            check(g.sigForBar(g.barIndex(960)).numerator == 3,
                  "换拍号之后的小节用 3/4，实际 "
                      + std::to_string(g.sigForBar(g.barIndex(960)).numerator));
            // 不变量：起点严格递增、每段长度 == 相邻起点之差、尾长 == 最后一段
            bool inc = true, span = true;
            for (size_t k = 1; k < g.starts.size(); k++)
                if (g.starts[k] <= g.starts[k - 1]) inc = false;
            for (size_t k = 0; k + 1 < g.starts.size(); k++)
                if (g.barSpan((int)k) != g.starts[k + 1] - g.starts[k]) span = false;
            check(inc, "小节起点严格递增");
            check(span, "每小节长度 == 相邻起点之差");
            check(g.tailBarLen == g.barSpan((int)g.starts.size() - 2),
                  "tailBarLen 等于最后一段的小节长度");
            // 每个起点都必须落在自己那一小节里（跨小节查找的根）
            bool self = true;
            for (size_t k = 0; k < g.starts.size(); k++)
                if (g.barIndex(g.starts[k]) != (int)k) self = false;
            check(self, "barIndex(每小节起点) == 该小节序号");
        }
        // 调号五度圈跨零点：+7 等价于 -5，但不能写成 7（七升号）；
        // 归一区间是 [-6,6]，跨过去要取升降号少的那一侧。
        check(transposeFifths(5, 2) == -5, "5 + 2 = 7 要归一成 -5，实际 "
              + std::to_string(transposeFifths(5, 2)));
        check(transposeFifths(-5, -2) == 5, "-5 - 2 = -7 要归一成 5，实际 "
              + std::to_string(transposeFifths(-5, -2)));
        check(transposeFifths(6, 0) == 6 && transposeFifths(0, 0) == 0,
              "边界 +6 与 0 原样保留（不越界）");
        // -6 与 +6 在模 12 下同余。先取模再折半会把 Gb 折成 F#——这正是
        // 「六个降号被印成六个升号」的根因，所以这里必须原样保留。
        check(transposeFifths(-6, 0) == -6, "-6 原样保留（不折成 +6），实际 "
              + std::to_string(transposeFifths(-6, 0)));
        check(transposeFifths(-6, 12) == -6, "Gb 移一个八度仍是 -6");

        // 渐变段内部的中间档位由楔形表达，文本谱不能逐点印成 [mp]。
        MidiData r;
        r.division = 480;
        r.timeSig.valid = true;
        r.trackNames[0] = "Piano";
        for (int i = 0; i < 12; i++) {
            n.pitch = 60 + (i % 4); n.startTicks = i * 480;
            r.notes.push_back(n);
        }
        for (int i = 0; i < 8; i++)
            r.dynamics.push_back({i * 240L, 40 + i * 10, 0});
        const std::string rt = formatJianpu(r, key);
        check(rt.find("[cresc.]") != std::string::npos,
              "连续渐强在文本谱里仍写成 [cresc.]");
        // 渐变段内部的档位（40→110 的 8 个采样点）只该在栏头留一个起点、
        // 下一栏头留一个落定值；若逐点印出来，这里会冒出 [mf] / [f] 等一长串。
        size_t dynMarks = 0;
        for (const char* lv : {"[pp]", "[p]", "[mp]", "[mf]", "[f]", "[ff]", "[fff]"})
            dynMarks += occurrences(rt, lv);
        check(dynMarks == 2,
              "渐变段内部的中间档位不逐点印成力度记号，只在栏头标起止");
    }

    // ---- 移调：调号必须跟着音高一起走（v1.51.0）----
    // 只移音不移调号，谱面就会「音符在 D 大调、页眉还写 1=C」：简谱唱名
    // 从 1 2 3 4 变成 2 3 #4，MusicXML 的 <fifths> 留在 0，整首挂临时记号。
    {
        // 五度圈：纯五度 = 7 半音，7x7 ≡ 1 (mod 12)，所以移 N 半音走 7N 步。
        check(transposeFifths(0, 2) == 2, "移调 +2：C(0) -> D(2 个升号)");
        check(transposeFifths(0, -2) == -2, "移调 -2：C(0) -> Bb(2 个降号)");
        check(transposeFifths(0, 7) == 1, "移调 +7：纯五度只差一个升号 C(0) -> G(1)");
        // 跨出零点时取升降号最少的等价写法：C# 要 7 个升号，Db 只要 5 个降号。
        check(transposeFifths(0, 1) == -5, "移调 +1：C(0) -> Db(-5)，不写 7 升的 C#");
        check(transposeFifths(0, -1) == 5, "移调 -1：C(0) -> B(5)，不写 7 降的 Cb");
        check(transposeFifths(-2, 2) == 0, "降号调跨零点：Bb(-2) +2 -> C(0)");
        check(transposeFifths(0, 12) == 0 && transposeFifths(-2, -12) == -2,
              "移整个八度不改调号");
        check(transposeFifths(3, 6) == -3, "升号调绕回：A(3) +6 -> Eb(-3)");

        // 手动的 --key / --gong 也要跟着走：走调号取最简写法，而不是把
        // 主音直接 +N（那样 C+3 会写成 9 个升号的 D#，谱面没法看）。
        KeyEstimate cMaj;
        cMaj.tonicPitchClass = 0;
        KeyEstimate aMin;
        aMin.tonicPitchClass = 9;
        aMin.isMajor = false;
        check(keyName(transposeKey(cMaj, 2)) == "D",
              "transposeKey：1=C +2 -> 1=D");
        check(keyName(transposeKey(cMaj, 1)) == "Db",
              "transposeKey：1=C +1 -> 1=Db，不写满升号的 C#");
        check(keyName(transposeKey(cMaj, 3)) == "Eb",
              "transposeKey：1=C +3 -> 1=Eb，不写 D#");
        check(keyName(transposeKey(aMin, 2)) == "D" &&
              !transposeKey(aMin, 2).isMajor,
              "transposeKey 保持大小调性质：a 小调 +2 -> b 小调（记作 1=D）");
        check(keyName(transposeKey(cMaj, 12)) == "C",
              "transposeKey 移八度不改调");

        // transposeData：音符与 keySig / keySigChanges 一起平移。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Melody";
        d.keySig = KeySignature{-2, false, true};             // 1=Bb
        d.keySigChanges.push_back({0, d.keySig});
        d.keySigChanges.push_back({7680, KeySignature{2, false, true}});  // 中途转 1=D
        // 两段都写足七声，免得被五声调式判定改掉 do 的位置。
        const int bbScale[8] = {70, 72, 74, 75, 77, 79, 81, 82};   // Bb C D Eb F G A Bb
        const int ddScale[8] = {62, 64, 66, 67, 69, 71, 73, 74};   // D E F# G A B C# D
        for (int i = 0; i < 8; i++) {
            NoteEvent m;
            m.startSeconds = i * 0.5;
            m.durationSeconds = 0.5;
            m.pitch = bbScale[i];
            m.startTicks = i * 480;
            m.durationTicks = 480;
            m.rawDurationTicks = 480;
            m.channel = 0;
            m.track = 0;
            d.notes.push_back(m);
            m.pitch = ddScale[i];
            m.startTicks = 7680 + i * 480;
            d.notes.push_back(m);
        }

        MidiData up = d;
        transposeData(up, 2);
        bool allShifted = true;
        for (size_t i = 0; i < d.notes.size(); i++)
            allShifted = allShifted && up.notes[i].pitch == d.notes[i].pitch + 2;
        check(allShifted, "transposeData 把每个音符的音高都平移");
        check(up.keySig.sharpsFlats == 0,
              "transposeData 把曲首调号 Bb(-2) 平移到 C(0)");
        check(up.keySigChanges.size() == 2 &&
              up.keySigChanges[0].sig.sharpsFlats == 0 &&
              up.keySigChanges[1].sig.sharpsFlats == 4,
              "中途转调点一起平移：1=Bb -> 1=C、1=D -> 1=E");
        check(keyName(estimateKey(up)) == "C",
              "移调后 estimateKey 读出新的 1=C，不再沿用文件里的 1=Bb");
        // 唱名不变才是「移对了」：原来的 1 2 3 移完仍读作 1 2 3。
        // 八度点是跟着绝对音高走的，跨过 C5 之后会多一个高音点，所以
        // 只比较音级数字（去掉 ' 与 ,）。
        auto stripOct = [](std::string s) {
            std::string r;
            for (char c : s) if (c != '\'' && c != ',') r += c;
            return r;
        };
        const KeyEstimate k0 = estimateKey(d);
        const KeyEstimate k1 = estimateKey(up);
        check(stripOct(pitchToJianpu(d.notes[0].pitch, k0)) ==
                  stripOct(pitchToJianpu(up.notes[0].pitch, k1)) &&
              stripOct(pitchToJianpu(d.notes[2].pitch, k0)) ==
                  stripOct(pitchToJianpu(up.notes[2].pitch, k1)),
              "移调后同一段旋律的简谱唱名保持不变");

        MidiData oct = d;
        transposeData(oct, 12);
        check(oct.keySig.sharpsFlats == -2 &&
              oct.keySigChanges[1].sig.sharpsFlats == 2 &&
              oct.notes[0].pitch == 82,
              "移八度：音高走、调号不动");

        MidiData same = d;
        transposeData(same, 0);
        check(same.keySig.sharpsFlats == -2 && same.notes[0].pitch == 70 &&
              same.keySigChanges[1].sig.sharpsFlats == 2,
              "移 0 半音是纯空操作");

        // v1.76.0 六个降号（Gb 大调）不再被写成六个升号（F# 大调）
        //
        // 同样的音级 6 有两种等音写法：F#(+6) 与 Gb(-6)，升降号一样多。
        // 音级查表只能挑一个，于是文件里明写 -6 的曲子被印成 1=F#，
        // MusicXML 也写 <fifths>6</fifths>——落到 MuseScore 里整首变成升号。
        {
            // a) 没有权威来源时仍按查表（--key 未给、自动检测、五声宫音）
            check(keyFifths(KeyEstimate{6, true}) == 6,
                  "无声明时音级 6 仍查表给 +6（既有行为不变）");

            MidiData gb;
            gb.division = 480;
            gb.timeSig.valid = true;
            gb.trackNames[0] = "Piano";
            gb.keySig.valid = true;
            gb.keySig.sharpsFlats = -6;
            NoteEvent g;
            g.track = 0; g.channel = 0; g.pitch = 66; g.durationTicks = 480;
            for (int i = 0; i < 8; i++) {
                g.startTicks = i * 480L;
                gb.notes.push_back(g);
            }
            const KeyEstimate gbk = estimateKey(gb);
            check(gbk.tonicPitchClass == 6 && gbk.isMajor,
                  "sf=-6 识别成音级 6 的大调（Gb / F# 等音）");
            check(keyFifths(gbk) == -6, "六降号留在降号侧（-6），实际 "
                  + std::to_string(keyFifths(gbk)));
            check(prefersFlats(gbk) && keyName(gbk) == "Gb",
                  "页眉音名是 Gb 而不是 F#，实际 " + keyName(gbk));
            check(renderMusicXml(gb, gbk).find("<fifths>-6</fifths>")
                      != std::string::npos,
                  "MusicXML 写 <fifths>-6</fifths>");
            check(renderHtml(gb, gbk, "t").find("1=Gb") != std::string::npos,
                  "HTML 页眉写 1=Gb");

            // b) 移调时调号侧跟着走：+2 落到 Ab(-4)，整八度回到 -6
            const KeyEstimate ab = transposeKey(gbk, 2);
            check(keyFifths(ab) == -4 && keyName(ab) == "Ab",
                  "Gb 升两个半音是 Ab（-4），实际 " + keyName(ab) + " / "
                      + std::to_string(keyFifths(ab)));
            check(keyFifths(transposeKey(gbk, 12)) == -6,
                  "Gb 移一个八度仍是 -6（不翻到升号侧）");

            // c) --key Gb 这类「用户写明降号」的入口
            check(keyFifthsOfFlatSpelling(6) == -6 &&
                  keyFifthsOfFlatSpelling(1) == -5 &&
                  keyFifthsOfFlatSpelling(10) == -2 &&
                  keyFifthsOfFlatSpelling(3) == -3,
                  "降号写法：Gb/Db/Bb/Eb 分别是 -6/-5/-2/-3");

            // d) 中途转调进 Gb 的那一段也得保留降号侧
            MidiData mod = gb;
            mod.keySig.sharpsFlats = 0;
            mod.keySigChanges.clear();
            mod.keySigChanges.push_back({0, KeySignature{0, false, true}});
            mod.keySigChanges.push_back({1920, KeySignature{-6, false, true}});
            const BarGrid mg = barGridOf(mod);
            const KeyMap km(mod, estimateKey(mod));
            check(keyFifths(km.keyAtBar(0, mg)) == 0,
                  "转调前是 C 大调（0）");
            check(keyFifths(km.keyAtBar(1, mg)) == -6,
                  "转调点的六降号也留在降号侧，实际 "
                      + std::to_string(keyFifths(km.keyAtBar(1, mg))));
            check(km.summary(mg).find("1=Gb") != std::string::npos,
                  "页眉摘要里的转调标记写 1=Gb，实际 " + km.summary(mg));
            // e) 五声宫音标注里的升降号：--gong Gb 不能在一处写 Gb、一处写 F#
            const KeyEstimate gbGong = keyFromGong(gb, 6, -6);
            check(gbGong.scaleLabel.find("Gb") != std::string::npos &&
                  gbGong.scaleLabel.find("F#") == std::string::npos,
                  "五声标注里的宫音写 Gb，实际 " + gbGong.scaleLabel);
            check(keyFifths(gbGong) == -6, "五声宫音的调号也留在降号侧");

        // v1.77.0 文本谱小节号（--bar-numbers）：与 HTML 的 .barno 同口径
        {
            MidiData nb;
            nb.division = 480;
            nb.timeSig.valid = true;
            nb.trackNames[0] = "Vocal";
            NoteEvent q;
            q.track = 0; q.channel = 0; q.pitch = 60; q.durationTicks = 480;
            for (int i = 0; i < 12; i++) {
                q.startTicks = i * 480L;
                nb.notes.push_back(q);
            }
            const KeyEstimate nk = estimateKey(nb);
            const std::string plain = formatJianpu(nb, nk);
            const std::string num = formatJianpu(nb, nk, true);
            check(plain.find("[1] ") == std::string::npos,
                  "默认不标小节号（文本谱零回归）");
            // 第 1 小节省号（与 HTML 的 .barno 一致：开头不必自证是第 1 小节），
            // 从第 2 小节起逐行标号，第 3 行就是 [3]。
            check(num.find("[2] ") != std::string::npos &&
                  num.find("[3] ") != std::string::npos &&
                  num.find("[1] ") == std::string::npos,
                  "从第 2 小节起逐行标号，第 1 小节省号");
            // 只加前缀，小节线与行边界一个都不能动
            check(occurrences(num, " |\n") == occurrences(plain, " |\n") &&
                  occurrences(num, "\n") == occurrences(plain, "\n"),
                  "小节号只是行首前缀，行数与小节线数不变");

            // 弱起小节不编号：第一行留空，第一个完整小节才是 1
            MidiData pk = nb;
            pk.pickupTicks = 480;
            const std::string pkNum = formatJianpu(pk, estimateKey(pk), true);
            check(pkNum.find("[1] ") != std::string::npos &&
                  pkNum.find("[0] ") == std::string::npos,
                  "弱起小节不编号，第一个完整小节标 [1]");
        }
        }

        // v1.78.0 打印配色：白纸上 #ddd / #bbb 等于隐形
        {
            MidiData pc;
            pc.division = 480;
            pc.timeSig.valid = true;
            NoteEvent pn;
            pn.track = 0; pn.channel = 0; pn.pitch = 60; pn.durationTicks = 480;
            for (int i = 0; i < 8; i++) {
                pn.startTicks = i * 480L;
                pc.notes.push_back(pn);
            }
            const std::string ph = renderHtml(pc, estimateKey(pc), "t");
            check(ph.find(".barno{color:#777}") != std::string::npos &&
                  ph.find(".barno{color:#ddd}") == std::string::npos,
                  "打印时小节号用深灰 #777（原 #ddd 在白纸上读不出）");
            check(ph.find(".footer{color:#888}") != std::string::npos,
                  "打印时页脚压到 #888（原 #bbb 同样读不出）");
            // 屏幕档位不动：只改 @media print，别的样式一个字节不该变
            check(ph.find(".barno{font-size:10px;color:#c9c9c9") != std::string::npos,
                  "屏幕上的小节号仍是最初的浅灰");
        }

        // 没有 0x59 的文件（靠自动猜调）本来就跟音符走，这里锁住别退回旧路。
        MidiData nokey;
        nokey.division = 480;
        nokey.timeSig.valid = true;
        nokey.trackNames[0] = "Vocal";
        const int cScale[8] = {0, 2, 4, 5, 7, 9, 11, 12};
        for (int i = 0; i < 8; i++) {
            NoteEvent m;
            m.startSeconds = i * 0.5;
            m.durationSeconds = 0.5;
            m.pitch = 60 + cScale[i];
            m.startTicks = i * 480;
            m.durationTicks = 480;
            m.rawDurationTicks = 480;
            m.channel = 0;
            m.track = 0;
            nokey.notes.push_back(m);
        }
        MidiData nk2 = nokey;
        transposeData(nk2, 2);
        check(!nk2.keySig.valid && nk2.notes[0].pitch == 62,
              "无调号文件只移音，不凭空造出调号");
        check(keyName(estimateKey(nokey)) == "C" && keyName(estimateKey(nk2)) == "D",
              "无调号文件的调性由音级分布决定，移调后自动猜调跟着走");
    }

    // -----------------------------------------------------------------
    // v1.52.0: 文本谱把同拍和弦并成一格
    // -----------------------------------------------------------------
    std::cout << "\n测试文本谱和弦（同拍多音）\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        const KeyEstimate c{0, true};

        // 4/4 一小节：C3-E3-G3 三个全音符 + 下一小节四个四分音符。
        // 旧实现一个音印一格，第一小节被写成 `1, - - - 3, - - - 5, - - -`，
        // 4/4 的一小节读出 12 拍。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Piano";
        add(d, 0, 1920, 48);
        add(d, 0, 1920, 52);
        add(d, 0, 1920, 55);
        add(d, 1920, 480, 60);
        add(d, 2400, 480, 62);
        add(d, 2880, 480, 64);
        add(d, 3360, 480, 65);
        const std::string jp = formatJianpu(d, c);
        check(jp.find("1,+3,+5,") != std::string::npos,
              "同拍三音并成一格 `1,+3,+5,`：" + jp);
        check(jp.find("1,+3,+5, - - -") != std::string::npos,
              "和弦的时值只写一次（4 拍 = 数字 + 三条延音线）：" + jp);
        check(jp.find("3, - - -") == std::string::npos,
              "和弦音不再各占一格、把小节撑成 12 拍：" + jp);

        // 不同音头（先后发音）的音不能被当成和弦串起来。
        MidiData seq;
        seq.division = 480;
        seq.timeSig.valid = true;
        add(seq, 0, 480, 60);
        add(seq, 480, 480, 64);
        add(seq, 960, 480, 67);
        add(seq, 1440, 480, 72);
        const std::string js = formatJianpu(seq, c);
        check(js.find("+") == std::string::npos,
              "先后发音的音不写成和弦：" + js);

        // 每个和弦音各带自己的八度记号。
        MidiData oct;
        oct.division = 480;
        oct.timeSig.valid = true;
        add(oct, 0, 1920, 48);   // C3：低八度
        add(oct, 0, 1920, 60);   // C4
        add(oct, 0, 1920, 72);   // C5：高八度
        add(oct, 1920, 480, 60);
        add(oct, 2400, 480, 62);
        add(oct, 2880, 480, 64);
        add(oct, 3360, 480, 65);
        const std::string jo = formatJianpu(oct, c);
        check(jo.find("1,+1+1'") != std::string::npos,
              "和弦各音保留自己的八度记号：" + jo);
    }

    // -----------------------------------------------------------------
    // v1.53.0: 时值折算不许把小节撑过拍号；倚音不许吞掉前面的休止
    // -----------------------------------------------------------------
    std::cout << "\n测试时值折算与小节拍数守恒\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        const KeyEstimate c{0, true};

        // 3.75 拍的休止：durationSuffix() 取最近档，3.75 落在 3 与 4 之间时
        // 向上取整成 4 拍，那一小节读成 4.25 拍，后面每小节的拍位整体错位。
        MidiData rest;
        rest.division = 480;
        rest.timeSig.valid = true;
        rest.trackNames[0] = "Vocal";
        for (int i = 0; i < 4; i++) add(rest, i * 480, 480, 60 + 2 * i);
        add(rest, 1920, 120, 60);          // 第 2 小节开头一个 16 分音符
        add(rest, 3840, 1920, 60);         // 第 3 小节（收在小节线，免得被判成弱起）
        const std::string jr = formatJianpu(rest, c);
        check(jr.find("0 - - -_.") != std::string::npos,
              "3.75 拍休止拆成 `0 - -` + `-_.`，不再向上取整成 4 拍：" + jr);
        check(jr.find("0 - - -  |") == std::string::npos,
              "休止不会把一小节撑过小节线：" + jr);

        // 音符一侧同理：3.75 拍的音正好顶到小节线，也不能印成 4 拍。
        MidiData note;
        note.division = 480;
        note.timeSig.valid = true;
        note.trackNames[0] = "Vocal";
        for (int i = 0; i < 4; i++) add(note, i * 480, 480, 60 + 2 * i);
        add(note, 1920, 120, 60);
        add(note, 2040, 1800, 67);         // 3.75 拍，尾部顶到第 2 小节线
        add(note, 3840, 1920, 72);
        const std::string jn = formatJianpu(note, c);
        check(jn.find("5 - - -_.") != std::string::npos,
              "3.75 拍的音同样拆段记谱，不越过小节线：" + jn);

        // 反向约束：还没顶到小节线的「零头」，照旧记最近档，不许碎片化。
        MidiData near;
        near.division = 480;
        near.timeSig.valid = true;
        near.trackNames[0] = "Vocal";
        add(near, 460, 480, 60);           // 开头 0.958 拍休止 -> 记 1 拍
        add(near, 940, 480, 62);
        add(near, 1420, 500, 64);
        const std::string jnr = formatJianpu(near, c);
        check(jnr.find("0_. -%%") == std::string::npos,
              "0.958 拍休止仍记成一拍 `0`，不拆成 `0_. -%%`：" + jnr);
        check(jnr.find("0 1") != std::string::npos,
              "小节内的最近档记法保持不变：" + jnr);

        // 倚音不占格，但它前面的休止必须照常印出来（旧实现先跳倚音再算
        // gap，那一段空白既没有音头也没有休止，直接消失）。
        MidiData gr;
        gr.division = 480;
        gr.timeSig.valid = true;
        gr.trackNames[0] = "Vocal";
        add(gr, 0, 480, 72);
        add(gr, 3840, 480, 72);            // 第 3 小节头
        add(gr, 4770, 14, 74);             // 倚音 D5（极短，紧跟 G5）
        add(gr, 4800, 960, 79);            // 主音 G5
        const std::string jg = formatJianpu(gr, c);
        check(jg.find("0 (2')5'") != std::string::npos,
              "倚音前面的 1 拍休止照常印出来：" + jg);
        check(jg.find("(2')5' -") != std::string::npos,
              "倚音仍挂在主音前面，主音时值不受影响：" + jg);
    }

    // -----------------------------------------------------------------
    // v1.54.0: 门限衰减留下的空档并进前一个音
    // -----------------------------------------------------------------
    std::cout << "\n测试门限空档并进前一个音\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        const KeyEstimate c{0, true};

        check(restGateTicks(480) == 240 && restGateTicks(96) == 48 &&
                  restGateTicks(0) == 1,
              "休止门槛：不足半拍（八分休止）的空档不记休止符");

        // DAW 门限 85% 的典型形状：四分音符只响 400 tick、间隔 480。
        // 旧版记成「附点八分 + 看不见的十六分休止」，4/4 一小节只剩 3 拍，
        // MusicXML 却补出一串 16 分休止——三端对不上。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Vocal";
        const int scale[4] = {60, 62, 64, 65};
        for (int i = 0; i < 4; i++) add(d, i * 480L, 400, scale[i]);
        add(d, 1920, 480, 67);       // 第 2 小节：真四分音符
        add(d, 2880, 400, 69);       // 门限音
        add(d, 3360, 480, 71);       // 末音收在小节线（免得被判成弱起）
        const QuantizeReport rep = quantizeTiming(d);
        check(rep.changed(), "门限空档触发了时值修正");
        check(d.notes[0].durationTicks == 480 && d.notes[3].durationTicks == 480,
              "400 tick 的门限音补成 480（四分音符），实际: " +
                  std::to_string(d.notes[0].durationTicks));

        const std::string jp = formatJianpu(d, c);
        check(jp.find("1 2 3 4  |") != std::string::npos,
              "第 1 小节读成四个四分音符（旧版只有 3 拍）：" + jp);
        check(jp.find("5 0 6 7  |") != std::string::npos,
              "真正的四分休止照常保留，没有被一起并掉：" + jp);

        // 三端一致：MusicXML 走同一份时值，不许再补出假的 16 分休止。
        const std::string xml = renderMusicXml(d, c);
        check(xml.find("<type>16th</type>") == std::string::npos,
              "MusicXML 里不再冒出一串 16 分休止：" + xml.substr(0, 60));
        check(xml.find("<note><rest/><duration>480</duration>") != std::string::npos,
              "MusicXML 保留真正的四分休止");

        // 反向约束一：音长一半就收尾的是刻意的断奏 / 打击乐击打，
        // 不能因为「空档不足半拍」就把 16 分击打拉成八分。
        MidiData hit;
        hit.division = 480;
        hit.timeSig.valid = true;
        hit.trackNames[0] = "Drums";
        for (int i = 0; i < 4; i++) add(hit, i * 240L, 120, 60 + 2 * i);
        add(hit, 1920, 1920, 60);
        quantizeTiming(hit);
        check(hit.notes[0].durationTicks == 120,
              "120 tick 的十六分击打保持 120，不被并成八分，实际: " +
                  std::to_string(hit.notes[0].durationTicks));

        // 反向约束二：复调轨里不能拿别的声部的音头截断按住的长音。
        // v1.55.0 起右手按本声部补空档（400 -> 480），左手全音符仍是 1920。
        MidiData poly;
        poly.division = 480;
        poly.timeSig.valid = true;
        poly.trackNames[0] = "Piano";
        add(poly, 0, 1920, 48);      // 左手全音符
        add(poly, 480, 400, 60);     // 右手门限音（空档 80 tick）
        add(poly, 960, 480, 62);
        add(poly, 1440, 480, 64);
        add(poly, 1920, 1920, 48);
        quantizeTiming(poly);
        check(poly.notes[0].durationTicks == 1920 && poly.notes[4].durationTicks == 1920,
              "复调轨里左手全音符不被右手音头截断，实际: " +
                  std::to_string(poly.notes[0].durationTicks));
        check(poly.notes[1].durationTicks == 480,
              "复调轨右手按本声部补空档（400 -> 480），实际: " +
                  std::to_string(poly.notes[1].durationTicks));
    }

    // -----------------------------------------------------------------
    // v1.55.0: 复调轨按声部补空档，三端拍数不再分叉
    // -----------------------------------------------------------------
    std::cout << "\n测试复调轨按声部补空档\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        // quantizeTiming() 末尾会按 (start, pitch) 重排 notes，按位置取索引不可靠。
        auto durOf = [](const MidiData& d, long start, int pitch) -> long {
            for (const auto& n : d.notes)
                if (n.startTicks == start && n.pitch == pitch) return n.durationTicks;
            return -1;
        };
        const KeyEstimate c{0, true};

        // 钢琴双手：左手每小节按住一个全音符（整轨 monophonic=false），
        // 右手是 DAW 门限 85% 的四分音符（只响 400 tick / 间隔 480）。
        // v1.54.0 的空档合并被整轨开关一刀切掉，右手一小节只剩 3 拍，
        // MusicXML 却补出 7 个 16 分休止。正确做法是按声部各自算下一音头。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Piano";
        for (int bar = 0; bar < 2; bar++) add(d, bar * 1920L, 1920, 48);
        const int scale[7] = {60, 62, 64, 65, 67, 69, 71};
        for (int i = 0; i < 7; i++) add(d, i * 480L, 400, scale[i]);
        add(d, 3360, 480, 72);       // 末音收在小节线（免得被判成弱起）
        const QuantizeReport rep = quantizeTiming(d);
        check(rep.changed(), "复调轨的门限空档触发了时值修正");
        check(durOf(d, 480, 62) == 480 && durOf(d, 2880, 71) == 480,
              "右手 400 tick 的门限音补成 480，实际: " +
                  std::to_string(durOf(d, 480, 62)));
        check(durOf(d, 0, 48) == 1920 && durOf(d, 1920, 48) == 1920,
              "左手全音符保持 1920，不被右手音头截断");

        const std::string jp = formatJianpu(d, c);
        check(jp.find("1 2 3 4  |") != std::string::npos,
              "右手第 1 小节读成四个四分音符（旧版只有 3 拍）：" + jp);
        check(jp.find("5 6 7 1'  |") != std::string::npos,
              "右手第 2 小节同样读成 4 拍：" + jp);
        check(jp.find("1, - - -  |") != std::string::npos,
              "左手全音符仍单独成声部，排版不变：" + jp);

        const std::string xml = renderMusicXml(d, c);
        check(xml.find("<type>16th</type>") == std::string::npos,
              "MusicXML 里不再补出一串假的 16 分休止");
        check(xml.find("<rest/>") == std::string::npos,
              "MusicXML 里不再补出任何假休止");
        const std::string html = renderHtml(d, c, "t");
        size_t quarters = 0, pos = 0;
        while ((pos = html.find("class='note quarter'", pos)) != std::string::npos)
            { quarters++; pos++; }
        check(quarters == 8, "HTML 是 8 个四分音符列，实际: " + std::to_string(quarters));
        check(html.find("class='note whole'") != std::string::npos,
              "HTML 保留左手的全音符列");

        // 反向约束一：本声部的下一个音头不能错拿成别的声部的音头。
        // 左手在 240 tick 处有个八分音头，右手下一音头在 480；若按整轨
        // onsets 取 240，右手的 400 tick 音就补不成整拍。
        MidiData lanes;
        lanes.division = 480;
        lanes.timeSig.valid = true;
        lanes.trackNames[0] = "Piano";
        add(lanes, 0, 200, 48);     // 左手八分
        add(lanes, 240, 200, 50);
        add(lanes, 480, 200, 52);
        add(lanes, 0, 400, 60);     // 右手门限音
        add(lanes, 480, 480, 62);
        quantizeTiming(lanes);
        check(durOf(lanes, 0, 60) == 480,
              "右手补到本声部 480 的音头，不是左手的 240，实际: " +
                  std::to_string(durOf(lanes, 0, 60)));
        check(durOf(lanes, 240, 50) == 240,
              "左手八分音只补到自己声部的 240 音头，实际: " +
                  std::to_string(durOf(lanes, 240, 50)));

        // 反向约束二：复调轨里真正的四分休止仍要保留。
        MidiData rest;
        rest.division = 480;
        rest.timeSig.valid = true;
        rest.trackNames[0] = "Piano";
        add(rest, 0, 1920, 48);
        add(rest, 0, 400, 60);      // 空档 480 tick（到 960），不是门限衰减
        add(rest, 960, 400, 62);
        add(rest, 1440, 480, 64);  // 末音收在小节线
        quantizeTiming(rest);
        check(durOf(rest, 0, 60) != 960 && durOf(rest, 0, 60) < 480,
              "复调轨里真四分休止没被并进门限音，实际: " +
                  std::to_string(durOf(rest, 0, 60)));
        const std::string rjp = formatJianpu(rest, c);
        check(rjp.find("1 2 3 4  |") == std::string::npos,
              "真休止那一小节没有被读成四个四分音符：" + rjp);

        // 反向约束三：复调轨里的 16 分击打仍不被拉成八分。
        MidiData hit;
        hit.division = 480;
        hit.timeSig.valid = true;
        hit.trackNames[0] = "Drums";
        add(hit, 0, 1920, 48);
        for (int i = 0; i < 4; i++) add(hit, i * 240L, 120, 60 + 2 * i);
        add(hit, 1920, 1920, 48);
        quantizeTiming(hit);
        check(durOf(hit, 0, 60) == 120,
              "复调轨里 120 tick 的十六分击打保持 120，实际: " +
                  std::to_string(durOf(hit, 0, 60)));
    }

    // -----------------------------------------------------------------
    // v1.56.0: 复调轨的门限因子按声部估，连奏声部不再锁死全局 gate
    // -----------------------------------------------------------------
    std::cout << "\n测试复调轨门限因子按声部估\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        // quantizeTiming() 末尾按 (start, pitch) 重排 notes，别按索引取。
        auto durOf = [](const MidiData& d, long start, int pitch) -> long {
            for (const auto& n : d.notes)
                if (n.startTicks == start && n.pitch == pitch) return n.durationTicks;
            return -1;
        };
        const KeyEstimate c{0, true};

        // 钢琴双手：左手 24 个八分连奏音（dur == ioi == 240），右手是门限
        // 70% 的四分音符（336 tick / 480 间隔）。整轨音头每 240 tick 一个，
        // 第一遍里右手 336/240 = 1.4 被当无效样本丢掉、左手 240/240 = 1.0
        // 灌满样本，全局 gate 落到 1.0，右手 12 个四分音符全被写成附点八分。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Piano";
        for (int i = 0; i < 24; i++) add(d, i * 240L, 240, 48 + i % 3);
        const int scale[12] = {60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65};
        for (int i = 0; i < 12; i++)
            add(d, i * 480L, (i < 11) ? 336 : 480, scale[i]);

        const QuantizeReport rep = quantizeTiming(d);
        check(rep.gate == 1.0,
              "全局 gate 仍是旧口径的 1.0（其余音符逐字节不变），实际: " +
                  std::to_string(rep.gate));
        check(rep.laneGate > 0.68 && rep.laneGate < 0.72,
              "本声部口径另估出门限 0.7，实际: " + std::to_string(rep.laneGate));
        check(durOf(d, 480, 62) == 480 && durOf(d, 2880, 71) == 480,
              "右手 336 tick 的门限音还原成 480，实际: " +
                  std::to_string(durOf(d, 480, 62)));
        check(durOf(d, 5280, 65) == 480,
              "末音没有 ioi 参照，不被虚增成 720，实际: " +
                  std::to_string(durOf(d, 5280, 65)));
        check(durOf(d, 0, 48) == 240 && durOf(d, 240, 49) == 240,
              "左手连奏八分音不被门限还原带偏，实际: " +
                  std::to_string(durOf(d, 0, 48)));

        const std::string jp = formatJianpu(d, c);
        check(jp.find("1 2 3 4  |") != std::string::npos,
              "右手第 1 小节读成四个四分音符（旧版是附点八分）：" + jp);
        check(jp.find("5 6 7 1'  |") != std::string::npos,
              "右手第 2 小节同样读成 4 拍");
        check(jp.find("7 6 5 4  |") != std::string::npos,
              "右手第 3 小节同样读成 4 拍");
        check(jp.find("1_. 2_. 3_. 4_.") == std::string::npos,
              "不再写成「附点八分」串");
        check(jp.find("1,_ #1,_ 2,_") != std::string::npos,
              "左手 24 个八分连奏音仍单独成声部、时值一字不改");
        check(jp.find("-_  |") == std::string::npos &&
                  jp.find("-=  |") == std::string::npos,
              "末音虚增会让谱面凭空多一小节，不许出现");

        const std::string xml = renderMusicXml(d, c);
        check(xml.find("<type>16th</type>") == std::string::npos &&
                  xml.find("<rest/>") == std::string::npos,
              "MusicXML 里不再补出 12 个假的 16 分休止");
        const std::string html = renderHtml(d, c, "t");
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) { n++; pos++; }
            return n;
        };
        check(countOf(html, "class='note quarter'") == 12,
              "HTML 是 12 个四分列，实际: " +
                  std::to_string(countOf(html, "class='note quarter'")));
        check(countOf(html, "class='note eighth'") == 24,
              "HTML 是 24 个八分列，实际: " +
                  std::to_string(countOf(html, "class='note eighth'")));

        // 反向约束一：单声部轨的 gate 完全走旧口径，本声部支路不启用。
        MidiData mono;
        mono.division = 480;
        mono.timeSig.valid = true;
        mono.trackNames[0] = "Vocal";
        for (int i = 0; i < 12; i++) add(mono, i * 480L, 403, 60 + i % 7);
        add(mono, 5760, 480, 67);
        const QuantizeReport mr = quantizeTiming(mono);
        check(mr.gate > 0.82 && mr.gate < 0.86,
              "单声部轨仍估出 0.84 的全局门限，实际: " +
                  std::to_string(mr.gate));
        check(mr.laneGate == 1.0,
              "没有声部音头交错时不启用本声部支路");
        check(durOf(mono, 480, 61) == 480,
              "单声部轨的门限还原结果不变，实际: " +
                  std::to_string(durOf(mono, 480, 61)));

        // 反向约束二：只有被整轨音头挡住的音才走支路；若本声部口径估不出
        // 显著更短的门限（样本不足 8 个），laneGate 必须退回 1.0。
        MidiData few;
        few.division = 480;
        few.timeSig.valid = true;
        few.trackNames[0] = "Piano";
        for (int i = 0; i < 6; i++) add(few, i * 480L, 336, 60 + i);
        for (int i = 0; i < 12; i++) add(few, i * 240L + 120, 240, 48);
        const QuantizeReport fr = quantizeTiming(few);
        check(fr.laneGate == 1.0,
              "本声部样本不足时不臆造门限，实际: " +
                  std::to_string(fr.laneGate));
    }

    // -----------------------------------------------------------------
    // v1.58.0: MusicXML 休止符时值拆成标准时值之和
    //   <duration> 精确但 <type> 是 durTypeDot() 猜的近似值时，MuseScore
    //   按 type 画形状、按 duration 推进游标，谱面会把休止符画错。
    // -----------------------------------------------------------------
    std::cout << "\n测试 MusicXML 休止符时值一致性\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        const KeyEstimate c{0, true};

        // 逐行取出所有 <note><rest/>，核对 duration 与 type/dot 是否严格一致。
        auto restRows = [](const std::string& x) {
            std::vector<std::string> rows;
            size_t pos = 0;
            while ((pos = x.find("<note><rest/>", pos)) != std::string::npos) {
                const size_t end = x.find("</note>", pos);
                if (end == std::string::npos) break;
                rows.push_back(x.substr(pos, end + 7 - pos));
                pos = end + 7;
            }
            return rows;
        };
        auto fieldNum = [](const std::string& row, const std::string& tag) {
            const size_t p = row.find(tag);
            if (p == std::string::npos) return -1;
            const size_t b = p + tag.size();
            const size_t e = row.find('<', b);
            if (e == std::string::npos) return -1;
            return std::stoi(row.substr(b, e - b));
        };
        auto fieldStr = [](const std::string& row, const std::string& tag) {
            const size_t p = row.find(tag);
            if (p == std::string::npos) return std::string();
            const size_t b = p + tag.size();
            const size_t e = row.find('<', b);
            return e == std::string::npos ? std::string() : row.substr(b, e - b);
        };
        auto badRests = [&](const std::string& x, int div) {
            static const struct { const char* name; double beats; } kTypes[] = {
                {"whole", 4.0}, {"half", 2.0}, {"quarter", 1.0},
                {"eighth", 0.5}, {"16th", 0.25}, {"32nd", 0.125}, {"64th", 0.0625},
            };
            int bad = 0;
            for (const auto& row : restRows(x)) {
                const int dur = fieldNum(row, "<duration>");
                if (dur < 0) continue;
                const std::string tp = fieldStr(row, "<type>");
                double want = 1.0;
                bool known = false;
                for (const auto& k : kTypes) {
                    if (tp == k.name) { want = k.beats; known = true; break; }
                }
                if (!known) { bad++; continue; }
                if (row.find("<dot/>") != std::string::npos) want *= 1.5;
                if (std::abs(dur / (double)div - want) > 1e-6) bad++;
            }
            return bad;
        };
        auto measureSums = [](const std::string& x) {
            std::vector<long> sums;
            size_t pos = 0;
            while ((pos = x.find("<measure", pos)) != std::string::npos) {
                const size_t open = x.find('>', pos);
                const size_t end = x.find("</measure>", open);
                if (open == std::string::npos || end == std::string::npos) break;
                const std::string body = x.substr(open + 1, end - open - 1);
                long sum = 0;
                size_t p = 0;
                while ((p = body.find("<note>", p)) != std::string::npos) {
                    const size_t ne = body.find("</note>", p);
                    if (ne == std::string::npos) break;
                    const std::string row = body.substr(p, ne - p);
                    const size_t dp = row.find("<duration>");
                    if (dp != std::string::npos) {
                        const size_t b = dp + 10, e = row.find('<', b);
                        if (e != std::string::npos)
                            sum += std::stol(row.substr(b, e - b));
                    }
                    p = ne + 7;
                }
                sums.push_back(sum);
                pos = end + 10;
            }
            return sums;
        };

        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Vocal";
        add(d, 0, 120, 60);        // 小节 1 末尾剩 1800 = 3.75 拍
        add(d, 1920, 480, 62);     // 小节 2 末尾剩 1440 = 3 拍（精确对照）
        add(d, 3840, 120, 64);     // 小节 3：间隙 840 = 1.75 拍
        add(d, 4800, 120, 65);     //            间隙 600 = 1.25 拍
        add(d, 5520, 240, 67);
        const std::string xml = renderMusicXml(d, c);

        check(badRests(xml, 480) == 0,
              "全部休止符 duration 与 type/dot 严格一致，实际不一致: " +
                  std::to_string(badRests(xml, 480)));
        check(xml.find("<duration>840</duration>") == std::string::npos &&
                  xml.find("<duration>600</duration>") == std::string::npos &&
                  xml.find("<duration>1800</duration>") == std::string::npos,
              "三段不规则间隙不再写成单个近似休止符");
        check(xml.find("<note><rest/><duration>720</duration><voice>1</voice>"
                       "<type>quarter</type><dot/></note>") != std::string::npos,
              "1.75 拍拆出附点四分休止（720）");
        check(xml.find("<note><rest/><duration>480</duration><voice>1</voice>"
                       "<type>quarter</type></note>") != std::string::npos,
              "1.25 拍拆出四分休止（480）");
        check(xml.find("<note><rest/><duration>360</duration><voice>1</voice>"
                       "<type>eighth</type><dot/></note>") != std::string::npos,
              "3.75 拍拆出附点八分休止（360）");
        check(xml.find("<note><rest/><duration>120</duration><voice>1</voice>"
                       "<type>16th</type></note>") != std::string::npos,
              "余下的十六分休止也单独成符");
        const std::vector<long> sums = measureSums(xml);
        check(sums.size() == 3 && sums[0] == 1920 && sums[1] == 1920 &&
                  sums[2] == 1920,
              "三个小节的总时长都守恒为 1920 tick");
        check(restRows(xml).size() == 7,
              "拆出 7 个休止符（3 段拆 6 个 + 1 个精确对照），实际: " +
                  std::to_string(restRows(xml).size()));

        // 精确档不许被拆散：3 拍的附点二分休止仍是单个 note。
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) { n++; pos += sub.size(); }
            return n;
        };
        check(countOf(xml, "<note><rest/><duration>1440</duration><voice>1</voice>"
                           "<type>half</type><dot/></note>") == 2,
              "3 拍精确档逐字节保持（3.75 拍拆出的那段 + 第 3 小节对照）");

        // 音符路径完全不动：仍是八分 / 十六分 / 四分。
        check(xml.find("<type>eighth</type>") != std::string::npos &&
                  xml.find("<type>16th</type>") != std::string::npos &&
                  xml.find("<type>quarter</type>") != std::string::npos,
              "音符时值符号不受拆分影响");

        // 反向闸门：不足一个 64 分音符的极不规则间隙无法精确拆，
        // 必须保留旧的单一休止符兜底，不许擅自改动时间推进。
        MidiData odd;
        odd.division = 480;
        odd.timeSig.valid = true;
        odd.trackNames[0] = "Vocal";
        add(odd, 0, 470, 60);
        add(odd, 480, 480, 62);
        const std::string oddXml = renderMusicXml(odd, c);
        check(oddXml.find("<duration>10</duration>") != std::string::npos,
              "10 tick 的不规则间隙保留旧兜底、时长不丢失");
        check(countOf(oddXml, "<duration>10</duration>") == 1,
              "不规则间隙只写一个休止符，不拆成多个");
        check(countOf(oddXml, "<note><rest/>") == 2,
              "除 10 tick 兜底外，小节末仍照常补一个半休止");

        // 反向闸门：全小节休止 <rest measure="yes"/> 是标准写法，不拆。
        MidiData empty;
        empty.division = 480;
        empty.timeSig.valid = true;
        empty.trackNames[0] = "Vocal";
        add(empty, 0, 480, 60);
        add(empty, 3840, 480, 62);   // 第 2 小节完全空
        const std::string emptyXml = renderMusicXml(empty, c);
        check(emptyXml.find("<rest measure=\"yes\"/>") != std::string::npos,
              "整小节空档仍写成全小节休止");
    }

    // v1.59.0: 小节中间的休止符必须精确拆段，谱面拍数与拍号守恒
    // -----------------------------------------------------------------
    std::cout << "\n测试休止符精确拆段与拍数守恒\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const KeyEstimate c{0, true};

        // 4/4 两小节：840(1.75 拍) 与 600(1.25 拍) 两段间隙夹在小节中间，
        // 后面紧跟音头。旧版按最近档记成 0.(1.5 拍) / 0(1 拍)，只读出 3.5 拍。
        MidiData mid;
        mid.division = 480;
        mid.timeSig.valid = true;
        mid.trackNames[0] = "Vocal";
        add(mid, 0, 120, 60);
        add(mid, 960, 120, 62);
        add(mid, 1680, 240, 64);
        add(mid, 1920, 480, 65);
        add(mid, 2400, 480, 67);
        add(mid, 2880, 480, 69);
        add(mid, 3360, 480, 71);
        const std::string jp = formatJianpu(mid, c);
        check(jp.find("1= 0. -= 2= 0 -= 3_") != std::string::npos,
              "1.75 / 1.25 拍休止精确拆成 0. -= / 0 -=：" + jp);
        check(jp.find("1= 0. 2= 0 3_") == std::string::npos,
              "旧的按最近档近似写法不再出现：" + jp);
        check(countOf(jp, "-=") == 2,
              "两段各补一个十六分续写，实际: " +
                  std::to_string(countOf(jp, "-=")));
        check(jp.find("4 5 6 7") != std::string::npos,
              "第 2 小节四个四分音符逐字节不变");
        check(jp.find("1=") != std::string::npos &&
                  jp.find("3_") != std::string::npos,
              "音符本身的时值记号不受休止拆段影响");

        // rest_split 的第 3 小节原先被误当作「末小节惯例」，其实同一缺陷：
        // 现在也必须拆段并守恒 4 拍。
        MidiData tail;
        tail.division = 480;
        tail.timeSig.valid = true;
        tail.trackNames[0] = "Vocal";
        add(tail, 3840, 120, 64);
        add(tail, 4800, 120, 65);
        add(tail, 5520, 240, 67);
        const std::string jt = formatJianpu(tail, c);
        check(jt.find("0. -= ") != std::string::npos &&
                  jt.find("0 -= ") != std::string::npos,
              "最后一小节的两段不规则休止也拆段：" + jt);
        check(jt.find("0. 4=") == std::string::npos,
              "末小节不再少 0.5 拍：" + jt);

        // 反向闸门：小节末 3.75 拍的旧拆段写法必须逐字节保持。
        MidiData end;
        end.division = 480;
        end.timeSig.valid = true;
        end.trackNames[0] = "Vocal";
        add(end, 0, 120, 60);
        add(end, 1920, 480, 62);
        const std::string je = formatJianpu(end, c);
        check(je.find("0 - - -_.") != std::string::npos,
              "小节末 3.75 拍仍写成 0 - - -_.，新路径不改变它：" + je);

        // 反向闸门：精确档休止仍是单个符号，不许被拆散。
        MidiData exact;
        exact.division = 480;
        exact.timeSig.valid = true;
        exact.trackNames[0] = "Vocal";
        add(exact, 0, 480, 60);
        add(exact, 960, 480, 62);      // 间隙恰好 1 拍
        const std::string jx = formatJianpu(exact, c);
        check(jx.find("1 0 2") != std::string::npos,
              "1 拍精确档休止仍是单个 0，不碎片化：" + jx);
        check(jx.find("0_. -%%") == std::string::npos,
              "精确档不许拆成 0_. -%%：" + jx);

        // 反向闸门：不足最小时值的零头无法精确拆，保留旧的最近档写法。
        // 460 tick = 0.958 拍，余量 100 tick 拆不到 60 的整数档。
        MidiData odd;
        odd.division = 480;
        odd.timeSig.valid = true;
        odd.trackNames[0] = "Vocal";
        add(odd, 460, 480, 60);
        add(odd, 940, 480, 62);
        add(odd, 1420, 500, 64);
        const std::string jo = formatJianpu(odd, c);
        check(jo.find("0_. -%%") == std::string::npos,
              "拆不干净的 0.958 拍仍按最近档记，不碎片化：" + jo);
        check(jo.find("0 1") != std::string::npos,
              "拆不干净的间隙仍记成一拍 0：" + jo);
    }


    // -----------------------------------------------------------------
    // v1.60.0: HTML 谱面必须把附点画出来。DurStyle::dotted 与 CSS 的
    // .dotted .num::after 一直在，noteColumn() 却没把 class 拼上去，
    // 0.75 拍画成八分、1.5 拍画成四分，视觉拍数凭空少 1/3。
    // -----------------------------------------------------------------
    std::cout << "\n测试 HTML 附点标记\n";
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const KeyEstimate c{0, true};

        // 4/4 一小节：0.75 / 0.375 / 0.375 / 1.5 / 1.0 拍，首尾相接不留空档，
        // 免得时值量化的补空档逻辑改掉这些精确档位。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Vocal";
        add(d, 0, 360, 60);
        add(d, 360, 180, 62);
        add(d, 540, 180, 64);
        add(d, 720, 720, 65);
        add(d, 1440, 480, 67);
        const std::string h = renderHtml(d, c, "t");
        check(countOf(h, "class='note eighth dotted'") == 1,
              "0.75 拍是附点八分列：" + h.substr(0, 200));
        check(countOf(h, "class='note sixteenth dotted'") == 2,
              "两个 0.375 拍都是附点十六分列");
        check(countOf(h, "class='note quarter dotted'") == 1,
              "1.5 拍是附点四分列");
        check(h.find("class='note quarter'") != std::string::npos,
              "1 拍精确档仍是不带点的四分列");
        check(h.find("class='note half dotted'") == std::string::npos,
              "二分列不该带附点");
        check(h.find("class='note whole dotted'") == std::string::npos,
              "全音符列不该带附点");
        const std::string jp = formatJianpu(d, c);
        check(jp.find("1_. 2=. 3=. 4. 5") != std::string::npos,
              "文本谱的附点记号不受 HTML 修复影响：" + jp);

        // 附点休止符：2400..2760 留 360 tick（0.75 拍）间隙，HTML 也要画点。
        MidiData r;
        r.division = 480;
        r.timeSig.valid = true;
        r.trackNames[0] = "Vocal";
        add(r, 1920, 480, 60);
        add(r, 2760, 720, 62);
        add(r, 3480, 240, 64);
        const std::string hr = renderHtml(r, c, "t");
        check(countOf(hr, "class='note eighth dotted'") == 1,
              "0.75 拍附点八分休止也画点");

        // 和弦列：三个音同时发 0.75 拍，附点只挂在最低音那个 tone 上。
        MidiData ch;
        ch.division = 480;
        ch.timeSig.valid = true;
        ch.trackNames[0] = "Vocal";
        add(ch, 0, 360, 60);
        add(ch, 0, 360, 64);
        add(ch, 0, 360, 67);
        add(ch, 360, 1560, 69);
        const std::string hc = renderHtml(ch, c, "t");
        check(countOf(hc, "class='tone dotted'") == 1,
              "和弦列只画一个附点（挂在最低音）：" + hc.substr(0, 200));
        check(hc.find("class='note chordcol eighth'") != std::string::npos,
              "和弦列本身仍是 eighth 档");
        check(hc.find("class='note chordcol eighth dotted'") == std::string::npos,
              "和弦整列不挂 dotted，否则每个音都会各画一个点");

        // 3 拍长音靠两条延音线表达（简谱 5 - -），不该再画附点。
        MidiData lg;
        lg.division = 480;
        lg.timeSig.valid = true;
        lg.trackNames[0] = "Vocal";
        add(lg, 0, 1440, 60);
        add(lg, 1440, 480, 62);
        const std::string hl = renderHtml(lg, c, "t");
        check(hl.find("class='note half dotted'") == std::string::npos,
              "3 拍长音不画附点（durStyleOf 的 3.0 档已收窄语义）");
        check(hl.find("class='note half'") != std::string::npos &&
                  hl.find("--n:2") != std::string::npos,
              "3 拍长音仍是二分列加两条延音线");
    }


    // ---- v1.61.0：HTML 休止符按精确档位之和拆列 ----
    {
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        const KeyEstimate c{0, true};

        // 4/4 两小节：1.75 拍与 1.25 拍间隙夹在小节中间，第二小节
        // 四个四分音符保证第一小节不是末小节。旧版画成 1.5 + 1 拍，
        // 两段共少 0.5 拍；修复后拆成 1.5+0.25 / 1+0.25 四个 0 列。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[0] = "Vocal";
        add(d, 0, 120, 60);
        add(d, 960, 120, 62);
        add(d, 1680, 240, 64);
        add(d, 1920, 480, 65);
        add(d, 2400, 480, 67);
        add(d, 2880, 480, 69);
        add(d, 3360, 480, 71);
        const std::string h = renderHtml(d, c, "t");
        check(countOf(h, "class='note quarter dotted'") == 1,
              "1.75 拍的第一段仍是附点四分列");
        check(countOf(h, "<span class='note sixteenth'><span class='oct'></span>"
                         "<span class='core'><span class='num'>0</span>") == 2,
              "1.75 / 1.25 拍各补一个十六分休止续列：" + h.substr(0, 200));
        check(countOf(h, "class='note quarter'") >= 5,
              "1.25 拍的第一段仍是四分列，第二小节四个四分音符不变");
        check(countOf(h, "<span class='num'>0</span>") == 4,
              "两段间隙共拆出 4 个 0 列（1.5+0.25 / 1+0.25）");
        const std::string jp = formatJianpu(d, c);
        check(jp.find("0. -=") != std::string::npos &&
                  jp.find("0 -=") != std::string::npos,
              "文本谱仍是精确拆段，HTML 与它逐段对齐：" + jp.substr(0, 200));

        // 精确档不许碎片化：0.75 拍休止仍是单列附点八分。
        MidiData e;
        e.division = 480;
        e.timeSig.valid = true;
        e.trackNames[0] = "Vocal";
        add(e, 0, 240, 60);
        add(e, 600, 1320, 62);       // gap 360 = 0.75 拍
        const std::string he = renderHtml(e, c, "t");
        check(countOf(he, "class='note eighth dotted'") == 1,
              "0.75 拍精确档仍是单个附点八分休止列");
        check(countOf(he, "<span class='num'>0</span>") == 1,
              "精确档不碎片化，只画一个 0 列");

        // 2.5 拍 -> 二分(2.0) + 八分(0.5) 两列。
        MidiData f;
        f.division = 480;
        f.timeSig.valid = true;
        f.trackNames[0] = "Vocal";
        add(f, 0, 120, 60);
        add(f, 1320, 600, 62);       // gap 1200 = 2.5 拍
        const std::string hf = renderHtml(f, c, "t");
        check(hf.find("--n:1") != std::string::npos &&
                  hf.find("class='note eighth'") != std::string::npos,
              "2.5 拍拆成二分休止 + 八分休止");
        check(countOf(hf, "<span class='num'>0</span>") == 2,
              "2.5 拍只拆两列，不碎片化");

        // 拆不干净的间隙（460 tick = 0.958 拍，不是 1/16 拍整数倍）
        // 必须保留旧的最近档单列兜底，与另外两端口径一致。
        MidiData g;
        g.division = 480;
        g.timeSig.valid = true;
        g.trackNames[0] = "Vocal";
        add(g, 0, 240, 60);
        add(g, 700, 1220, 62);       // gap 460
        const std::string hg = renderHtml(g, c, "t");
        check(countOf(hg, "<span class='num'>0</span>") == 1,
              "拆不干净的 0.958 拍仍按最近档画一个 0 列");
        check(hg.find("--n:1") == std::string::npos,
              "兜底路径不擅自改成 2 拍");
    }

    {
        // ---- MusicXML 连梁（<beam>）----
        // 没有 <beam> 时 MuseScore 只能靠自动连梁，混合时值 / 跨拍连音会连错。
        // 口径与 HTML 端减时线一致：同小节、同谱表、同声部、首尾相接、
        // 同一拍（三连音例外）；时值 >= 四分不画梁。
        std::cout << "\n测试 MusicXML <beam> 连梁\n";
        auto add = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        auto countOf = [](const std::string& s, const std::string& sub) {
            size_t n = 0, pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                n++; pos += sub.size();
            }
            return n;
        };
        KeyEstimate c{0, true};

        // 同拍两个八分 + 一个四分：八分成一组，四分不画梁。
        MidiData a;
        a.division = 480;
        a.timeSig.valid = true;
        a.trackNames[0] = "Vocal";
        add(a, 0, 240, 60);
        add(a, 240, 240, 62);
        add(a, 480, 480, 64);
        const std::string ax = renderMusicXml(a, c);
        check(countOf(ax, "<beam number=\"1\">begin</beam>") == 1 &&
                  countOf(ax, "<beam number=\"1\">end</beam>") == 1,
              "同拍两个八分连成一组梁（begin/end 各一）");
        check(countOf(ax, "<beam ") == 2,
              "四分音符不参与连梁，全曲只有 2 个 <beam> 元素");
        const size_t aType = ax.find("<type>eighth</type>");
        const size_t aBeam = ax.find("<beam number=\"1\">begin</beam>");
        check(aType != std::string::npos && aBeam != std::string::npos &&
                  aType < aBeam,
              "<beam> 排在 <type> 之后（MusicXML 子元素顺序）");

        // 跨拍不连：两拍各自成一组。
        MidiData b;
        b.division = 480;
        b.timeSig.valid = true;
        b.trackNames[0] = "Vocal";
        add(b, 0, 240, 60);
        add(b, 240, 240, 62);
        add(b, 480, 240, 64);
        add(b, 720, 240, 65);
        const std::string bx = renderMusicXml(b, c);
        check(countOf(bx, "<beam number=\"1\">begin</beam>") == 2 &&
                  countOf(bx, "<beam number=\"1\">end</beam>") == 2,
              "按拍分组：跨拍的两个八分各成一组，不连成跨强拍长梁");

        // 一拍四个十六分：一级梁 + 二级梁都成一组（begin/continue*2/end）。
        MidiData s;
        s.division = 480;
        s.timeSig.valid = true;
        s.trackNames[0] = "Vocal";
        add(s, 0, 120, 60);
        add(s, 120, 120, 62);
        add(s, 240, 120, 64);
        add(s, 360, 120, 65);
        add(s, 480, 1440, 67);
        const std::string sx = renderMusicXml(s, c);
        check(countOf(sx, "<beam number=\"2\">begin</beam>") == 1 &&
                  countOf(sx, "<beam number=\"2\">continue</beam>") == 2 &&
                  countOf(sx, "<beam number=\"2\">end</beam>") == 1,
              "十六分音符连出二级梁（begin/continue/continue/end）");

        // 十六 + 十六 + 八分：一级梁连三个，二级梁只覆盖前面两个十六分。
        MidiData m;
        m.division = 480;
        m.timeSig.valid = true;
        m.trackNames[0] = "Vocal";
        add(m, 0, 120, 60);
        add(m, 120, 120, 62);
        add(m, 240, 240, 64);
        add(m, 480, 1440, 65);
        const std::string mx = renderMusicXml(m, c);
        check(countOf(mx, "<beam number=\"1\">begin</beam>") == 1 &&
                  countOf(mx, "<beam number=\"1\">continue</beam>") == 1 &&
                  countOf(mx, "<beam number=\"1\">end</beam>") == 1,
              "混合时值：一级梁仍连成一组三颗音");
        check(countOf(mx, "<beam number=\"2\">begin</beam>") == 1 &&
                  countOf(mx, "<beam number=\"2\">end</beam>") == 1,
              "次级梁只盖住两个十六分，不延伸到八分上");

        // 十六 + 八分 + 十六：二级梁的两颗参与者各自孤立，不能整层丢掉。
        // 少了第二层梁，记谱软件就把十六分显示成八分；HTML 端却按
        // 自身时值画两条减时线，两端读出的节奏会分叉。
        MidiData h;
        h.division = 480;
        h.timeSig.valid = true;
        h.trackNames[0] = "Vocal";
        add(h, 0, 120, 60);
        add(h, 120, 240, 62);
        add(h, 360, 120, 64);
        add(h, 480, 1440, 65);
        const std::string hx = renderMusicXml(h, c);
        check(countOf(hx, "<beam number=\"2\">forward hook</beam>") == 1 &&
                  countOf(hx, "<beam number=\"2\">backward hook</beam>") == 1,
              "二级梁两颗孤立项各画一个钩形梁，组首 forward / 组尾 backward");
        check(countOf(hx, "<beam number=\"2\">begin</beam>") == 0 &&
                  countOf(hx, "<beam number=\"2\">continue</beam>") == 0 &&
                  countOf(hx, "<beam number=\"2\">end</beam>") == 0,
              "孤立项不伪装成长梁，钩不会跨过中间的八分");
        check(countOf(hx, "<beam number=\"2\"") == 2,
              "参与连梁的每颗十六分都带第二层梁（层级完整）");
        check(countOf(hx, "<beam number=\"1\">begin</beam>") == 1 &&
                  countOf(hx, "<beam number=\"1\">continue</beam>") == 1 &&
                  countOf(hx, "<beam number=\"1\">end</beam>") == 1,
              "一级梁仍把三颗音连成一组");

        // 孤立的八分（后面跟着休止）：单颗音不画梁。
        MidiData iso;
        iso.division = 480;
        iso.timeSig.valid = true;
        iso.trackNames[0] = "Vocal";
        add(iso, 0, 480, 60);
        add(iso, 480, 240, 62);      // 八分，之后是 240 tick 休止
        add(iso, 960, 960, 64);
        const std::string ix = renderMusicXml(iso, c);
        check(countOf(ix, "<beam ") == 0,
              "休止打断连梁：孤立八分不画梁");

        // 复拍子：6/8 的梁组是附点四分（三个八分一组），不是每个八分一组。
        // 按「分母分之一」算拍长的话，六个八分全落在不同的拍上，一条梁都
        // 画不出来——旧版正是如此，而 HTML 端与它同一口径，一起画错。
        MidiData s68;
        s68.division = 480;
        s68.timeSig.valid = true;
        s68.timeSig.numerator = 6;
        s68.timeSig.denomPow = 3;      // 6/8
        s68.trackNames[0] = "Vocal";
        for (int i = 0; i < 6; ++i) add(s68, 240L * i, 240, 60 + i);
        const std::string s68x = renderMusicXml(s68, c);
        check(countOf(s68x, "<beam number=\"1\">begin</beam>") == 2 &&
                  countOf(s68x, "<beam number=\"1\">continue</beam>") == 2 &&
                  countOf(s68x, "<beam number=\"1\">end</beam>") == 2,
              "6/8 六个八分连成两组三音，而不是六个单颗音各成一组");
        check(countOf(s68x, "<beam ") == 6,
              "6/8 全曲共 6 个 <beam> 元素（两组各三颗音）");

        // 3/8：整小节三个八分连成一组（分子能被 3 整除且分母 >= 8）。
        MidiData s38;
        s38.division = 480;
        s38.timeSig.valid = true;
        s38.timeSig.numerator = 3;
        s38.timeSig.denomPow = 3;      // 3/8
        s38.trackNames[0] = "Vocal";
        for (int i = 0; i < 3; ++i) add(s38, 240L * i, 240, 60 + i);
        const std::string s38x = renderMusicXml(s38, c);
        check(countOf(s38x, "<beam number=\"1\">begin</beam>") == 1 &&
                  countOf(s38x, "<beam number=\"1\">continue</beam>") == 1 &&
                  countOf(s38x, "<beam number=\"1\">end</beam>") == 1,
              "3/8 整小节三个八分连成一组");

        // 分组边长本身：单拍子一拍、复拍子附点拍（1/分母 x 3）。
        check(TimeSignature{4, 2, true}.beamUnitTicks(480) == 480,
              "4/4 的分组边长是一拍");
        check(TimeSignature{3, 2, true}.beamUnitTicks(480) == 480,
              "3/4 的分组边长也是一拍");
        check(TimeSignature{2, 1, true}.beamUnitTicks(480) == 960,
              "2/2 的分组边长是二分音符");
        check(TimeSignature{6, 3, true}.beamUnitTicks(480) == 720,
              "6/8 的分组边长是附点四分（三个八分）");
        check(TimeSignature{9, 3, true}.beamUnitTicks(480) == 720,
              "9/8 同样是附点四分一组");
        check(TimeSignature{3, 3, true}.beamUnitTicks(480) == 720,
              "3/8 整小节一组");
        // 大谱表：按谱表分组，且 <staff> 必须排在 <beam> 之前。
        MidiData g;
        g.division = 480;
        g.timeSig.valid = true;
        g.trackNames[0] = "Piano";
        add(g, 0, 240, 84);          // 上行谱两个八分
        add(g, 240, 240, 86);
        add(g, 480, 240, 36);        // 下行谱两个八分
        add(g, 720, 240, 38);
        const std::string gx = renderMusicXml(g, c);
        check(gx.find("<staff>1</staff><beam number=\"1\">begin</beam>") !=
                  std::string::npos,
              "大谱表上行谱的梁落在 staff 1，且 <beam> 排在 <staff> 之后");
        check(gx.find("<staff>2</staff><beam number=\"1\">begin</beam>") !=
                  std::string::npos,
              "大谱表下行谱的梁落在 staff 2，不跨谱表连梁");

        // 三十二分 + 十六分 + 三十二分 + 八分：三级梁同样两颗孤立项，
        // 而二级梁仍是一整段 begin/continue/end。
        MidiData h3;
        h3.division = 480;
        h3.timeSig.valid = true;
        h3.trackNames[0] = "Vocal";
        add(h3, 0, 60, 60);
        add(h3, 60, 120, 62);
        add(h3, 180, 60, 64);
        add(h3, 240, 240, 65);
        add(h3, 480, 1440, 67);
        const std::string h3x = renderMusicXml(h3, c);
        check(countOf(h3x, "<beam number=\"3\">forward hook</beam>") == 1 &&
                  countOf(h3x, "<beam number=\"3\">backward hook</beam>") == 1,
              "三级梁的孤立项也画 forward/backward hook");
        check(countOf(h3x, "<beam number=\"2\">begin</beam>") == 1 &&
                  countOf(h3x, "<beam number=\"2\">continue</beam>") == 1 &&
                  countOf(h3x, "<beam number=\"2\">end</beam>") == 1,
              "三级梁孤立时二级梁仍是完整三音段");

        // 2/2 一拍（960 tick）内混合时值：组首孤立项 forward、
        // 组内中间孤立项 backward，末尾两颗连续十六分仍画 begin/end。
        MidiData mid;
        mid.division = 480;
        mid.timeSig.valid = true;
        mid.timeSig.numerator = 2;
        mid.timeSig.denomPow = 1;
        mid.trackNames[0] = "Vocal";
        add(mid, 0, 120, 60);
        add(mid, 120, 240, 62);
        add(mid, 360, 120, 64);
        add(mid, 480, 240, 65);
        add(mid, 720, 120, 67);
        add(mid, 840, 120, 69);
        add(mid, 960, 960, 72);
        const std::string midx = renderMusicXml(mid, c);
        check(countOf(midx, "<beam number=\"2\">forward hook</beam>") == 1 &&
                  countOf(midx, "<beam number=\"2\">backward hook</beam>") == 1 &&
                  countOf(midx, "<beam number=\"2\">begin</beam>") == 1 &&
                  countOf(midx, "<beam number=\"2\">end</beam>") == 1,
              "2/2 一拍内：组首/中间孤立项画钩，末尾连续段画 begin/end");

        // 完全孤立的十六分音符画的是符尾（flag），不该凭空出现梁或钩。
        MidiData iso16;
        iso16.division = 480;
        iso16.timeSig.valid = true;
        iso16.trackNames[0] = "Vocal";
        add(iso16, 0, 480, 60);
        add(iso16, 480, 120, 62);
        add(iso16, 960, 960, 64);
        const std::string i16x = renderMusicXml(iso16, c);
        check(countOf(i16x, "<beam ") == 0,
              "完全孤立的十六分音符画符尾而不是梁或钩");

        // ---- 大谱表方向记号的谱表归属（v1.68.0）----
        // 旧实现给整个声部只算一次「全曲多数音高侧」：同一声部整小节换到
        // 另一行谱时，力度记号会印到完全没有音符的空白谱行。下面第 2 小节
        // 全是低音，第 4 小节前半高音 / 后半低音且中途换档，全曲高音 10 个
        // 低音 6 个，多数侧仍是上行谱，正好复现这个缺陷。
        auto measureBody = [](const std::string& xml, int num) {
            const std::string tag = "<measure number=\"" + std::to_string(num) + "\">";
            const size_t a = xml.find(tag);
            if (a == std::string::npos) return std::string();
            const size_t b = xml.find("</measure>", a);
            return xml.substr(a, b == std::string::npos ? std::string::npos : b - a);
        };
        auto dirStaffs = [](const std::string& body) {
            std::vector<int> out;
            size_t pos = 0;
            while ((pos = body.find("<direction", pos)) != std::string::npos) {
                const size_t e = body.find("</direction>", pos);
                if (e == std::string::npos) break;
                const std::string d = body.substr(pos, e - pos);
                const size_t s = d.find("<staff>");
                if (s != std::string::npos)
                    out.push_back(std::stoi(d.substr(s + 7)));
                pos = e + 12;
            }
            return out;
        };
        MidiData sw;
        sw.division = 480;
        sw.timeSig.valid = true;
        sw.trackNames[0] = "Piano";
        sw.channelProgram[0] = 0;
        const int swBars[4][4] = {{72, 74, 76, 77}, {36, 40, 43, 48},
                                  {79, 81, 83, 84}, {72, 74, 43, 48}};
        for (int b = 0; b < 4; ++b)
            for (int i = 0; i < 4; ++i)
                add(sw, b * 1920 + i * 480, 480, swBars[b][i]);
        sw.dynamics.push_back({0, 30, 0});        // p
        sw.dynamics.push_back({1920, 100, 0});    // ff（第 2 小节，全低音）
        sw.dynamics.push_back({3840, 70, 0});     // mf
        sw.dynamics.push_back({6720, 110, 0});    // 第 4 小节第 3 拍中途换档
        const std::string swx = renderMusicXml(sw, c);
        check(swx.find("<staves>2</staves>") != std::string::npos,
              "换谱表样本被判为大谱表");
        const std::vector<int> d1 = dirStaffs(measureBody(swx, 1));
        const std::vector<int> d2 = dirStaffs(measureBody(swx, 2));
        const std::vector<int> d3 = dirStaffs(measureBody(swx, 3));
        const std::vector<int> d4 = dirStaffs(measureBody(swx, 4));
        check(!d1.empty() && d1 == std::vector<int>{1},
              "第 1 小节全在上行谱，方向记号标 staff 1");
        check(!d2.empty() && d2 == std::vector<int>(d2.size(), 2),
              "第 2 小节全在下行谱，方向记号全部标 staff 2（旧实现会标 1）");
        check(!d3.empty() && d3 == std::vector<int>{1},
              "第 3 小节回到上行谱，方向记号再次标 staff 1");
        check(measureBody(swx, 4).find("<offset>960</offset>") != std::string::npos &&
                  !d4.empty() && d4 == std::vector<int>(d4.size(), 2),
              "第 4 小节中途换档按拍位定位到后半小节的低音谱表");
        check(countOf(swx, "<staff>1</staff>") >= 4 &&
                  countOf(swx, "<staff>2</staff>") >= 4,
              "两行谱的音符与方向记号都实际出现");

        // ---- 大谱表跨谱表渐强楔形必须落同一谱表（v1.70.0）----
        // 旧实现每条 <direction> 的 <staff> 各自按拍位取：crescendo 起点在
        // 上行谱(1)、stop 落在下行谱(2)，一条 hairpin 被拆到两个谱表，
        // MuseScore 渲染成断裂的楔形。修复后以起点谱表为锚，stop 复用同一
        // <staff>，整条楔形留在同一行谱上。
        auto addGsw = [](MidiData& d, long start, long dur, int pitch) {
            NoteEvent n;
            n.startTicks = start;
            n.durationTicks = dur;
            n.rawDurationTicks = dur;
            n.pitch = pitch;
            n.channel = 0;
            n.track = 0;
            d.notes.push_back(n);
        };
        MidiData gsw;
        gsw.division = 480;
        gsw.timeSig.valid = true;
        gsw.trackNames[0] = "Piano";
        gsw.channelProgram[0] = 0;
        // 第 1 小节上行谱(72-77)，第 2 小节下行谱(34-48)：跨度 43、跨 C(60)
        const int gswHi[4] = {72, 74, 76, 77};
        const int gswLo[4] = {34, 38, 43, 48};
        for (int i = 0; i < 4; i++) {
            addGsw(gsw, i * 480, 480, gswHi[i]);
            addGsw(gsw, 1920 + i * 480, 480, gswLo[i]);
        }
        // CC11 斜坡 30->55->100（>=3 点、跨度>=24）-> 一条 crescendo，
        // 起点 tick 0（落在第 1 小节上行谱）、终点 tick 1920（落在第 2 小节下行谱）
        gsw.dynamics.push_back({0, 30, 0});
        gsw.dynamics.push_back({960, 55, 0});
        gsw.dynamics.push_back({1920, 100, 0});
        const KeyEstimate gswKey{0, true};
        const std::string gswx = renderMusicXml(gsw, gswKey);
        check(gswx.find("<staves>2</staves>") != std::string::npos,
              "跨谱表楔形样本被判为大谱表");
        auto wedgeStaffs = [](const std::string& xml) {
            std::vector<int> out;
            size_t pos = 0;
            while ((pos = xml.find("<direction", pos)) != std::string::npos) {
                const size_t e = xml.find("</direction>", pos);
                if (e == std::string::npos) break;
                const std::string d = xml.substr(pos, e - pos);
                if (d.find("wedge") != std::string::npos) {
                    const size_t s = d.find("<staff>");
                    if (s != std::string::npos)
                        out.push_back(std::stoi(d.substr(s + 7)));
                }
                pos = e + 12;
            }
            return out;
        };
        const std::vector<int> ws = wedgeStaffs(gswx);
        check(ws.size() == 2 && ws[0] == 1 && ws[1] == 1,
              "crescendo 起与 stop 止都落同一谱表（起点 staff 1），"
              "不再跨谱表断裂（旧实现 stop 落 staff 2）");
    }

    // -------------------------------------------------------------------
    // 段落名截断必须落在 UTF-8 字符边界上（v1.79.0）
    // 旧实现 text.substr(0, 40) 是**按字节**下刀：一个汉字 3 字节，40 不是 3 的
    // 倍数，最后一个字被劈成半截，剩下的孤立续字节（0x80..0xBF）让三端全是
    // 非法 UTF-8。MusicXML 更致命——解析器在第一个非法字节就报错，MuseScore
    // 会拒绝打开整份谱，而不是只丢一个段落名。
    // 新口径：上限 40 个字符（码点），截断点回退到序列首字节，半个字整个丢掉。
    // -------------------------------------------------------------------
    {
        // 严格 UTF-8 校验：首字节声明的长度必须整段都在，续字节必须是 10xxxxxx。
        auto utf8Valid = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size()) {
                const unsigned char c = (unsigned char)s[i];
                size_t len = 0;
                if (c < 0x80) { i++; continue; }
                else if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                else return false;            // 孤立续字节 / 非法首字节
                if (i + len > s.size()) return false;
                for (size_t k = 1; k < len; k++)
                    if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
                i += len;
            }
            return true;
        };
        auto utf8Prefix = [](const std::string& s, size_t maxChars) {
            size_t i = 0, c = 0;
            while (i < s.size() && c < maxChars) {
                const unsigned char b = (unsigned char)s[i];
                size_t len = 1;
                if ((b & 0xE0) == 0xC0) len = 2;
                else if ((b & 0xF0) == 0xE0) len = 3;
                else if ((b & 0xF8) == 0xF0) len = 4;
                if (i + len > s.size()) break;
                i += len;
                c++;
            }
            return s.substr(0, i);
        };

        const std::string cn19 = "副歌第一段从这里开始进入主歌的过渡段落";  // 19 字 / 57 字节
        std::string cn50;
        while (cn50.size() < 150) cn50 += "超长中文段落名称";   // 8 字一轮，停在 56 字
        const std::string ascii45 = "ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDE";

        check(cn19.size() == 57 && cn50.size() > 150 && ascii45.size() == 45,
              "素材字节长度符合预期（19 字 = 57B，长名 > 150B）");

        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.tempoChanges.push_back({0, 120.0});
        d.trackNames[0] = "Melody";
        d.markers.push_back({0, cn19});
        d.markers.push_back({1920, cn50});
        d.markers.push_back({3840, ascii45});
        NoteEvent n;
        n.track = 0; n.channel = 0; n.velocity = 88;
        n.startSeconds = 0; n.durationSeconds = 0.5;
        n.durationTicks = 480;
        for (int i = 0; i < 24; i++) {
            n.pitch = 60 + (i % 6);
            n.startTicks = i * 480;
            d.notes.push_back(n);
        }
        const KeyEstimate key{0, true};
        const std::string jp = formatJianpu(d, key);
        const std::string htm = renderHtml(d, key, "t");
        const std::string xml = renderMusicXml(d, key);

        check(jp.find(cn19) != std::string::npos,
              "19 字中文段落名一个字都不少（旧版只剩 13 字 + 半个字）");
        check(htm.find("<span class='section'>" + cn19 + "</span>") != std::string::npos,
              "HTML 里的中文段落名同样完整");
        check(xml.find("<words>" + cn19 + "</words>") != std::string::npos,
              "MusicXML 的 <words> 同样完整");

        const std::string cn40 = utf8Prefix(cn50, 40);
        const std::string cn41 = utf8Prefix(cn50, 41);
        check(cn40.size() == 120 && cn41.size() == 123,
              "40 个汉字 = 120 字节，第 41 个字的起点在 120");
        check(jp.find(cn40) != std::string::npos &&
              jp.find(cn41) == std::string::npos,
              "超长中文段落名截到 40 个字符（不是 40 字节）");
        check(jp.find(ascii45) == std::string::npos &&
              jp.find(ascii45.substr(0, 40)) != std::string::npos,
              "45 个 ASCII 字符同样截到 40 个字符，与旧版一致");

        check(utf8Valid(jp), "文本谱输出是合法 UTF-8");
        check(utf8Valid(htm), "HTML 输出是合法 UTF-8");
        check(utf8Valid(xml), "MusicXML 输出是合法 UTF-8");
        check(jp.find("\xEF\xBF\xBD") == std::string::npos,
              "没有把非法字节悄悄替换成 U+FFFD（那是另一种隐瞒）");

        // 直接喂一个被上游截过的残片：整个残字丢掉，不留下半截序列
        MidiData trunc;
        trunc.division = 480;
        trunc.timeSig.valid = true;
        trunc.trackNames[0] = "Melody";
        trunc.markers.push_back({0, cn19.substr(0, cn19.size() - 1)});
        trunc.notes.push_back(n);
        const std::string tj = formatJianpu(trunc, key);
        check(utf8Valid(tj) && tj.find(cn19) == std::string::npos,
              "本身残缺的段落名整字丢弃，输出仍是合法 UTF-8");
    }

    // ---- v1.80.0：旧编码 MIDI 文本的编码归一 ----
    // 0x03/0x05/0x06 等文本 meta 是裸字节：老中文 MIDI 用 GBK、老日文用
    // Shift-JIS。原样输出会让 MusicXML 变成非法 UTF-8，MuseScore 拒绝打开。
    {
        using namespace m2j::textenc;
        const std::string gbkName =
            "\xD6\xF7\xD0\xFD\xC2\xC9" " " "\xB8\xD6\xC7\xD9";            // 主旋律 钢琴
        const std::string sjisName =
            "\x8E\xE5\x90\xF9\x97\xA5" " " "\x83\x73\x83\x41\x83\x6D";    // 主旋律 ピアノ
        const std::string utf8Name = "主旋律 钢琴";
        const std::string utf8SjisName = "主旋律 ピアノ";

        // 严格 UTF-8 校验：过长编码 / 代理区 / 越界 / 截断 / 孤立续字节都要挡住
        check(isValidUtf8(""), "空串是合法 UTF-8");
        check(isValidUtf8("Piano Track"), "纯 ASCII 是合法 UTF-8");
        check(isValidUtf8(utf8Name), "三字节中文是合法 UTF-8");
        check(isValidUtf8("\xF0\x9F\x98\x80"), "四字节 emoji 是合法 UTF-8");
        check(!isValidUtf8("\xC0\xAF"), "过长编码 C0 AF 判为非法");
        check(!isValidUtf8("\xED\xA0\x80"), "UTF-16 代理区判为非法");
        check(!isValidUtf8("\xF4\x90\x80\x80"), "超过 U+10FFFF 判为非法");
        check(!isValidUtf8("\xE4\xB8"), "末尾只写了一半的序列判为非法");
        check(!isValidUtf8("\x80"), "孤立续字节判为非法");

        // 探测：合法 UTF-8 一个字节都不许动
        check(detect("Piano Track") == Encoding::Utf8, "ASCII 判为 UTF-8（不进转码分支）");
        check(detect(utf8Name) == Encoding::Utf8, "UTF-8 中文判为 UTF-8");
        check(detect(gbkName) == Encoding::Gb2312, "GBK 中文判为 GB2312");
        check(detect(sjisName) == Encoding::ShiftJis, "Shift-JIS 假名判为 Shift-JIS");
        check(detect("it\x92s") == Encoding::Fold, "Windows-1252 弯引号判为折叠兜底");
        check(normalize(utf8Name) == utf8Name, "合法 UTF-8 归一化后逐字节不变");

        // 解码结果
        check(normalize(gbkName) == utf8Name, "GBK 轨名解成 UTF-8 中文");
        check(normalize(sjisName) == utf8SjisName, "Shift-JIS 假名解成 UTF-8 假名");
        check(normalize("it\x92s") == "it\xE2\x80\x99s", "0x92 折叠成 U+2019 右单引号");
        check(normalize("caf\xE9") == "caf\xC3\xA9", "Latin-1 的 café 折叠成 UTF-8 café");
        check(foldByte(0x80) == 0x20ACu, "CP1252 0x80 -> 欧元符号 U+20AC");
        check(foldByte(0x92) == 0x2019u, "CP1252 0x92 -> U+2019");

        // 映射表本身：GB2312「啊」= B0A1，JIS「あ」= 0x82A0 -> 4 区 2 点
        check(cellAt(kGb2312Hex, 0xB0 - 0xA0, 0xA1 - 0xA0) == 0x554Au,
              "GB2312 表 B0A1 = 啊 U+554A");
        int row = 0, col = 0;
        check(sjisToJis(0x82, 0xA0, row, col) && row == 4 && col == 2,
              "Shift-JIS 0x82A0 落在 JIS 4 区 2 点");
        check(cellAt(kJis0208Hex, 4, 2) == 0x3042u, "JIS 表 4/2 = あ U+3042");

        // 陷阱：GBK「水」= CB AE 恰好也是合法的 2 字节 UTF-8（U+02EE），
        // 「平」= C6 BD 同理。单看一条永远判不出来，只能靠整份文件一次判定。
        const std::string gbkWater = "\xCB\xAE";
        const std::string utf8Water = "水";
        check(isValidUtf8(gbkWater), "GBK「水」的字节对碰巧是合法 UTF-8（陷阱成立）");
        check(normalize(gbkWater) != utf8Water, "单独一条无法分辨，正是文件级判定的理由");
        check(decode(gbkWater, Encoding::Gb2312) == utf8Water,
              "显式按 GB2312 解 CB AE 得到「水」");

        // 端到端：同一份 SMF，文本分别用 GBK 与 UTF-8 写，解析结果必须一致
        auto metaEv = [](uint8_t type, const std::string& s) {
            std::vector<uint8_t> v = {0xFFu, type};
            putVlq(v, (uint32_t)s.size());
            v.insert(v.end(), s.begin(), s.end());
            return v;
        };
        auto legacySmf = [&](const std::string& name, const std::string& mark,
                             const std::string& lyric) {
            SmfTrack t0, t1;
            t0.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
            t0.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
            t1.at(0, metaEv(0x03, name));
            t1.at(0, metaEv(0x06, mark));
            t1.at(0, metaEv(0x05, lyric));   // 歌词就用「水」，压上面的陷阱
            t1.at(0, ev({0x90, 60, 88}));
            t1.at(470, ev({0x80, 60, 0x40}));
            return buildSmf(2, 480, {t0, t1});
        };
        const auto gbkBytes = legacySmf(gbkName, "\xB8\xB1\xB8\xE8\xB6\xCE\xC2\xE4",
                                        gbkWater);   // 副歌段落
        const auto utf8Bytes = legacySmf(utf8Name, "副歌段落", utf8Water);
        MidiData dg, du;
        const bool okG = loadMidiFromMemory(gbkBytes.data(), gbkBytes.size(), dg);
        const bool okU = loadMidiFromMemory(utf8Bytes.data(), utf8Bytes.size(), du);
        check(okG && okU, "GBK / UTF-8 两份内存 SMF 都能解析");
        check(dg.trackNames[1] == utf8Name, "解析层把 GBK 轨名归一成 UTF-8");
        check(dg.markers.size() == 1 && dg.markers[0].text == "副歌段落",
              "解析层把 GBK 段落名归一成 UTF-8");
        check(dg.midiLyrics.size() == 1 && dg.midiLyrics[0].text == utf8Water,
              "碰巧合法 UTF-8 的 GBK「水」也按文件级判定解对");
        check(okG && okU && dg.markers.size() == du.markers.size() &&
              dg.midiLyrics.size() == du.midiLyrics.size() &&
              !dg.markers.empty() && !dg.midiLyrics.empty() &&
              dg.trackNames[1] == du.trackNames[1] &&
              dg.markers[0].text == du.markers[0].text &&
              dg.midiLyrics[0].text == du.midiLyrics[0].text,
              "GBK 与 UTF-8 两份同源 SMF 解析出的文本完全一致");
    }

    // ---- v1.84.0：GBK 扩展区生僻字正确解码 ----
    // v1.80 的表只收 GB2312 常用区（lead/trail 都在 0xA1..0xFE），GBK 扩展区的
    // 生僻字（镕喆堃淼鑫 等）会被整对折叠成 Latin-1 乱码。v1.84 新增 kGbkExtHex
    // 覆盖 lead 0x81..0xA0 全 trail、以及 lead 0xA1..0xFE 的 trail 0x40..0x7E /
    // 0x80..0xA0，并把常用区改用 gbk 编解码（顺带把 GB2312 漏定义的同字节码位也补上）。
    {
        using namespace m2j::textenc;
        // 五个生僻字：镕 E9 46 / 喆 86 B4 / 堃 88 D2 / 淼 ED B5 / 鑫 F6 CE。
        // 其中 淼/鑫 落在常用区（A1..FE, A1..FE），其余在扩展区。
        const std::string rare = "\xE9\x46\x86\xB4\x88\xD2\xED\xB5\xF6\xCE";  // 镕喆堃淼鑫
        const std::string rareUtf8 = "镕喆堃淼鑫";
        check(decodeGb2312(rare).text == rareUtf8,
              "GBK 扩展区五生僻字解码成正确 UTF-8");
        check(detect(rare) == Encoding::Gb2312, "含生僻字的 GBK 串判为 GB2312");
        check(normalize(rare) == rareUtf8, "normalize 把五生僻字归一成 UTF-8");

        // 扩展表本身：kGbkExtHex 的扁平偏移与码位
        check(gbkExtOffset(0xE9, 0x46) >= 0, "镕 的扩展偏移合法");
        check(readCell4(kGbkExtHex + gbkExtOffset(0xE9, 0x46) * 4) == 0x9555u,
              "kGbkExtHex 表 E9 46 = 镕 U+9555");
        check(readCell4(kGbkExtHex + gbkExtOffset(0x86, 0xB4) * 4) == 0x5586u,
              "kGbkExtHex 表 86 B4 = 喆 U+5586");
        check(readCell4(kGbkExtHex + gbkExtOffset(0x88, 0xD2) * 4) == 0x5803u,
              "kGbkExtHex 表 88 D2 = 堃 U+5803");

        // 常用区改用 gbk：补足 GB2312 在同字节范围内漏定义的码位。
        // GBK 常用区表 B0 A1 = 啊 不变；补的码位如 A2 A1 = ⅰ、A1 A4 = ·。
        check(cellAt(kGb2312Hex, 0xB0 - 0xA0, 0xA1 - 0xA0) == 0x554Au,
              "GBK 常用区 B0 A1 = 啊 U+554A（与旧 GB2312 表一致）");
        check(cellAt(kGb2312Hex, 0xA2 - 0xA0, 0xA1 - 0xA0) == 0x2170u,
              "GBK 常用区 A2 A1 = ⅰ U+2170（GB2312 旧表此处为 0）");
        check(cellAt(kGb2312Hex, 0xA1 - 0xA0, 0xA4 - 0xA0) == 0x00B7u,
              "GBK 常用区 A1 A4 = · U+00B7（GB2312 旧表此处误为 U+30FB）");

        // 端到端：轨名含生僻字，解析层必须归一成正确 UTF-8（不再折叠）。
        auto metaEv = [](uint8_t type, const std::string& s) {
            std::vector<uint8_t> v = {0xFFu, type};
            putVlq(v, (uint32_t)s.size());
            v.insert(v.end(), s.begin(), s.end());
            return v;
        };
        SmfTrack t0, t1;
        t0.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        t0.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        t1.at(0, metaEv(0x03, rare));
        t1.at(0, ev({0x90, 60, 88}));
        t1.at(470, ev({0x80, 60, 0x40}));
        const auto bytes = buildSmf(2, 480, {t0, t1});
        MidiData dm;
        const bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), dm);
        check(ok && dm.trackNames[1] == rareUtf8,
              "解析层把含生僻字的 GBK 轨名归一成正确 UTF-8");
    }

    // ---- v1.81.0 CLI 参数校验层（header-only，CLI 与单测共用同一套口径） ----
    // 这些纯函数决定了「某个词是选项还是取值」，判错就会把参数拼错伪装成
    // 文件缺失，或者把 --output 的值写成另一个选项名。
    {
        using namespace m2j::cli;
        check(valueOptions().size() == 11, "带值选项表共 11 项");
        check(flagOptions().size() == 14, "开关选项表共 14 项");
        check(optionNeedsValue("--html") && optionNeedsValue("--musicxml") &&
              optionNeedsValue("--soft-pedal-scale"), "输出/路径/系数类选项都需要取值");
        check(optionNeedsValue("--transpose") && optionNeedsValue("--track") &&
              optionNeedsValue("--pickup") && optionNeedsValue("--bars"),
              "数值类选项都需要取值");
        check(optionNeedsValue("--key") && optionNeedsValue("--gong") &&
              optionNeedsValue("--lyrics") && optionNeedsValue("--output"),
              "调性/歌词/文本输出选项都需要取值");
        check(!optionNeedsValue("--player") && !optionNeedsValue("--bar-numbers"),
              "纯开关不需要取值");
        check(!optionNeedsValue("--help") && !optionNeedsValue("--version"),
              "帮助/版本不需要取值");
        check(!optionNeedsValue("--bogus"), "未知选项不在带值表里");
        check(isKnownOption("--bar-numbers") && isKnownOption("--html"),
              "已知长选项能被识别");
        check(isKnownOption("-h") && isKnownOption("-v"), "短别名也算已知选项");
        check(!isKnownOption("--foo") && !isKnownOption("-x"),
              "未知长短选项都不算已知");
        check(isNumericToken("-2") && isNumericToken("-0.5") &&
              isNumericToken("+2") && isNumericToken("1.5"),
              "负数/带符号小数是合法取值 token");
        check(!isNumericToken("") && !isNumericToken("-2x") &&
              !isNumericToken("o.html") && !isNumericToken("--html"),
              "空串/残缺数字/文件名/选项名都不是数值 token");
        check(isOptionToken("--html") && isOptionToken("--foo") &&
              isOptionToken("-x"), "长选项与未知短参数都是选项 token");
        check(!isOptionToken("-2") && !isOptionToken("-1.5"),
              "负数值不能被误判成选项（--transpose -2 要用）");
        check(!isOptionToken("a.mid") && !isOptionToken("o.html"),
              "普通文件名不是选项 token");
        check(!isOptionToken("-") && isOptionToken("--"),
              "单独一个 - 是取值，-- 是选项分隔符");
        check(editDistance("kitten", "sitting") == 3, "编辑距离标准样例 = 3");
        check(editDistance("abc", "abc") == 0 && editDistance("a", "") == 1,
              "编辑距离：相同为 0、删一字符为 1");
        check(editDistance("", "") == 0, "编辑距离：两空串为 0");
        check(suggestOption("--bar-number") == "--bar-numbers",
              "少打一个 s 时建议 --bar-numbers");
        check(suggestOption("--musixml") == "--musicxml",
              "拼错 --musicxml 时给出正确写法");
        check(suggestOption("--no-dynamic") == "--no-dynamics",
              "拼错 --no-dynamics 时给出正确写法");
        check(suggestOption("--transposee") == "--transpose",
              "多打一个字母时也能建议 --transpose");
        check(suggestOption("--foo").empty() && suggestOption("-x").empty() &&
              suggestOption("--help").empty(),
              "离得太远 / 短参数 / 本来就正确时都不给建议");
        check(suggestOption("--bar-number").size() > 2,
              "建议返回的是完整长选项名");
    }
    // ---- v1.82.0 输出路径冲突预检（纯字符串，CLI 与单测共用） ----
    {
        using m2j::cli::normalizePath;
        using m2j::cli::samePath;
        using m2j::cli::validateOutputPaths;
        using m2j::cli::OutputPath;

        check(normalizePath("a.txt") == "a.txt",
              "路径归一化：普通相对路径原样返回");
        check(normalizePath("./a.txt") == "a.txt",
              "路径归一化：吃掉前导 ./");
        check(normalizePath(".\\a.txt") == "a.txt",
              "路径归一化：反斜杠按斜杠处理");
        check(normalizePath("dir//sub/../a.txt") == "dir/a.txt",
              "路径归一化：合并重复斜杠、吃掉 ..");
        check(normalizePath("C:\\TMP\\A.TXT") == "c:/tmp/a.txt",
              "路径归一化：Windows 盘符保留、大小写折叠");
        check(normalizePath("a/../b.txt") == "b.txt",
              "路径归一化：同级回退");
        check(normalizePath("../a.txt") == "../a.txt",
              "路径归一化：越过当前目录的 .. 保留");
        check(normalizePath("") == ".",
              "路径归一化：空串给稳定哨兵，不判为任意路径");

        check(samePath("a.mid", "./a.mid"),
              "同路径判定：./ 前缀视为同一文件");
        check(samePath("A.MID", "a.mid"),
              "同路径判定：按 Windows 语义大小写不敏感");
        check(samePath("dir\\a.mid", "dir/a.mid"),
              "同路径判定：反斜杠与斜杠等价");
        check(!samePath("a.mid", "b.mid") &&
              !samePath("a.txt", "a.txt.bak"),
              "同路径判定：不同文件名 / 加后缀不误报");
        check(!samePath("", "a.mid") && !samePath("a.mid", ""),
              "同路径判定：空路径不参与比较");

        const std::string in = "song.mid";
        check(validateOutputPaths(in, {}).empty(),
              "冲突预检：没有任何输出路径时放行");
        check(validateOutputPaths(in, {{"--output", ""}, {"--html", ""}}).empty(),
              "冲突预检：输出路径全空时放行");
        check(validateOutputPaths(in, {{"--output", "a.txt"},
                                      {"--html", "a.html"},
                                      {"--musicxml", "a.xml"}}).empty(),
              "冲突预检：三个不同输出路径放行");
        for (const char* opt : {"--output", "--html", "--musicxml"}) {
            const std::string e = validateOutputPaths(
                in, {{opt, "./song.mid"}});
            check(!e.empty() && e.find(opt) == 0 &&
                  e.find("覆盖源文件") != std::string::npos,
                  std::string(opt) + " 指向输入 MIDI 时拒绝并点明会覆盖源文件");
        }
        check(validateOutputPaths("x/song.mid",
                                 {{"--html", "x/./song.mid"}}).size() > 0,
              "冲突预检：带 ./ 的同目录写法也能拦住");
        for (size_t k = 0; k < 3; ++k) {
            const char* opts[3] = {"--output", "--html", "--musicxml"};
            std::vector<OutputPath> outs;
            outs.push_back({opts[k], "same.out"});
            outs.push_back({opts[(k + 1) % 3], "same.out"});
            const std::string e = validateOutputPaths(in, outs);
            check(!e.empty() && e.find("指向同一文件") != std::string::npos,
                  std::string(opts[k]) + " 与另一个输出撞路径时拒绝");
        }
        check(validateOutputPaths(in, {{"--output", "same.out"},
                                      {"--html", "same.out/../same.out"}}).size() > 0,
              "冲突预检：.. 绕一圈回到同一路径仍判为撞车");
        {
            const std::string e = validateOutputPaths(in,
                {{"--output", "./song.mid"}, {"--html", "./song.mid"}});
            check(e.find("覆盖源文件") != std::string::npos,
                  "同时踩两条时优先报「会覆盖源文件」");
        }
    }

    // ---- v1.83.0 调名严格语法 + 有限浮点 + 冲突开关（纯函数） ----
    {
        using m2j::cli::KeySpec;
        using m2j::cli::parseKeySpec;
        using m2j::cli::parseFiniteDouble;
        using m2j::cli::validateKeyOptions;

        KeySpec k;
        check(parseKeySpec("C", k) && k.pitchClass == 0 && k.isMajor
              && !k.flatSpelling, "调名 C = 0 大调");
        check(parseKeySpec("c", k) && k.pitchClass == 0,
              "调名小写 c 与 C 同义");
        check(parseKeySpec("Am", k) && k.pitchClass == 9 && !k.isMajor,
              "调名 Am = a 小调");
        check(parseKeySpec("aM", k) && k.pitchClass == 9 && !k.isMajor,
              "大写 M 后缀沿用既有小调口径");
        check(parseKeySpec("C#m", k) && k.pitchClass == 1 && !k.isMajor,
              "调名 C#m = C# 小调");
        check(parseKeySpec("C#M", k) && k.pitchClass == 1 && !k.isMajor,
              "调名 C#M 同样合法（三位已是上限）");
        check(parseKeySpec("B", k) && k.pitchClass == 11,
              "调名 B = 11");
        check(parseKeySpec("Bb", k) && k.pitchClass == 10 && k.flatSpelling,
              "调名 Bb 记住降号侧");
        check(parseKeySpec("Gb", k) && k.pitchClass == 6 && k.flatSpelling,
              "调名 Gb 记住降号侧（v1.76 六个降号）");
        check(parseKeySpec("F#", k) && k.pitchClass == 6 && !k.flatSpelling,
              "调名 F# 走升号侧");
        check(!parseKeySpec("", k), "空调名拒绝");
        check(!parseKeySpec("H", k), "H 不是音名");
        check(!parseKeySpec("1", k), "数字不是音名");
        check(!parseKeySpec(" C", k), "前导空格拒绝");
        check(!parseKeySpec("C ", k), "尾随空格拒绝（旧版被吞成 C 大调）");
        check(!parseKeySpec("Cfoo", k),
              "尾部垃圾 Cfoo 不再被吞成 C 大调");
        check(!parseKeySpec("Cmaj7", k),
              "和弦名 Cmaj7 不再被曲解成 C 小调");
        check(!parseKeySpec("Amxxx", k),
              "尾部垃圾 Amxxx 不再被吞成 a 小调");
        check(!parseKeySpec("C#b", k), "C#b 两个变音记号拒绝");
        check(!parseKeySpec("Cmm", k), "两个小调后缀拒绝");
        check(!parseKeySpec("Cb", k), "Cb 不在写法表内（与旧版一致）");
        check(!parseKeySpec("B#", k), "B# 不在写法表内（与旧版一致）");

        double v = -1;
        check(parseFiniteDouble("0.5", v) && v == 0.5,
              "0.5 解析成有限浮点");
        check(parseFiniteDouble("0", v) && v == 0.0, "0 是有限值");
        check(parseFiniteDouble("-1.5", v) && v == -1.5,
              "负数有限值放行");
        check(parseFiniteDouble(" 4", v) && v == 4.0,
              "前导空白沿用 strtod 的宽容口径");
        check(!parseFiniteDouble("nan", v), "nan 拒绝");
        check(!parseFiniteDouble("NaN", v), "NaN 拒绝");
        check(!parseFiniteDouble("inf", v), "inf 拒绝");
        check(!parseFiniteDouble("-inf", v), "-inf 拒绝");
        check(!parseFiniteDouble("1e309", v), "溢出成 inf 的值拒绝");
        check(!parseFiniteDouble("0.5x", v), "浮点尾部垃圾拒绝");
        check(!parseFiniteDouble("", v), "浮点空串拒绝");

        check(validateKeyOptions("C", "").empty() &&
              validateKeyOptions("", "G").empty() &&
              validateKeyOptions("", "").empty(),
              "只给一个调性入口（或都不给）时放行");
        check(validateKeyOptions("D", "G").find("不能同时使用") != std::string::npos,
              "--key 与 --gong 同时给时明确拒绝");
    }
    // ---- v1.85.0 时间轴跨度保护 ----
    // 畸形 delta-time 能把 note-on / note-off 拉到两端，拼出「幽灵长音」。
    // 旧版按小节切分 / 补空小节是 O(小节数)，一份几 KB 的坏文件就能让 HTML
    // 生成卡到十余秒、产出上百 MB。这里验证解析末尾把越界音符截断并留提示。
    std::cout << "\n测试时间轴跨度保护\n";
    {
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));   // 4/4
        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            long t = i * 480L;
            mel.at(t, ev({0x90, 60 + i, 90}));
            mel.at(t + 480, ev({0x80, 60 + i, 0}));
        }
        // 坏 delta：note-on 落在 3000 万 tick（远超 512 小节 = 98.3 万 tick 上限）
        const long kJump = 30000000L;
        mel.at(1920 + kJump, ev({0x90, 72, 90}));
        mel.at(1920 + kJump + 480, ev({0x80, 72, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        bool ok = loadMidiFromMemory(bytes.data(), bytes.size(), d);
        check(ok, "含巨额 delta 的 SMF 仍能解析（不崩、不卡死）");
        check(d.notes.size() == 4,
              "越界幽灵音被丢弃，只剩 4 个正常音, 实际: " + std::to_string(d.notes.size()));
        check(d.timelineClamped >= 1,
              "记录到越界音符, 实际: " + std::to_string(d.timelineClamped));
        const long cap = 512L * 1920L;
        long maxEnd = 0;
        for (const auto& n : d.notes)
            maxEnd = std::max(maxEnd, n.startTicks + n.durationTicks);
        check(maxEnd <= cap, "所有音符落在 512 小节内, 实际末端: " + std::to_string(maxEnd));
        check(truncationLabel(d).find("时间轴跨度异常") != std::string::npos,
              "页眉提示时间轴跨度异常: " + truncationLabel(d));
    }
    {
        // 单音时值上限：起点在界内、尾巴却超长（幽灵长音的另一种形态）。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(500000, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData dm;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), dm), "超长单音 SMF 解析成功");
        check(!dm.notes.empty() && dm.notes[0].durationTicks <= 256L * 1920L,
              "单个幽灵长音被夹到 256 小节内, 实际: " +
              std::to_string(dm.notes.empty() ? -1 : dm.notes[0].durationTicks));
        check(dm.timelineClamped >= 1, "超长单音计一次截断");
    }
    {
        // 无巨型 tick 的正常文件：timelineClamped 恒为 0，截断提示里不出现时间轴字样。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(480, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData dn;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), dn), "普通单音 SMF 解析成功");
        check(dn.timelineClamped == 0 && truncationLabel(dn).find("时间轴") == std::string::npos,
              "正常文件的截断提示不含时间轴字样（零回归）");
    }

    // ---- v1.86.0 越界「点事件」也要过同一道时间轴闸 ----
    //
    // v1.85 只夹 notes，于是 marker / 歌词 / CC11 / 变速 / 变拍号 / 转调 /
    // 换音色等落在 3000 万 tick 的事件仍被当成本曲的一部分：一份 1 小节
    // 4 个音的谱子会被谎报成「4/4 → 3/4(第11小节)」「♩=120 → 256」
    // 「1=C → 1=D」「声部：·小提琴」。这里逐张事件表验证统一丢弃。
    std::cout << "\n测试越界点事件截断\n";
    {
        const long kFar = 30000000L;
        const long cap = 512L * 1920L;
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));   // 4/4
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));          // 120 BPM
        cond.at(0, ev({0xFF, 0x59, 0x02, 0x00, 0x00}));                 // C 大调
        cond.at(0, ev({0xFF, 0x06, 0x05, 'I', 'n', 't', 'r', 'o'}));    // 界内段落
        cond.at(kFar, ev({0xFF, 0x06, 0x05, 'G', 'h', 'o', 's', 't'})); // markers
        cond.at(kFar, ev({0xFF, 0x01, 0x03, 'a', 'b', 'c'}));           // textEvents
        cond.at(kFar, ev({0xFF, 0x05, 0x01, 'x'}));                     // midiLyrics
        cond.at(kFar, ev({0xFF, 0x51, 0x03, 0x03, 0x93, 0x87}));        // tempoChanges
        cond.at(kFar, ev({0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08}));  // timeSigChanges
        cond.at(kFar, ev({0xFF, 0x59, 0x02, 0x02, 0x00}));              // keySigChanges
        cond.at(kFar, ev({0xB0, 0x0B, 100}));                           // dynamics (CC11)
        cond.at(kFar, ev({0xE0, 0x7F, 0x7F}));                          // pitchBends
        cond.at(kFar, ev({0xB0, 101, 0}));                               // bendRanges (RPN)
        cond.at(kFar, ev({0xB0, 100, 0}));
        cond.at(kFar, ev({0xB0, 6, 12}));
        cond.at(kFar, ev({0xB0, 101, 127}));
        cond.at(kFar, ev({0xB0, 100, 127}));
        cond.at(kFar, ev({0xB0, 0, 0}));                                 // Bank MSB
        cond.at(kFar, ev({0xC0, 40}));                                   // programTimeline
        cond.at(kFar, ev({0xB0, 0x40, 127}));                            // 踏板（解析期消费）

        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            mel.at(i * 480L, ev({0x90, 60 + i, 90}));
            mel.at(i * 480L + 480, ev({0x80, 60 + i, 0}));
        }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "含越界点事件的 SMF 仍能解析（不崩、不卡死）");
        check(d.notes.size() == 4,
              "4 个界内音符一个不少, 实际: " + std::to_string(d.notes.size()));
        check(d.timelineClamped == 0,
              "没有越界音符时不计音符截断, 实际: " + std::to_string(d.timelineClamped));
        check(d.timelineEventsDropped >= 10,
              "记录到 10 类越界点事件, 实际: " + std::to_string(d.timelineEventsDropped));

        auto noneBeyond = [cap](const auto& v) {
            for (const auto& e : v) if (e.tick >= cap) return false;
            return true;
        };
        check(noneBeyond(d.dynamics), "dynamics 无越界点");
        check(noneBeyond(d.tempoChanges), "tempoChanges 无越界点");
        check(noneBeyond(d.timeSigChanges), "timeSigChanges 无越界点");
        check(noneBeyond(d.keySigChanges), "keySigChanges 无越界点");
        check(noneBeyond(d.pitchBends), "pitchBends 无越界点");
        check(noneBeyond(d.bendRanges), "bendRanges 无越界点");
        check(noneBeyond(d.midiLyrics), "midiLyrics 无越界点");
        check(noneBeyond(d.textEvents), "textEvents 无越界点");
        check(noneBeyond(d.markers), "markers 无越界点");
        check(noneBeyond(d.programTimeline), "programTimeline 无越界点");
        check(d.markers.size() == 1, "只剩界内那条 Intro 段落, 实际: " +
              std::to_string(d.markers.size()));
        check(d.channelProgram.empty(),
              "越界 0xC0 的末值已从 channelProgram 清掉（不再谎报乐器名）");
        check(d.bendRangeEvents == 0 && d.bendRangeChannels == 0,
              "弯音范围诊断计数按界内重算，不打印「 半音」空值行");
        check(d.programChangeEvents == 0 && d.bankSelectEvents == 0,
              "音色库诊断计数按界内重算");
        check(bendRangeLabel(d).empty() && bankLabel(d).empty(),
              "没有界内设定时两条残缺诊断行都不出现");
        const std::string lab = truncationLabel(d);
        check(lab.find("时间轴跨度异常") != std::string::npos &&
              lab.find("已丢弃") != std::string::npos,
              "页眉如实提示已丢弃越界事件: " + lab);
        check(lab.find("已截断") == std::string::npos,
              "没有越界音符时不误报已截断音符: " + lab);
    }
    {
        // 只有越界音符、没有越界点事件：提示必须与 v1.85 逐字节一致
        // （huge_delta.mid 等既有素材零回归的守护）。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(480, ev({0x80, 60, 0}));
        mel.at(30000000L, ev({0x90, 72, 90}));
        mel.at(30000480L, ev({0x80, 72, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "纯越界音符 SMF 解析成功");
        check(d.timelineClamped >= 1 && d.timelineEventsDropped == 0,
              "纯音符越界不计点事件丢弃");
        const std::string lab = truncationLabel(d);
        check(lab.find("已截断") != std::string::npos &&
              lab.find("丢弃") == std::string::npos,
              "纯音符越界的提示与 v1.85 一致: " + lab);
    }
    {
        // 正常文件：点事件一个都不丢，提示里没有时间轴字样。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(960, ev({0xFF, 0x06, 0x05, 'V', 'e', 'r', 's', 'e'}));
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(480, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "正常段落 SMF 解析成功");
        check(d.timelineEventsDropped == 0 && d.markers.size() == 1,
              "正常文件的界内 marker 不被误丢");
        check(truncationLabel(d).find("时间轴") == std::string::npos,
              "正常文件提示不含时间轴字样");
    }

    // ---- v1.87.0 解析期消费的事件也要过同一道时间轴闸 ----
    //
    // v1.86 的 dropStrayEvents() 只看得见 out 里的事件表。踏板 / CC7 /
    // SysEx 鼓模式 / Bank / FF21 却在解析循环里就被消费掉：有的只留下一个
    // int 计数器，有的只存在于 parseBuffer 的局部 vector。畸形 delta 把
    // 「抬踏板」推到 3000 万 tick 时，buildPedalRanges() 会拿界内的「踩下」
    // 配出横跨全曲的区间，把 1 小节 4 个音补成幽灵长音（HTML 铺 256 个
    // 空小节）；boundTimeline() 事后再夹也收不回补出来的空小节。
    std::cout << "\n测试解析期事件过时间轴闸\n";
    {
        const long kFar = 30000000L;
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(0, ev({0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}));
        cond.at(0, ev({0xB0, 0x40, 127}));       // 界内真实踩下
        cond.at(kFar, ev({0xB0, 0x40, 0}));     // 坏 delta 把抬起推到世界尽头
        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            mel.at(i * 480L, ev({0x90, 60 + i, 90}));
            mel.at(i * 480L + 480, ev({0x80, 60 + i, 0}));
        }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "含越界抬踏板的 SMF 仍能解析");
        check(d.notes.size() == 4, "4 个界内音符一个不少, 实际: " +
              std::to_string(d.notes.size()));
        check(d.timelineClamped == 0,
              "踏板不再造出需要截断的幽灵长音, 实际: " +
              std::to_string(d.timelineClamped));
        long maxEnd = 0;
        for (const auto& n : d.notes)
            maxEnd = std::max(maxEnd, n.startTicks + n.durationTicks);
        check(maxEnd <= 1920L,
              "音符末端仍收在 1 小节内, 实际: " + std::to_string(maxEnd));
        check(d.timelineEventsDropped >= 1, "越界抬起被如实丢弃");
        check(d.pedalEvents == 1, "踏板计数按界内重算, 实际: " +
              std::to_string(d.pedalEvents));
        check(controllerLabel(d).find("1 个踏板事件") != std::string::npos,
              "延音踏板诊断只报界内那一次踩下: " + controllerLabel(d));
        const std::string lab = truncationLabel(d);
        check(lab.find("已丢弃") != std::string::npos &&
              lab.find("已截断") == std::string::npos,
              "只提示丢弃越界事件、不误报截断音符: " + lab);
    }
    {
        // 解析期直接 ++ 的 5 类诊断计数：全部落在世界尽头时不能留痕。
        const long kFar = 30000000L;
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(0, ev({0xC0, 0}));                            // 界内：钢琴
        cond.at(kFar, ev({0xF0, 0x0B, 0xF0, 0x41, 0x10, 0x42, 0x12,
                          0x40, 0x11, 0x15, 0x01, 0x41, 0xF7}));  // GS part mode
        cond.at(kFar, ev({0xB0, 0x07, 100}));                // CC7
        cond.at(kFar, ev({0xB0, 0x00, 0}));                  // Bank MSB
        cond.at(kFar, ev({0xB0, 0x20, 0}));                  // Bank LSB
        cond.at(kFar, ev({0xC0, 40}));                        // 越界：小提琴
        cond.at(kFar, ev({0xFF, 0x20, 0x01, 0x03}));         // Channel Prefix
        cond.at(kFar, ev({0xFF, 0x21, 0x01, 0x01}));         // MIDI Port
        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            mel.at(i * 480L, ev({0x90, 60 + i, 90}));
            mel.at(i * 480L + 480, ev({0x80, 60 + i, 0}));
        }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "含越界解析期事件的 SMF 仍能解析");
        check(d.notes.size() == 4, "4 个界内音符一个不少");
        check(d.sysExEvents == 0 && d.sysExModeEvents == 0 &&
              d.sysExDrumChannels == 0, "SysEx 计数不含幽灵设定");
        check(sysExLabel(d).empty(), "页眉不出现「SysEx 鼓模式」幽灵行: " +
              sysExLabel(d));
        check(d.midiPortEvents == 0 && d.midiPortCount <= 1 &&
              d.channelPrefixEvents == 0, "端口 / 通道前缀计数不含幽灵声明");
        check(portLabel(d).empty(), "页眉不出现「MIDI Port」幽灵行: " +
              portLabel(d));
        check(d.cc7Dynamics == 0 &&
              controllerLabel(d).find("CC7") == std::string::npos,
              "CC7 力度点不凭空补入");
        check(d.bankSelectEvents == 0 && bankLabel(d).empty(),
              "音色库诊断计数不含越界 Bank Select");
        check(d.channelProgram.count(0) == 1 && d.channelProgram.at(0) == 0,
              "channelProgram 末值没被越界 0xC0 污染成小提琴");
        check(d.timelineEventsDropped >= 5,
              "记录到多类越界解析期事件, 实际: " +
              std::to_string(d.timelineEventsDropped));
    }
    {
        // 同一批事件全放在界内：一个都不能丢，这是零回归的守护。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(0, ev({0xC0, 0}));
        cond.at(0, ev({0xF0, 0x0B, 0xF0, 0x41, 0x10, 0x42, 0x12,
                       0x40, 0x11, 0x15, 0x01, 0x41, 0xF7}));
        cond.at(0, ev({0xB0, 0x07, 100}));
        cond.at(0, ev({0xB0, 0x00, 0}));
        cond.at(0, ev({0xB0, 0x20, 0}));
        cond.at(0, ev({0xB0, 0x40, 127}));
        cond.at(960, ev({0xB0, 0x40, 0}));
        cond.at(0, ev({0xFF, 0x20, 0x01, 0x03}));
        cond.at(0, ev({0xFF, 0x21, 0x01, 0x01}));
        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            mel.at(i * 480L, ev({0x90, 60 + i, 90}));
            mel.at(i * 480L + 480, ev({0x80, 60 + i, 0}));
        }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "界内解析期事件 SMF 解析成功");
        check(d.timelineEventsDropped == 0 && d.timelineClamped == 0,
              "界内事件一个都不丢");
        check(d.sysExEvents == 1 && d.sysExModeEvents == 1,
              "界内 SysEx 鼓模式仍被统计");
        check(d.midiPortEvents == 1 && d.channelPrefixEvents == 1,
              "界内端口 / 通道前缀仍被统计");
        check(d.bankSelectEvents == 2 && d.cc7Dynamics == 1,
              "界内 Bank Select / CC7 仍被统计");
        check(d.pedalEvents == 2, "界内踏板事件仍被统计");
        check(truncationLabel(d).find("时间轴") == std::string::npos,
              "界内事件不误报时间轴异常");
    }

    // ---- v1.88.0 时间轴护栏收口 ----
    //
    // v1.85~1.87 的三道闸都盯住「畸形 delta 造成的巨额 tick」，却留下三个
    // 收口问题：① cap 定成 6000 小节本身太大，一个完全合法的极晚长音能让
    // barGridOf() 铺 6000 个空小节（HTML 2.46 MB）；② 同一个音符命中两处
    // 夹取时 timelineClamped 加两次，页眉写「已截断 2 个」但谱里只有 1 音；
    // ③ boundTimeline() 在 notes.empty() 时整函数早退，空谱文件的点事件
    // 不过闸，--list-tracks 会数出幽灵歌词。
    std::cout << "\n测试时间轴护栏收口（v1.88）\n";
    {
        // 1) 上限收紧：极晚的合法长音整条丢弃，4 个正常音照常出谱。
        const long kCap = 512L * 1920L;
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        SmfTrack mel;
        for (int i = 0; i < 4; i++) {
            mel.at(i * 480L, ev({0x90, 60 + i, 90}));
            mel.at(i * 480L + 480, ev({0x80, 60 + i, 0}));
        }
        const long late = 5900L * 1920L;   // 旧 cap 内、新 cap 外，且 VLQ 完全合法
        mel.at(late, ev({0x90, 72, 90}));
        mel.at(late + 480, ev({0x80, 72, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "极晚长音 SMF 仍能解析");
        check(d.notes.size() == 4,
              "极晚长音被丢弃，4 个正常音照常出谱, 实际: " + std::to_string(d.notes.size()));
        long maxEnd = 0;
        for (const auto& n : d.notes)
            maxEnd = std::max(maxEnd, n.startTicks + n.durationTicks);
        check(maxEnd < kCap,
              "谱面末端收在 512 小节内（输出层不再铺几千空小节）, 实际: " +
              std::to_string(maxEnd));
        check(d.timelineClamped == 1,
              "极晚长音只计一次截断, 实际: " + std::to_string(d.timelineClamped));
        check(truncationLabel(d).find("已截断 1 个越界音符") != std::string::npos,
              "页眉如实提示已截断 1 个: " + truncationLabel(d));
    }
    {
        // 2) 一个音符同时命中「时长 > 256 小节」与「尾巴越轴」两处夹取。
        const long kCap = 512L * 1920L;
        const long start = 500L * 1920L;
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        SmfTrack mel;
        mel.at(start, ev({0x90, 60, 90}));
        mel.at(start + 300L * 1920L, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "双重夹取样本 SMF 解析成功");
        check(d.notes.size() == 1,
              "两处夹取命中的仍是同一个音, 实际: " + std::to_string(d.notes.size()));
        check(d.timelineClamped == 1,
              "按音符计一次截断（旧版计 2）, 实际: " + std::to_string(d.timelineClamped));
        check(d.notes[0].durationTicks == kCap - start,
              "尾巴夹到时间轴上限, 实际: " + std::to_string(d.notes[0].durationTicks));
        check(truncationLabel(d).find("已截断 1 个越界音符") != std::string::npos,
              "页眉不再谎报 2 个越界音符: " + truncationLabel(d));
    }
    {
        // 3) 空谱文件：三道闸照样要跑。--list-tracks 在「未提取到音符」
        //    return 3 之前执行，读的正是这些事件表。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(0, ev({0xFF, 0x06, 0x05, 'I', 'n', 't', 'r', 'o'}));      // 界内 marker
        cond.at(30000000L, ev({0xFF, 0x05, 0x01, 'x'}));                  // 越界歌词
        cond.at(30000000L, ev({0xFF, 0x06, 0x05, 'G', 'h', 'o', 's', 't'}));// 越界 marker
        cond.at(30000000L, ev({0xC0, 40}));                                // 越界换音色
        cond.at(30000000L, ev({0xB0, 0x0B, 100}));                         // 越界 CC11
        SmfTrack emptyTrack;
        emptyTrack.at(0, ev({0xFF, 0x03, 0x05, 'V', 'o', 'c', 'a', 'l'}));
        auto bytes = buildSmf(2, 480, {cond, emptyTrack});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "空谱 SMF 解析成功");
        check(d.notes.empty(), "样本确实不含音符");
        check(d.midiLyrics.empty(),
              "越界歌词被清掉（--list-tracks 不再报幽灵歌词）, 实际 " +
              std::to_string(d.midiLyrics.size()) + " 条");
        check(d.markers.size() == 1,
              "只剩界内 Intro 段落, 实际: " + std::to_string(d.markers.size()));
        check(d.dynamics.empty(), "越界 CC11 力度点被清掉");
        check(d.channelProgram.empty(),
              "空谱里越界 0xC0 的末值也被重建清掉");
        check(d.timelineEventsDropped >= 3,
              "空谱的点事件丢弃也计入, 实际: " + std::to_string(d.timelineEventsDropped));
        check(truncationLabel(d).find("已丢弃") != std::string::npos,
              "空谱也如实提示已丢弃: " + truncationLabel(d));
    }
    {
        // 4) 正常文件零变化：这道闸一个字节都不该动。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}));
        cond.at(960, ev({0xFF, 0x06, 0x05, 'V', 'e', 'r', 's', 'e'}));
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(480, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d), "正常 SMF 解析成功");
        check(d.timelineClamped == 0 && d.timelineEventsDropped == 0,
              "正常文件不计任何截断 / 丢弃（零回归）");
        check(d.markers.size() == 1 && d.notes.size() == 1,
              "正常文件的 marker 与音符一个不少");
        check(truncationLabel(d).find("时间轴") == std::string::npos,
              "正常文件提示不含时间轴字样");
    }

    // ---- v1.89.0 时间轴护栏与曲首拍号解耦 ----
    //
    // v1.88 把上限收到 512 小节，却仍拿「曲首拍号」反算 barLen：
    //   * 曲首 1/16 -> cap 只有 61440 tick（32 个 4/4 小节），从第 2 小节
    //     回到 4/4 的正常长曲后半被整条丢弃，页眉谎报「疑似损坏的 delta-time」；
    //   * 曲首 255/1 -> cap 2.5 亿 tick，输出层按 489600 tick 的 barLen
    //     给每小节铺列，再变 1/256 就铺出 20000 小节 / 49 MB HTML。
    // 现在 cap 固定按 4/4 基准小节算（解析层与 barGridOf() 共用），
    // 0x58 里音乐上不存在的拍号不再进 timeSigChanges。
    std::cout << "\n测试时间轴护栏与拍号解耦（v1.89）\n";
    {
        // 1) 基准函数只认 division，不认拍号。
        check(timelineCapTicks(480) == 512L * 1920L,
              "480 tpqn 的 cap = 512 个 4/4 小节");
        check(timelineCapTicks(240) == 512L * 960L,
              "240 tpqn 的 cap 按 division 等比缩放");
        check(timelineCapTicks(0) == 512L * 1920L,
              "非法 division 回退 480 后再算 cap");
    }
    {
        // 2) 曲首 1/16 + 第 2 小节回 4/4：40 小节 160 个音一个不少。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x01, 0x04, 0x18, 0x08}));   // 1/16
        cond.at(120, ev({0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08})); // 4/4
        SmfTrack mel;
        for (int b = 0; b < 40; b++)
            for (int i = 0; i < 4; i++) {
                long t = b * 1920L + i * 480L;
                mel.at(t, ev({0x90, (uint8_t)(60 + i), 90}));
                mel.at(t + 480, ev({0x80, (uint8_t)(60 + i), 0}));
            }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "短拍号长曲 SMF 解析成功");
        check(d.notes.size() == 160,
              "160 个正常音符全保留（旧版只剩 128）, 实际: " +
              std::to_string(d.notes.size()));
        check(d.timelineClamped == 0 && d.timelineEventsDropped == 0,
              "短拍号长曲不计任何截断 / 丢弃");
        check(d.timeSigChanges.size() == 2 && d.timeSig.toString() == "1/16",
              "合法的 1/16 与 4/4 变拍号都照常保留");
        check(truncationLabel(d).find("时间轴") == std::string::npos,
              "不再谎报「时间轴跨度异常 / 疑似损坏的 delta-time」: " +
              truncationLabel(d));
    }
    {
        // 3) 255/1 与 1/256 都被挡掉，长音夹到 256 小节，铺表有界。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0xFF, 0x00, 0x18, 0x08}));      // 255/1
        cond.at(489600L, ev({0xFF, 0x58, 0x04, 0x01, 0x08, 0x18, 0x08}));// 1/256
        SmfTrack mel;
        mel.at(0, ev({0x90, 60, 90}));
        mel.at(250000000L, ev({0x80, 60, 0}));
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "离谱拍号 SMF 解析成功");
        check(d.timeSigChanges.empty(),
              "255/1 与 1/256 都不进拍号表, 实际: " +
              std::to_string(d.timeSigChanges.size()));
        check(d.timeSig.toString() == "4/4",
              "无合法拍号时回退 4/4, 实际: " + d.timeSig.toString());
        long maxDur = 0;
        for (const auto& n : d.notes) maxDur = std::max(maxDur, n.durationTicks);
        check(maxDur <= 512L * 1920L,
              "长音夹在固定 cap 内, 实际: " + std::to_string(maxDur));
        check(barGridOf(d).starts.size() < 600,
              "输出层铺表有界（旧版撞 20000 上限）, 实际: " +
              std::to_string(barGridOf(d).starts.size()));
    }
    {
        // 4) 真实拍号一个都不许误伤：1/16 是下界，3/8 / 6/8 / 12/8 / 5/4 照常。
        const uint8_t meters[][2] = {{1, 4}, {3, 3}, {6, 3}, {12, 3}, {5, 2}};
        const char* labels[] = {"1/16", "3/8", "6/8", "12/8", "5/4"};
        for (int i = 0; i < 5; i++) {
            SmfTrack cond;
            cond.at(0, ev({0xFF, 0x58, 0x04, meters[i][0], meters[i][1],
                           0x18, 0x08}));
            SmfTrack mel;
            mel.at(0, ev({0x90, 60, 90}));
            mel.at(480, ev({0x80, 60, 0}));
            auto bytes = buildSmf(2, 480, {cond, mel});
            MidiData d;
            check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
                  std::string("拍号 ") + labels[i] + " 样本解析成功");
            check(d.timeSigChanges.size() == 1 &&
                  d.timeSig.toString() == labels[i],
                  std::string("拍号 ") + labels[i] + " 被保留, 实际: " +
                  d.timeSig.toString());
        }
    }
    {
        // 5) 全程 3/8 的短拍号曲目也不误杀（cap 与拍号无关的直接体现）。
        SmfTrack cond;
        cond.at(0, ev({0xFF, 0x58, 0x04, 0x03, 0x03, 0x18, 0x08}));
        SmfTrack mel;
        for (int i = 0; i < 24; i++) {
            mel.at(i * 240L, ev({0x90, 60, 90}));
            mel.at(i * 240L + 240, ev({0x80, 60, 0}));
        }
        auto bytes = buildSmf(2, 480, {cond, mel});
        MidiData d;
        check(loadMidiFromMemory(bytes.data(), bytes.size(), d),
              "全程 3/8 曲目解析成功");
        check(d.notes.size() == 24 && d.timelineClamped == 0,
              "3/8 曲目 24 个音全保留, 实际: " + std::to_string(d.notes.size()));
    }
    {
        // ---- v1.90.0：HTML 页眉 meta 行逐段转义 ----
        // 轨名 / marker / 歌词来源都是 MIDI 文件内容。旧版页眉只给
        // keys.summary() 转义，其余片段裸拼，一份 0x03 轨名写成
        // <script>alert(1)</script> 的合法 SMF 就能注入可执行 JS。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[1] = "<script>alert(1)</script> A&B <C>";
        for (int i = 0; i < 4; i++) {
            NoteEvent e;
            e.track = 1; e.channel = 0; e.startTicks = i * 480L;
            e.durationTicks = 480; e.pitch = 60 + i * 2; e.velocity = 90;
            d.notes.push_back(e);
        }
        const KeyEstimate k = estimateKey(d);
        const std::string h = renderHtml(d, k, "<title>&</title>");
        check(h.find("<script>alert(1)</script>") == std::string::npos,
              "页眉不再出现可执行的裸 <script> 注入");
        check(h.find("&lt;script&gt;alert(1)&lt;/script&gt;")
              != std::string::npos,
              "轨名在页眉被转义成实体");
        const size_t ms = h.find("<div class=\"meta\">");
        check(ms != std::string::npos, "页眉 meta 段存在");
        const size_t me = h.find("</div>", ms);
        check(me != std::string::npos, "页眉 meta 段闭合");
        const std::string meta = h.substr(ms, me - ms);
        check(meta.find("<script") == std::string::npos &&
              meta.find("<C>") == std::string::npos &&
              meta.find("<b>") == std::string::npos,
              "meta 段里没有任何裸 HTML 标签");
        check(meta.find("A&amp;B") != std::string::npos,
              "页眉里的 & 被转义");
        check(meta.find("&amp;amp;") == std::string::npos,
              "keys.summary() 没有被双重转义");
        check(h.find("<h1>&lt;title&gt;&amp;&lt;/title&gt;</h1>")
              != std::string::npos,
              "title 参数仍走既有的 <h1> 转义");
        check(h.find("<h3>&lt;script&gt;alert(1)&lt;/script&gt;")
              != std::string::npos,
              "谱面主体 <h3> 的转义口径与页眉一致");
        check(h.find("<script") == std::string::npos,
              "未开 --player 时整份页面没有 script 标签");
        const std::string txt = formatJianpu(d, k, false);
        check(txt.find("<script>alert(1)</script>") != std::string::npos,
              "文本谱是纯文本，保留原始轨名不转义");
        const std::string ph = renderHtml(d, k, "t", nullptr, true);
        check(occurrences(ph, "<script") == 1,
              "--player 时页面只有播放器自己的那一段 script");
    }
    {
        // ---- v1.95.0：MusicXML 同样必须逐段转义恶意轨名/歌词 ----
        // HTML 页眉的注入转义在 v1.90.0 已锁死（上面那段），但 MusicXML 的
        // <part-name> / 歌词 <text> 是另一条输出通道，历史上没有确定性单测，
        // 只有 fuzz 的「XML 能解析」顺带兜住。若哪天 xmlEscape 被改坏，裸
        // <script> 进 <part-name> 会让整份 MusicXML 非良构（MuseScore 打不开）。
        // 这里把同一条恶意轨名 + 恶意歌词喂进 renderMusicXml（歌词走 MIDI
        // 内置通道，lyricLines 传 nullptr 回落到 data.midiLyrics），断言：
        //   · 原始 <script>alert(1)</script> / <script>x</script> 在整份 XML 里绝不可见；
        //   · 转义后实体 (&lt; &gt; &amp;) 出现，裸标签一个都不留；
        //   · <part-name> 段内没有任何裸 HTML 标签，口径与 HTML 页眉一致。
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[1] = "<script>alert(1)</script> A&B <C>";
        for (int i = 0; i < 4; i++) {
            NoteEvent e;
            e.track = 1; e.channel = 0; e.startTicks = i * 480L;
            e.durationTicks = 480; e.pitch = 60 + i * 2; e.velocity = 90;
            d.notes.push_back(e);
        }
        const KeyEstimate k = estimateKey(d);
        const std::string x = renderMusicXml(d, k);
        check(x.find("<script>alert(1)</script>") == std::string::npos,
              "MusicXML 里不得出现裸 <script> 注入（轨名）");
        check(x.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos,
              "轨名在 <part-name> 被转义成实体");
        check(x.find("A&amp;B") != std::string::npos,
              "MusicXML 里的 & 被转义");
        check(x.find("&amp;amp;") == std::string::npos,
              "MusicXML 没有被双重转义");
        check(x.find("&lt;C&gt;") != std::string::npos,
              "MusicXML 里的 <C> 被转义");
        const size_t pn = x.find("<part-name>");
        check(pn != std::string::npos, "MusicXML <part-name> 存在");
        const size_t pne = x.find("</part-name>", pn);
        check(pne != std::string::npos, "MusicXML <part-name> 闭合");
        const std::string name = x.substr(pn, pne - pn);
        check(name.find("<script") == std::string::npos &&
              name.find("<C>") == std::string::npos &&
              name.find("<b>") == std::string::npos,
              "part-name 段内没有任何裸 HTML 标签");
    }

    // ---- v1.97.0 弱起矛盾选项（纯函数，与 validateKeyOptions 同口径） ----
    {
        using m2j::cli::validatePickupOptions;

        check(validatePickupOptions(false, -1).empty(),
              "两个弱起开关都不给：放行（走自动判定）");
        check(validatePickupOptions(true, -1).empty(),
              "只给 --no-pickup：放行");
        check(validatePickupOptions(false, 1).empty(),
              "只给 --pickup 1：放行");
        check(validatePickupOptions(false, 0).empty(),
              "只给 --pickup 0：放行（正数校验在 parseBeats 另拒）");
        check(!validatePickupOptions(true, 1).empty(),
              "--pickup 1 + --no-pickup 必须拒绝");
        check(!validatePickupOptions(true, 99).empty(),
              "--pickup 99 + --no-pickup 必须拒绝（不得绕过越界校验）");
        check(!validatePickupOptions(true, 1.5).empty(),
              "小数拍数的矛盾组合同样拒绝");
        const std::string e = validatePickupOptions(true, 1);
        check(e.find("--pickup") != std::string::npos &&
              e.find("--no-pickup") != std::string::npos,
              "报错文案点名两个冲突选项");
        check(e.find("只保留一个") != std::string::npos,
              "报错文案给出修复指引");
    }

    // ---- v1.98.0：排版/显示类开关不得改变 PLAY JSON（C++ 层钉死） ----
    // 播放数据取自「谱面上真正画出来的那份音符」+ tempo 表，与每行几小节、
    // 要不要画力度记号、歌词来自哪里都无关。此前 check_html_player.py 只审
    // 「--html --player」一种 flagset，--bars / --no-dynamics 叠加时的页面从
    // 没被确定性验证过。这里把 barsPerRow 的全边界逐页提取 PLAY JSON 比对，
    // 一旦有人把排版状态渗进播放数据（例如让 withTick 改写时长），立刻变红。
    // 注意 --no-quantize 不在此契约内：它改的就是音符时值，PLAY 必须跟着变。
    {
        MidiData d;
        d.division = 480;
        d.timeSig.valid = true;
        d.trackNames[1] = "Piano";
        for (int i = 0; i < 4; i++) {
            NoteEvent e;
            e.track = 1; e.channel = 0; e.startTicks = i * 480L;
            e.durationTicks = 480; e.pitch = 60 + i * 2; e.velocity = 90;
            d.notes.push_back(e);
        }
        const KeyEstimate k = estimateKey(d);
        auto playOf = [](const std::string& h) -> std::string {
            const std::string tag = "var M2J_PLAY = ";
            const size_t s = h.find(tag);
            if (s == std::string::npos) return {};
            const size_t e = h.find(";\n", s);
            if (e == std::string::npos) return {};
            return h.substr(s + tag.size(), e - s - tag.size());
        };
        const std::string ref = playOf(renderHtml(d, k, "t", nullptr, true, 0));
        check(!ref.empty() && ref.find("\"notes\"") != std::string::npos,
              "--player 页面含可提取的 PLAY JSON（notes 段）");
        const int barsList[] = {1, 2, 4, 8};
        for (int bars : barsList) {
            const std::string h = renderHtml(d, k, "t", nullptr, true, bars);
            check(playOf(h) == ref,
                  "--bars N 不得改变 PLAY JSON（N=" + std::to_string(bars) + "）");
        }
        const std::string plain = renderHtml(d, k, "t", nullptr, false, 0);
        check(plain.find("M2J_PLAY") == std::string::npos,
              "不开 --player 时整页没有 M2J_PLAY");
    }
    // ---- v1.99.0 重复带值选项 + 外部歌词源文件保护（纯字符串） ----
    {
        using m2j::cli::validateDuplicateValueOptions;
        using m2j::cli::validateOutputPaths;
        using m2j::cli::OutputPath;

        const std::string in = "song.mid";
        check(validateDuplicateValueOptions({}).empty(),
              "重复选项预检：空参数列表放行");
        check(validateDuplicateValueOptions({in}).empty(),
              "重复选项预检：只有输入文件时放行");
        check(validateDuplicateValueOptions(
                  {in, "--player", "--player"}).empty(),
              "重复选项预检：开关类重复是幂等的，放行");
        check(validateDuplicateValueOptions(
                  {in, "--output", "a.txt", "--html", "b.html",
                   "--musicxml", "c.xml"}).empty(),
              "重复选项预检：三个不同输出选项共存放行");
        check(validateDuplicateValueOptions(
                  {in, "--transpose", "-2", "--bars", "2"}).empty(),
              "重复选项预检：合法负数值不会被误判成新选项");
        for (const char* opt : {"--html", "--musicxml", "--output", "--key",
                                "--gong", "--track", "--transpose", "--lyrics",
                                "--pickup", "--bars", "--soft-pedal-scale"}) {
            const std::vector<std::string> a = {in, opt, "one", opt, "two"};
            const std::string e = validateDuplicateValueOptions(a);
            check(!e.empty() && e.find(opt) == 0 &&
                  e.find("重复") != std::string::npos,
                  std::string(opt) + " 重复给时拒绝并点明重复");
        }
        check(validateDuplicateValueOptions(
                  {"--html", "a.html", in, "--html", "b.html"}).size() > 0,
              "重复选项预检：两个 --html 之间夹着输入文件也能拦住");
        check(validateDuplicateValueOptions(
                  {"--key", "C", "--transpose", "-2", "--key", "D"}).size() > 0,
              "重复选项预检：负数值不遮挡后面的重复 --key");
        check(validateDuplicateValueOptions(
                  {"--bars", "2", "--no-dynamics", "--bars", "3"}).size() > 0,
              "重复选项预检：中间夹开关仍能拦住重复 --bars");
        check(validateOutputPaths(in, {{"--html", "a.html"}},
                                  {{"--lyrics", "a.lrc"}}).empty(),
              "源文件保护：歌词与输出路径不同放行");
        for (const char* opt : {"--output", "--html", "--musicxml"}) {
            const std::string e = validateOutputPaths(
                in, {{opt, "./WORDS.LRC"}}, {{"--lyrics", "words.lrc"}});
            check(!e.empty() && e.find(opt) == 0 &&
                  e.find("覆盖歌词源文件") != std::string::npos,
                  std::string(opt) + " 覆盖 --lyrics 源文件时拒绝");
        }
        check(validateOutputPaths(in, {{"--html", "a.html"}},
                                  {{"--lyrics", ""}}).empty(),
              "源文件保护：未指定 --lyrics 时放行");
        check(validateOutputPaths(in, {{"--html", "a.html"}}).empty(),
              "源文件保护：两参重载保持 v1.82 口径");
    }
    std::cout << "\n------------------------------------------------\n";
    std::cout << "共 " << total << " 项，失败 " << failures << " 项 -> "
              << (failures == 0 ? "全部通过" : "有失败") << "\n";
    return failures == 0 ? 0 : 1;
}
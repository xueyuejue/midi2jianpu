#pragma once
// midifile.hpp - 标准 MIDI 文件(SMF)解析
// 手写二进制解析，不依赖第三方库。提取音符事件、拍号、速度。
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace m2j {

// 单个音符事件
struct NoteEvent {
    double startSeconds;
    double durationSeconds;
    long startTicks = 0;      // 起始 tick（用于划分小节）
    long durationTicks = 0;    // 持续 tick
    int pitch;
    int channel;
    int track;
};

// 拍号（从 MIDI 0x58 meta 事件解析）
struct TimeSignature {
    int numerator = 4;     // 分子
    int denomPow = 2;      // 分母 = 2^denomPow（2=四分音符为一拍, 3=八分, 1=二分）
    bool valid = false;

    int beatsPerBar() const { return numerator; }
    double barLengthTicks(long division) const {
        double oneBeat = division * 4.0 / (1 << denomPow);
        return oneBeat * numerator;
    }
    std::string toString() const {
        return std::to_string(numerator) + "/" +
               std::to_string(1 << denomPow);
    }
};

// 调号（从 MIDI 0x59 meta 事件解析）
struct KeySignature {
    int sharpsFlats = 0;   // 升号数(正)或降号数(负), -7..+7
    bool isMinor = false;
    bool valid = false;
};

// 力度变化点（从 CC 0x0B Expression 解析）
// channel 用于把力度限定在所属轨道，避免伴奏的力度串到旋律轨
struct DynPoint {
    long tick;
    int value;
    int channel = 0;
};

// 速度变化点（从 0x51 set_tempo 解析）
struct TempoPoint {
    long tick;
    double bpm;  // 每分钟拍数
};

// 拍号变化点（从 0x58 time_signature 解析）
//
// 旧实现每次都用最后一次 0x58 覆盖 out.timeSig，于是「前 4/4、后 3/4」的曲子
// 整份谱子都按 3/4 划小节线，还连带把弱起判错。拍号和速度一样是一张 map，
// 必须按 tick 记全；timeSig 字段只表示「曲首拍号」，供不需要逐小节的近似计算用。
struct TimeSigPoint {
    long tick;
    TimeSignature sig;
};

// 调号变化点（从 0x59 key_signature 解析）
//
// 旧实现每读到一个 0x59 就覆盖 out.keySig，于是「前 1=C、副歌转 1=D」的
// 曲子整份谱子都按最后一个调号换算唱名：前半首的数字全错。调号和拍号、
// 速度一样是一张按 tick 排序的 map；keySig 字段只表示「曲首调号」。
struct KeySigPoint {
    long tick;
    KeySignature sig;
};

// 弯音轮事件（从 0xE0 解析，14位值，中心8192）
struct PitchBendPoint {
    long tick;
    int value;          // 0-16383, 8192=无弯音
    int channel = 0;    // 所属通道，避免不同乐器互相干扰
};

// 歌词事件（从 MIDI 0x05 lyric 解析）
//
// track 记下事件所在的轨道：format 1 的卡拉 OK 文件通常把歌词放在人声轨上，
// 这是「哪条轨是旋律」最硬的证据（比按音符数/单音率猜可靠得多）。
// format 0 只有一个轨道，这个字段恒为 0，下游要先确认存在多条轨才敢用。
struct LyricEvent {
    long tick;
    std::string text;
    int track = -1;
};

// 文本事件（从 MIDI 0x01 text 解析）
struct TextEvent {
    long tick;
    std::string text;
};

// 标记事件（从 MIDI 0x06 marker 解析）
struct MarkerEvent {
    long tick;
    std::string text;
};

// 解析结果
struct MidiData {
    int division = 480;
    std::vector<NoteEvent> notes;
    double tempo = 500000.0;
    TimeSignature timeSig;
    KeySignature keySig;
    std::vector<DynPoint> dynamics;
    std::vector<TempoPoint> tempoChanges;
    std::vector<TimeSigPoint> timeSigChanges;  // 0x58 全序列；timeSig = 曲首那一个
    std::vector<KeySigPoint> keySigChanges;    // 0x59 全序列；keySig = 曲首那一个
    std::vector<PitchBendPoint> pitchBends;
    double bendRange = 2.0;  // 弯音范围(半音), 默认±2, 可被RPN 0x00改变
    std::map<int, std::string> trackNames;  // 轨道名称(0x03 meta)
    std::vector<LyricEvent> midiLyrics;     // MIDI内置歌词(0x05 meta)
    std::vector<TextEvent> textEvents;      // 文本事件(0x01 meta)
    std::vector<MarkerEvent> markers;       // 标记事件(0x06 meta)

    // MIDI 通道 10（channel index 9）是打击乐专用通道：音高代表底鼓/军鼓/踩镲
    // 这些鼓件，而不是音级。简谱没有对应的记法，把它当旋律转出来只会得到
    // `1,, #4,, #6,,` 这种垃圾，并且会因为音符数最多而抢走「旋律轨」身份。
    // 所以解析阶段就丢弃，这里只留下数量供 CLI 提示用户。
    int drumNotesSkipped = 0;

    // 弱起（anacrusis）小节长度，单位 tick；-1 表示自动判定（见 notation.hpp
    // 的 detectPickupTicks），0 表示明确没有弱起。
    //
    // 大量歌本里的曲子是从弱拍进唱的（「| 5 5 | 1 - - - 」的 5 5），MIDI 里这些
    // 音就从 tick 0 开始，第一小节天然不满。若照 `tick / barLen` 划小节线，
    // 整条网格会整体左移，谱面上每一小节的重音都落到第 4 拍，小节线也全部错位。
    long pickupTicks = -1;

    // 是否应用中途转调（0x59 map）。CLI 的 --no-key-changes 把它关掉，
    // 但事件表本身保留——调号还要用来判曲首调性，清空数据会把两件事搅在一起。
    bool applyKeyChanges = true;

    // 是否启用「五声调式宫音校正」（见 notation.hpp 的 detectPentatonic）。
    // 只影响没有 0x59 调号、需要自动猜调的文件；CLI --no-pentatonic 关掉它，
    // 网页上是同名复选框。事件表本身不受影响。
    bool usePentatonic = true;
};

// ---- 小节网格 ----
// 有弱起时，bar 0 是那半截不完整的小节，完整小节从 1 起算；没有弱起时
// bar 0 就是第 1 小节。所有「tick -> 小节」的换算都必须走这三个函数，
// 否则弱起谱面会一半对一半错。
inline long barIndexAt(long tick, long barLen, long pickupTicks) {
    if (barLen <= 0) return 0;
    if (tick <= 0) return 0;
    if (pickupTicks <= 0) return tick / barLen;
    if (tick < pickupTicks) return 0;
    return 1 + (tick - pickupTicks) / barLen;
}

inline long barStartTick(long bar, long barLen, long pickupTicks) {
    if (bar <= 0) return 0;
    if (pickupTicks > 0) return pickupTicks + (bar - 1) * barLen;
    return bar * barLen;
}

inline long barEndTick(long bar, long barLen, long pickupTicks) {
    if (pickupTicks > 0) {
        if (bar <= 0) return pickupTicks;
        return pickupTicks + bar * barLen;
    }
    return (bar + 1) * barLen;
}

bool loadMidiFile(const std::string& path, MidiData& out);

// 从内存加载MIDI
bool loadMidiFromMemory(const uint8_t* data, size_t size, MidiData& out);

} // namespace m2j

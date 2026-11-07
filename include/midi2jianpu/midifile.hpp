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

    // note-on 力度与「量化前」的原始发声时长。
    // 演奏法（断音 / 重音）要靠这两个量判定：断音是 note-off 明显早于下一个
    // 音头，而 quantizeTiming() 会把门限衰减补回满时值；重音是力度显著高于
    // 邻音。两者都必须在量化之前留下的原始读数，否则谱面全变成连奏。
    int velocity = 96;        // 1..127
    long rawDurationTicks = 0;
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

// ---- 控制器事件（0xB0）----
// 过去 0xB0 分支只认 CC11（表情）与 RPN，其余控制器被当成「跳过两个字节」。
// 但有三类控制器直接决定谱面长什么样：
//   CC64 延音踏板：琴键抬手不等于消音，音要响到踏板抬起（或同键重新按下）；
//   CC120 / CC123：强制清音，是某些导出器代替 note-off 的写法；
//   CC7 主音量：与 CC11 一样是渐强渐弱的来源，只认 CC11 会整段丢掉力度。
// seq 是文件里的出现顺序；同一 tick 上多个事件要靠它定序（跨轨时 tick 相同）。
struct PedalPoint {
    long tick;
    int channel;
    bool down;      // CC64 值 >= 64 为踩下，< 64 为抬起
    int seq;
};
struct AllNotesOffPoint {
    long tick;
    int channel;
    int seq;
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

    // ---- 文件头 MThd 的三个字段 ----
    // 过去这一段是整块跳过的，于是两个「头字段级」的错误一直存在：
    //   1) SMPTE 时间基准（division 高字节为负）被当成无符号 PPQ 读成 59176，
    //      一首 16 拍的曲子被压进 1 小节，还被拆成 3 个假声部；
    //   2) format 2 的多条轨其实是**各自独立的序列**，合并到同一时间轴后
    //      两段音乐互相错位、同拍音被当成和弦。
    // format: 0=单轨，1=多轨同一首，2=多轨各自独立。
    int format = 1;
    int trackCount = 0;
    int rawDivision = 480;      // 文件里原始的 16 位 division（含 SMPTE 标志位）

    bool smpte = false;         // division 高字节为负 → SMPTE 时间基准
    int smpteFps = 0;           // 24 / 25 / 29（29.97 丢帧）/ 30
    int smpteTicksPerFrame = 0;

    // format 2 的取舍结果：共几段独立序列、实际转换的是哪一轨。
    int format2SequenceCount = 0;
    int format2UsedTrack = -1;

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
    std::map<int, int> channelProgram;      // 通道 -> GM 音色号(0..127)，0xC0 解析得到
    std::vector<LyricEvent> midiLyrics;     // MIDI内置歌词(0x05 meta)
    std::vector<TextEvent> textEvents;      // 文本事件(0x01 meta)
    std::vector<MarkerEvent> markers;       // 标记事件(0x06 meta)

    // 控制器语义修正的统计。解析完所有轨之后统一处理（踏板可能写在另一条轨上，
    // 逐轨即时处理会读乱时间顺序），这里记下「谱面被动过什么」，
    // 供 CLI / HTML 页眉 / WASM 三处如实说明。
    int pedalEvents = 0;           // CC64 事件数（踩下 + 抬起）
    int pedalExtendedNotes = 0;    // 因踏板补长的音符数
    int allNotesOffEvents = 0;     // CC120 / CC123 事件数
    int allNotesOffTrimmed = 0;    // 被清音事件截尾的音符数
    int cc7Dynamics = 0;           // 由 CC7 主音量补入力度表的点数

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

    // 是否把力度 / 断音 / 滑音 / 段落标记写进谱面。CLI --no-dynamics 关掉，
    // 网页上是「力度与演奏法」复选框。关掉只影响输出层，事件表仍完整保留。
    bool useDynamics = true;
};

// ---- 时间轴（tick <-> 秒）----
// 一段恒定速度的区间：从 tick 起、该点对应的真实秒数、以及本段的
// 每四分音符微秒数。按 tick 升序，segments[0].tick 恒为 0。
struct TempoSegment {
    long tick = 0;
    double seconds = 0.0;
    double usPerQuarter = 500000.0;
};

// 把 set_tempo 序列编译成一张可双向查询的时间轴。
//
// LRC 歌词的时间戳是「真实秒」，而谱面里一切都是 tick。原先 LRC 解析用
// 固定的 `60/bpm` 做秒 -> tick，变速曲目的后半段会整体错位（120→60 BPM
// 时两小节后能差出整整一小节）。音符秒数的换算早就按分段做了，但那段
// 逻辑藏在 recomputeSeconds 的局部 lambda 里，外面复用不到。现在抽出来，
// 让音符秒数、LRC 歌词、以及任何将来看时间的功能共用同一张表。
class TempoTimeline {
public:
    TempoTimeline() = default;  // 120 BPM，division 480
    TempoTimeline(const std::vector<TempoPoint>& changes, int division);
    explicit TempoTimeline(const MidiData& data);

    double tickToSeconds(long tick) const;   // 支持负 tick / 越过末段外推
    long secondsToTick(double seconds) const;  // 反解，四舍五入到整数 tick
    int changeCount() const { return (int)segments_.size() - 1; }  // 实质变速点数
    int division() const { return division_; }
    const std::vector<TempoSegment>& segments() const { return segments_; }

private:
    std::vector<TempoSegment> segments_{{0, 0.0, 500000.0}};
    int division_ = 480;
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

// keepAllFormat2 只对 format 2 文件有意义：默认只保留第一条含音符的序列
// （其余轨属别的曲子），传 true 则原样保留全部轨——CLI 的 --track 要靠它
// 才能选到第 2 段以后。
bool loadMidiFile(const std::string& path, MidiData& out, bool keepAllFormat2 = false);

// 从内存加载MIDI
bool loadMidiFromMemory(const uint8_t* data, size_t size, MidiData& out,
                        bool keepAllFormat2 = false);

// ---- 文件头的一句话说明（CLI 提示 / HTML 页眉 / WASM 共用）----
// 普通 PPQ 文件返回空串，调用方不必判断。
// SMPTE：「SMPTE 25 fps × 40 tick → 500 tick/拍（按 ♩=120 折算）」
std::string timeBaseLabel(const MidiData& data);
// format 2：「format 2（多个独立序列）：共 2 段，只转换第 2 轨「SongA」」
// 控制器语义提示：延音踏板 / 强制清音 / CC7 主音量各改了多少东西。
// 没有触发任何一项时返回空串。
std::string controllerLabel(const MidiData& data);

std::string formatLabel(const MidiData& data);

} // namespace m2j

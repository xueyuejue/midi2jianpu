#pragma once
// notation.hpp - 调性识别与音高->简谱转换
#include "midi2jianpu/midifile.hpp"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace m2j {

// 调性识别结果
struct KeyEstimate {
    int tonicPitchClass = 0;  // 0=C ... 11=B
    bool isMajor = true;
};

// 根据 MIDI 数据估计调性（优先用内置调号，否则 Krumhansl-Schmuckler 相关度匹配）
KeyEstimate estimateKey(const MidiData& data);

// 简谱调号的 do 音所在音级。
// 简谱用首调唱名：小调不写 1=主音，而是把关系大调的 do 记作 1，主音落在 6 上
// （la 为主音）。所以 a 小调的调号是 1=C，e 小调是 1=G。
int notationDoPitchClass(const KeyEstimate& key);

// 调号音名（已按首调换算），如 "C"、"Bb"；a 小调返回 "C"
std::string keyName(const KeyEstimate& key);

// 完整调性描述，如 "1=C (a小调)"、"1=G 大调"
std::string keyDescription(const KeyEstimate& key);

// 该调性是否习惯用降号记谱（调号含降号）
bool prefersFlats(const KeyEstimate& key);

// 调号升降数（-7..+7），供 MusicXML 等输出使用
int keyFifths(const KeyEstimate& key);

// 单个音高 -> 简谱片段（只含数字/升降/八度，不含时值）
std::string pitchToJianpu(int pitch, const KeyEstimate& key);

// 时值 tick -> 简谱时值后缀（延长线/下划线）
//   4拍="---" 3拍="--" 2拍="-" 附点四分="." 四分=""
//   附点八分="_." 八分="_" 附点十六="=." 十六="=" 三十二="%%"
std::string durationSuffix(long durationTicks, int division);

// 时值量化报告
struct QuantizeReport {
    int adjusted = 0;    // 被修正时值/位置的音符数
    long gridTicks = 0;  // 实际使用的音头网格（tick）
    double gate = 1.0;   // 估计出的门限因子（note-off 提前比例）
    bool changed() const { return adjusted > 0; }
};

// 把演奏 MIDI 的时值吸附到记谱网格。
//
// 真实录音转出的 MIDI 里，note-off 普遍早于下一个音头（门限时间 85% 左右是
// DAW 的常见默认值），直接拿时长查时值表会把八分音符判成附点十六分。这里做两件事：
//   1) 音头吸附到 1/24 拍网格（同时容纳 32 分音符与三连音）
//   2) 时长取「到下一个音头的时间」——当原时长接近该间隔时（>=70%），
//      说明只是门限衰减而非休止；否则按原时长吸附到最近的合法时值
// 会就地修改 data.notes 的 startTicks / durationTicks，seconds 保持不变。
QuantizeReport quantizeTiming(MidiData& data);

// 把音符列表格式化成文本简谱（含拍号、小节线、时值）
std::string formatJianpu(const MidiData& data, const KeyEstimate& key);

// MIDI 音号 -> 音名字符串 (60 -> "C4")
std::string midiNoteName(int pitch);

// 检测三连音: 返回每个音符是否属于三连音组
std::vector<bool> detectTriplets(const std::vector<NoteEvent>& notes, int division);

// 歌词字（对齐到音符）
struct LyricSyllable {
    long tick;      // 对齐的起始tick
    std::string text;  // 单字
};

// 歌词行（起始tick + 整行文本）
struct LyricLine {
    long startTick;
    std::string text;
};

// 解析歌词JSON文件，返回按tick排序的歌词字列表
std::vector<LyricSyllable> parseLyrics(const std::string& jsonPath, int division);

// 解析LRC歌词文件（[mm:ss.xx]歌词），返回按tick排序的歌词字列表
std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, int division, int bpm);

// 解析LRC为行列表（用于按音符对齐）
std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, int division, int bpm);

// 同 parseLrcLines，但输入是 LRC 文本而不是路径。
// WASM 版没有文件系统，只能走这个入口。
std::vector<LyricLine> parseLrcText(const std::string& lrc, int division, int bpm);

// ---- 歌词对齐结果 ----
// 一个字落在哪个音上、以及它「唱」了几个音。noteCount >= 2 就是一字多音
// （melisma）：HTML 要画连音线，MusicXML 要写 syllabic=begin/middle/end 与 <slur>。
// 这段判定原先只活在 HTML 渲染器里，导出 MusicXML 时整份歌词都丢了。
struct LyricPlacement {
    std::string text;       // 单字
    long tick = 0;          // 起始音头
    long spanEnd = 0;       // 覆盖区间右端（不含）：下一个字 / 下一行的起点
    int noteCount = 1;      // 区间内的音头数
    long lastNoteTick = 0;  // 区间内最后一个音头
};

struct LyricAssignment {
    // 音头 tick -> 该字的起始信息（只有起始音上有条目）
    std::map<long, LyricPlacement> at;
    // noteCount >= 2 的组，按 tick 升序
    std::vector<LyricPlacement> melismas;

    // 任意 tick 落在哪个一字多音组里：返回 melismas 下标 + 1，0 表示不在。
    // 传延长段（`-`）的 tick 也能命中，连音线才不会在长音中间断掉。
    int groupAt(long tick) const {
        for (size_t i = 0; i < melismas.size(); i++)
            if (tick >= melismas[i].tick && tick < melismas[i].spanEnd) return (int)i + 1;
        return 0;
    }
    bool empty() const { return at.empty(); }
};

// 把 LRC 行按「行内音数均匀分布」铺到旋律轨上。
// n > m（音多于字）时第 i 个字落在第 floor(i*n/m) 个音上，剩下的音由这个字
// 拖着唱；n <= m 时一音一字，多出来的字没有音可挂。
// mainTrack 由 pickMelodyTrack 给出；传 -1 或空行返回空结果。
// 三个输出层必须共用它，否则会出现「网页有连音线、导入 MuseScore 却没有词」。
LyricAssignment assignLyrics(const MidiData& data,
                             const std::vector<LyricLine>& lines,
                             int mainTrack);

// ---- MIDI 内置歌词（0x05 lyric meta） ----
// 和 LRC 是两种东西：LRC 只有「一行词从哪一刻开始」，字要靠行内音数均分；
// 内置歌词每个事件自带精确 tick，通常一个事件就是一个音节。把它转成 LyricLine
// 再走 assignLyrics，会把这份精确对齐抹平，还会把相邻音节粘成一个词。
// 所以单开一条通路，最终仍产出 LyricAssignment，输出层不必区分来源。
struct MidiSyllable {
    long tick = 0;
    std::string text;   // 已规整的单个音节（中文单字 / 英文单词）
    long spanEnd = 0;   // 本批音节共享的右端；0 = 直到曲终
};

// 规整 0x05 事件流。各家 DAW / 卡拉 OK 文件的事实约定：
//   "-"、"_"、空串、纯空白      一字多音的延续，不产生新音节（前一个字自然拖长）
//   "/"、"\"、CR/LF             换行或分隔，不产生新音节
//   "@" 开头                    @KMIDI / @TITLE 这类文件元数据，跳过
//   "+" 前缀                    续行标记，去掉后保留内容
// 中文按字拆、连续 ASCII 字母按词拆（"Hello" 是一个音节而不是五个），
// 标点与空白不占音节。同 tick 的多个事件合成一批，共享 spanEnd。
std::vector<MidiSyllable> normalizeMidiLyrics(const MidiData& data);

// 按精确 tick 对齐：一批音节吸附到 [tick, spanEnd) 内真实存在的旋律音头，
// 单音节批次就覆盖整段（这正是一字多音），多字批次在段内均分。
LyricAssignment assignMidiLyrics(const MidiData& data, int mainTrack);

// 内置歌词事件所在的轨道（format 1 且事件全在同一条有音符的轨上才可信）。
// format 0 只有一条轨、歌词分散在多轨、或歌词轨没有音符时都返回 -1。
int midiLyricTrack(const MidiData& data);

enum class LyricSource { None, External, Midi };
LyricSource lyricSourceOf(const MidiData& data, const std::vector<LyricLine>* external);

// 页眉/控制台用的来源说明，如 "歌词：MIDI 内置（12 字）"；无词时返回空串。
std::string lyricSourceLabel(const MidiData& data,
                             const std::vector<LyricLine>* external);

// 三个输出层唯一入口：显式给的 LRC 优先，没有就自动回退到 MIDI 内置歌词。
// 用户把 data.midiLyrics 清空（CLI 的 --no-midi-lyrics）即可彻底关掉回退。
LyricAssignment resolveLyrics(const MidiData& data,
                              const std::vector<LyricLine>* external,
                              int mainTrack);

// 和弦标记
struct ChordMark {
    long startTick;
    std::string name;  // 如 "C", "Am", "G7"
};

// 单条轨道的特征，供旋律轨判定使用
struct TrackProfile {
    int track = -1;
    size_t noteCount = 0;
    size_t onsetCount = 0;        // 不同音头的个数
    double polyphony = 1.0;       // 每个音头的平均发声音数（1 表示单声部）
    size_t distinctPitches = 0;
    int minPitch = 0;
    int maxPitch = 0;
    double avgPitch = 0;
    bool lyricAligned = false;    // 歌词行首能否落在该轨的音符上
    double score = 0;
};

// 统计每条轨道的特征
std::vector<TrackProfile> profileTracks(const MidiData& data,
                                       const std::vector<LyricLine>* lyrics = nullptr);

// 判定哪条轨道是旋律轨。
//
// 旧实现在四个地方各自写了一遍"音符最多的轨就是旋律轨"。真实编曲工程里音符最多
// 的那条轨几乎永远是鼓或柱式和弦伴奏，于是歌词和和弦标记全挂到伴奏上，主旋律
// 反而被当成伴奏。现在改成按证据强弱打分：
//   1) 歌词行首与音符对齐（最强证据）
//   2) 单音率——旋律是单声部，柱式和弦的复音数远大于 1
//   3) 音高丰富度与音域——贝斯也是单声部，但音域窄、音高种类少
//   4) 音符数只作微调
// 返回 -1 表示没有可用轨道。
int pickMelodyTrack(const MidiData& data, const std::vector<LyricLine>* lyrics = nullptr);

// 速度摘要（变速曲目会列出每次变化及其小节）
std::string tempoSummary(const MidiData& data);

// ---- 小节网格 ----
// 「tick -> 小节」的换算分散在文本 / HTML / MusicXML 三个输出里，弱起一进来
// 就得三处同时改，否则谱面一半对一半错。统一收进这个结构体。
//
// 后来又发现拍号也能中途变（4/4 转 3/4 再转 6/8），固定 barLen 除法同样全错。
// 所以网格不再是「一个常量 + 取模」，而是解析阶段就铺出一张逐小节的边界表：
// starts[i] 是第 i 小节起点 tick，sigs[i] 是这一小节生效的拍号。
struct BarGrid {
    long barLen = 1920;   // 曲首小节长度（无变拍号时全曲都是它）
    long pickup = 0;      // 弱起小节长度 tick（0 = 没有弱起）
    int beatsPerBar = 4;  // 曲首每小节拍数
    long tailBarLen = 1920;  // 边界表尾之后外推用的小节长

    std::vector<long> starts;            // 每个小节的起点 tick
    std::vector<TimeSignature> sigs;     // 每个小节生效的拍号（与 starts 等长）

    long barIndex(long tick) const {
        if (starts.empty()) return barIndexAt(tick, barLen, pickup);  // 兜底：老行为
        if (tick <= starts.front()) return 0;
        auto it = std::upper_bound(starts.begin(), starts.end(), tick);
        long idx = (long)(it - starts.begin()) - 1;
        if (it == starts.end()) {   // 超出表尾：按最后一段长度线性外推
            long tb = tailBarLen > 0 ? tailBarLen : barLen;
            if (tb > 0) idx += (tick - starts.back()) / tb;
        }
        return idx;
    }

    long barStart(long bar) const {
        if (starts.empty()) return barStartTick(bar, barLen, pickup);
        if (bar <= 0) return starts.front();
        if ((size_t)bar < starts.size()) return starts[(size_t)bar];
        long tb = tailBarLen > 0 ? tailBarLen : barLen;
        if (tb <= 0) tb = 1920;
        return starts.back() + (bar - (long)starts.size() + 1) * tb;
    }

    long barEnd(long bar) const {
        if (starts.empty()) return barEndTick(bar, barLen, pickup);
        return barStart(bar + 1);
    }

    // 谱面上印出的小节号：弱起小节不编号，第一个完整小节才是 1。
    int measureNumber(long bar) const { return pickup > 0 ? (int)bar : (int)bar + 1; }

    // 一小节的实际 tick 数（弱起小节、变拍号小节都可能和 barLen 不同）
    long barSpan(long bar) const { return barEnd(bar) - barStart(bar); }

    TimeSignature sigForBar(long bar) const {
        if (sigs.empty()) return TimeSignature{4, 2, true};
        size_t i = bar <= 0 ? 0 : (size_t)bar;
        return sigs[std::min(i, sigs.size() - 1)];
    }
    int beatsForBar(long bar) const {
        int n = sigForBar(bar).numerator;
        return n > 0 ? n : 4;
    }
    // 这一小节是否刚换了拍号（页眉标注、MusicXML 重写 <time> 都用它）
    bool meterChangedAt(long bar) const {
        if (bar <= 0 || sigs.size() < 2) return false;
        return sigForBar(bar - 1).toString() != sigForBar(bar).toString();
    }
    bool hasMeterChange() const {
        for (size_t i = 1; i < sigs.size(); i++)
            if (sigs[i].toString() != sigs[0].toString()) return true;
        return false;
    }
};

BarGrid barGridOf(const MidiData& data);

// ---- 调性 map（中途转调） ----
// 简谱是首调记谱：调一变，同一段旋律的唱名就得整体重算。旧实现全曲只有
// 一个 KeyEstimate，解析出来的 0x59 又只留最后一个，于是「前 1=C、副歌
// 转 1=D」的文件前半首数字全错。
//
// 和 BarGrid 一样的思路：把「当前调性」抽象成按 tick 查表的对象，三个输出
// 层不再各自持有常量。转调点一律吸附到所在小节的起点，避免一个小节里
// 前半用旧调、后半用新调（简谱歌本的转调标记都落在小节线上）。
struct KeyChange {
    long tick = 0;        // 生效 tick（已吸附到小节起点）
    KeyEstimate key;      // 该点起生效的调性（按 base 平移后的绝对调性）
    int delta = 0;        // 相对曲首首调 do 的半音偏移
    std::string label;    // "1=D"
};

class KeyMap {
public:
    KeyMap() = default;
    // data 提供 0x59 序列；base 是「曲首调性」，可以来自 estimateKey 的
    // 自动/MIDI 调号识别，也可以是用户 --key 手工指定。
    KeyMap(const MidiData& data, const KeyEstimate& base);

    KeyEstimate at(long tick) const;
    KeyEstimate keyAtBar(long bar, const BarGrid& grid) const {
        return at(grid.barStart(bar));
    }
    // 这一小节是否刚转调（页眉外的谱面标记、MusicXML 重写 <key> 都用它）
    bool keyChangedAtBar(long bar, const BarGrid& grid) const;

    bool hasChange() const { return !changes_.empty(); }
    const std::vector<KeyChange>& changes() const { return changes_; }
    KeyEstimate base() const { return base_; }

    // 页眉摘要，如 "1=C 大调 → 1=D(第5小节) → 1=F(第9小节)"；
    // 没有转调时就是 keyDescription(base)。
    std::string summary(const BarGrid& grid) const;

private:
    KeyEstimate base_;
    std::vector<KeyChange> changes_;
};

// 实质性的转调点（按 tick 排序，已去重复声明、去掉与当前生效调号相同的）。
// 空 = 全曲同一个调。
std::vector<KeySigPoint> keyChangesOf(const MidiData& data);

// 实质性的拍号变化（已按 tick 排序、去掉多轨重复声明、去掉曲首那一个）。
// 空 = 全曲同一个拍号。
std::vector<TimeSigPoint> meterChangesOf(const MidiData& data);

// 拍号变化摘要，如 "4/4 → 3/4(第3小节) → 6/8(第5小节)"；无变化时返回曲首拍号。
std::string meterSummary(const MidiData& data);

// 弱起（anacrusis）小节长度，单位 tick；0 表示判不出弱起。
//
// 判定只用硬证据，宁可漏判也不误判——判错的代价是整份谱子的小节线全错，
// 而漏判时用户还能用 --pickup N 手动指定：
//   1) 曲子必须从 tick 0 直接起唱。开头若干小节空白是前奏休止，不是弱起；
//   2) 全曲最后一个音的收尾必须落在小节线上。弱起的总长是 P + N*小节长，
//      于是「末音位置 mod 小节长」就是弱起长度，四舍五入到整拍；
//   3) 弱起之后那一拍必须真有音起，否则只是长音停在小节中间（最常见的误判）。
// 三条都成立时，弱起长度没有第二种解释，才敢用。
long detectPickupTicks(const MidiData& data);

// 实际要用的弱起长度：data.pickupTicks >= 0 时直接用它（手动覆盖），
// 否则走 detectPickupTicks（自动）。
long effectivePickupTicks(const MidiData& data);

// 弱起的人类可读说明，如 "弱起 1 拍"；没有弱起时返回空串。
// 文本谱头与 HTML 页眉共用，判错了用户一眼能看出来并去改 --pickup。
std::string pickupLabel(const MidiData& data);

// 和声窗长度（tick）：4/4 半小节、6/8 附点四分、3/4 整小节
long chordWindowTicks(const MidiData& data);

// 从MIDI数据中检测和弦（按和声窗，相邻同名合并）
std::vector<ChordMark> detectChords(const MidiData& data, const KeyEstimate& key);

} // namespace m2j

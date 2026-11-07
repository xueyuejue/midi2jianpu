#pragma once
// notation.hpp - 调性识别与音高->简谱转换
#include "midi2jianpu/midifile.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace m2j {

// 调性识别结果
struct KeyEstimate {
    int tonicPitchClass = 0;  // 0=C ... 11=B
    bool isMajor = true;
    // 非空表示识别到中国五声调式，如 "G 徵调式"。只用于页眉/MusicXML 的
    // 文字标注：简谱的唱名换算仍只由 tonicPitchClass + isMajor 决定，
    // 这样 KeyMap 转调、等音写法等既有链路完全不用改。
    std::string scaleLabel;
};

// ---- 中国五声调式（宫商角徵羽）----
// 值就是该调式主音相对宫音的唱名（宫=1、商=2、角=3、徵=5、羽=6）。
enum class PentatonicMode { None = 0, Gong = 1, Shang = 2, Jue = 3, Zhi = 5, Yu = 6 };

struct PentatonicEstimate {
    bool detected = false;
    PentatonicMode mode = PentatonicMode::None;
    int gongPitchClass = 0;    // 宫音，也就是简谱的 1
    int finalPitchClass = 0;   // 终止音（调式主音）
    double confidence = 0.0;   // 五声正音覆盖的时长比例，1.0 = 纯五声
    int distinctPitchClasses = 0;
    double outsideWeight = 0.0;  // 偏音（非本调五声正音）占的时长比例
    std::string label() const;   // "G 徵调式（宫音 C）"
};

// 五声调式判定：只看旋律轨的音级分布与终止音。
//
// Krumhansl-Schmuckler 用的是七声大/小调轮廓，遇到纯五声旋律会把 1 定错：
// C D E G A 且落在 G 上的 G 徵调式，KS 相关度最高的是 G 大调（1=G），
// 整份谱子的唱名就全错了。中国民歌/古风曲里「终止音不是宫音」是常态，
// 所以必须按「宫音 + 终止音」定调式，而不是硬套大小调主音。
// 七声（≥7 个音级）、偏音过多、终止音落在偏音上、宫音有歧义时一律拒绝，
// 交回 KS，避免把普通流行歌误判成五声。
PentatonicEstimate detectPentatonic(const MidiData& data);

std::string pentatonicModeName(PentatonicMode mode);

// 手动指定宫音（CLI --gong C / --gong Bb）时按当前终止音推出调式。
// 返回可直接交给三个输出层的 KeyEstimate，失败时返回空 label 的宫调式。
KeyEstimate keyFromGong(const MidiData& data, int gongPitchClass);

// 根据 MIDI 数据估计调性（优先用内置调号，否则 Krumhansl-Schmuckler 相关度匹配，
// 再由高置信的五声调式判定校正宫音）
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

// 同轨复调拆分：把一条轨的音符贪心分到若干「互不重叠」的声部，返回每个音符
// 的声部号（按入参下标，0 = 最高的那个声部）。
//
// 一条轨里有两个声部是很常见的：钢琴左右手导进同一个 MIDI 轨、把伴奏垫在
// 人声轨下面、或者踩延音踏板让上一个和弦还响着。若不分声部，这些重叠音会
// 被当成和弦塞进同一拍 —— 长音叠短音写成的 <chord/> 时值各不相同，小节总
// 时值直接溢出（4/4 的一小节写成 8 拍）。
//
// 判定分三步：
//   1. 同拍同值  -> 真和弦，并进同一声部（只有这时才该写 <chord/>）
//   2. 有空出来的声部（上一个音已收尾）-> 放进去
//   3. 都占着 -> 开新声部；到 maxVoices 上限就并进最早结束的那个，并把该音
//      截短到下一个音头（宁可少一个长音的尾巴，也不能让小节时值溢出）
//
// notes 会被就地修改（只有第 3 步的溢出情形会改动时值）。
//
// skip 用来排除「不是独立声部」的音（目前是倚音）：倚音时长极短，量化时
// 按门限还原会把它放大到压住主音，误判成第二声部。被 skip 的音一律归入
// 声部 0，且不参与声部状态推进。
std::vector<int> splitVoices(std::vector<NoteEvent>& notes, int maxVoices = 3,
                             const std::function<bool(const NoteEvent&)>& skip = nullptr);

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

// ---- 按 tempo map 对齐的 LRC 入口（推荐）----
// LRC 的时间戳是真实秒，谱面用 tick。上面三个 (division, bpm) 版本把整首歌
// 当成一个恒定速度，变速曲目的后半段歌词会整体错位（120→60 BPM 时能差出
// 一整个小节）。新重载接收 MidiData，用 TempoTimeline 逐段反解秒 -> tick。
// 旧签名保留给不关心变速的调用方，内部按曲首单速构造一条等价时间轴。
//
// 同时支持 LRC 的两项常见约定：
//   [offset:±毫秒]              所有时间戳整体平移（正=延后）
//   [mm:ss.xx][mm:ss.xx]同一行   一行词在多个时间点重复出现
std::vector<LyricSyllable> parseLrc(const std::string& lrcPath, const MidiData& data);
std::vector<LyricLine> parseLrcLines(const std::string& lrcPath, const MidiData& data);
std::vector<LyricLine> parseLrcText(const std::string& lrc, const MidiData& data);

// 实质变速点数（供 CLI / 网页提示「LRC 已按 N 处变速分段对齐」）。
// 走 tempoMarksOf 的折叠结果，所以一段渐慢只算一处，不是几十处。
int tempoChangeCount(const MidiData& data);

// ---- 速度标记（渐快渐慢的折叠）----
//
// DAW 里的渐慢 / 渐快在 MIDI 里不是一条曲线，而是**几十个连续的 set_tempo
// 事件**（自动化节点通常每 1/16 ~ 1 拍一个）。这些点对时间轴换算是必需的
// ——TempoTimeline 全靠它们才能算准秒数——但**不能一个个写进谱面**：
//
//   - 旧页眉取前 5 个变化点，一段渐慢直接变成
//     「♩=120 → 120(第3小节) → 116(第3小节) → 113(第3小节) → …」，
//     五条全指同一小节，读者拿不到任何信息；
//   - HTML 每个小节起点采样一次速度，渐慢段每小节都冒出一个 ♩=，
//     满屏都是没有意义的数字；
//   - MusicXML 反过来只在「tick 正好等于小节线」时才写 <metronome>，
//     而渐变节点几乎不可能落在小节线上，于是整段渐慢在导出的谱里**彻底消失**。
//
// 乐谱的正确写法只有一个记号：`rit.` / `accel.`。所以这里把事件序列收敛成
// 「谱面上该有的那几个记号」，时间轴仍用原始事件表精确换算。这就是本项目
// 一贯的分层：事件表是播放的真相，输出层只挑该显示的。
enum class TempoMarkKind {
    Step,          // 阶跃变速：♩=90
    Accelerando,   // 渐快：accel. A→B
    Ritardando,    // 渐慢：rit. A→B
};

struct TempoMark {
    long tick = 0;        // 生效位置（渐变取第一个节点的 tick）
    long endTick = 0;     // 渐变结束 tick；Step 时等于 tick
    int bpm = 0;          // 起点速度
    int toBpm = 0;        // 终点速度；Step 时等于 bpm
    TempoMarkKind kind = TempoMarkKind::Step;
    int pointCount = 1;   // 由多少个 set_tempo 事件收敛而来（供测试与诊断）
};

// 把 set_tempo 序列折叠成谱面标记。判据：
//   - 相邻两点间隔 <= 1 拍（division tick）算同一次速度变化；
//   - 一簇里 >=3 点、单调、且总跨度 >= 5% 才算渐变（渐快 / 渐慢）；
//   - 其余（单点阶跃、两点、来回抖动、跨度太小）一律退化成 Step 取簇末速度
//     ——宁可写成 `♩=N` 也不要给谱面撒一个假的 rit.。
// 曲首速度恒为独立的 Step，所以「从头开始渐慢」会得到 ♩=120 + rit. 120→60。
std::vector<TempoMark> tempoMarksOf(const MidiData& data);

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

// 0x05 歌词分散在多轨时的取用结果。
//
// 一个文件里出现两条歌词轨是常事：卡拉 OK 文件把第一段词、第二段词各放一轨；
// 也有伴奏轨拿 0x05 存和弦名。旧实现按 tick 把所有轨的事件合并成一行，得到
// 「春夜来风雨声花落」这种交错乱码，音节数还会翻倍。现在只取有效音节最多的
// 那一条轨（并列时取轨号小的），其余整轨忽略并记下丢了多少。
struct LyricTrackPick {
    int track = -1;            // 实际取用的轨；-1 = 没有可用歌词
    int syllables = 0;         // 该轨的有效音节数
    int droppedTracks = 0;     // 被忽略的其它歌词轨数
    int droppedSyllables = 0;  // 被忽略的音节数
};
LyricTrackPick pickMidiLyricTrack(const MidiData& data);

// 「（另 1 轨的 10 个音节已忽略）」，没有可忽略的轨时返回空串。
std::string droppedLyricNote(const MidiData& data);

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
//
// name 是谱面显示用的字符串（文本 / HTML 直接印它）；其余三个字段是给
// MusicXML 的 <harmony> 用的结构化信息——把「显示名」反解析成根音/类型
// 既脆弱又要把 m7b5、C/E 这类写法再拆一遍，不如在检测时就一并给出。
struct ChordMark {
    long startTick = 0;
    std::string name;            // 如 "C", "Am", "G7", "C/E", "Bm7b5"
    int rootPc = -1;             // 根音音级 0..11（绝对音高），-1 = 未知
    std::string kind;            // MusicXML <kind> 标准值，如 "major" / "half-diminished"
    std::string kindText;        // <kind text="..."> 的显示后缀，如 "m7b5" / "7sus4"
    int bassPc = -1;             // 转位低音音级，-1 = 原位（或低音不是和弦音）
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

// ---- 声部 / 乐器标注 ----
// GM 音色号(0..127) -> 乐器名。常用乐器给中文译名（钢琴/长笛/二胡风格等），
// 其余回落到 GM 标准英文全名。中文曲 MIDI 常把笛子/古筝/二胡映射到 Flute/Harp/
// Tremolo Strings 等通用音色，所以中文译名只覆盖标准 GM 集，不臆造民族乐器名。
std::string gmInstrumentName(int prog);

// 列出所有「含音符」的声部（跳过打击乐通道 9）。返回 (声部名, 乐器名)，
// 声部名取轨道名（0x03），没有就回落到「轨 N」；乐器名来自该声部主导通道的
// 0xC0 音色号，查不到则空串。
std::vector<std::pair<std::string, std::string>> listParts(const MidiData& data);

// 页眉用的声部摘要，如 "声部：Vocal·钢琴, Piano·钢琴"；没有可标注的声部时返回空串。
std::string partSummary(const MidiData& data);

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

// ---- 演奏表情（力度 / 渐强渐弱 / 断音 / 重音 / 滑音 / 颤音 / 段落）----
//
// 这些数据在解析层早就有了（CC11、note-on velocity、pitch bend、0x06
// marker），但只有 CC11 被文本谱和 HTML 各写了一遍粗糙的 dynAt()，
// MusicXML 完全没有；velocity、弯音、marker 则一条都没进过谱面。
// 三个输出层各写一套的后果是「网页有力度、导出 MuseScore 却没有」。
//
// ExpressionMap 把判定收成唯一权威：构造一次，三个输出层只读结果。
// 判据全部是硬证据（数值阈值），且**只在真的检出事件时才输出**——
// 一份什么表情数据都没有的合成 MIDI，谱面与上一版逐字节一致。
enum class BendMark { None = 0, SlideUp = 1, SlideDown = 2, Trill = 3 };

// 挂在谱面「标记行」的事件：力度档位、渐强/渐弱起止、段落名。
struct ExpressionMark {
    long tick = 0;
    std::string dynamic;      // "pp"/"p"/"mp"/"mf"/"f"/"ff"，空 = 此处无变化
    int wedge = 0;            // +1 渐强起点 / -1 渐弱起点 / 0 无
    bool wedgeStop = false;   // 渐强渐弱的终点
    std::string section;      // 0x06 marker 的原文

    bool empty() const {
        return dynamic.empty() && wedge == 0 && !wedgeStop && section.empty();
    }
};

// 挂在单个音头上的演奏法。断音与重音可以同时成立，所以不用枚举。
struct NoteExpression {
    bool staccato = false;
    bool staccatissimo = false;
    bool accent = false;
    BendMark bend = BendMark::None;
    int gracePitch = -1;   // >=0 表示本音前面带一个倚音（MIDI 音号），-1 表示无

    bool any() const {
        return staccato || staccatissimo || accent ||
               bend != BendMark::None || gracePitch >= 0;
    }
    bool hasGrace() const { return gracePitch >= 0; }
    std::string symbol() const;  // 文本谱用的紧凑标记，如 "▼>"、"↗"
};

class ExpressionMap {
public:
    ExpressionMap() = default;
    explicit ExpressionMap(const MidiData& data);

    bool enabled() const { return enabled_; }

    // 某 tick、某通道当前生效的力度档位。沿用旧 dynAt 的六档阈值，
    // 保证已有 CC11 素材的谱面不因这次重构而变样。
    std::string dynamicAt(long tick, int channel) const;

    // 某音头、某通道的演奏法；没有则返回全 false 的对象。
    // 键里必须带通道：旋律和伴奏常有同一 tick 的音头，只按 tick 索引会让
    // 伴奏「继承」旋律的滑音/断音记号（真实压力素材里踩到过）。
    NoteExpression noteAt(long tick, int channel) const;

    // 某个音头本身是不是「被判定为倚音」的音（渲染时整颗跳过，改挂到主音上）。
    // 同样按 (tick, channel) 索引，避免跨通道串扰。
    bool isGraceNote(long tick, int channel) const;

    // 按 tick 升序的标记行事件；inRange 供 MusicXML 按小节取用。
    const std::vector<ExpressionMark>& marks() const { return marks_; }
    std::vector<ExpressionMark> inRange(long from, long to) const;

    // "力度 3 · 渐强 1 · 断音 4 · 段落 2"；没有任何表情时返回空串。
    std::string summary() const;

    int dynamicCount() const      { return nDynamic_; }
    int crescendoCount() const    { return nCresc_; }
    int diminuendoCount() const   { return nDim_; }
    int articulationCount() const { return nArtic_; }
    int bendCount() const         { return nBend_; }
    int sectionCount() const      { return nSection_; }
    int totalCount() const {
        return nDynamic_ + nCresc_ + nDim_ + nArtic_ + nBend_ + nSection_;
    }

private:
    bool enabled_ = false;
    std::vector<DynPoint> dyn_;
    std::vector<ExpressionMark> marks_;
    std::map<std::pair<long, int>, NoteExpression> notes_;
    std::set<std::pair<long, int>> graceNotes_;  // 被判定为倚音的音头，渲染时跳过
    int nDynamic_ = 0, nCresc_ = 0, nDim_ = 0;
    int nArtic_ = 0, nBend_ = 0, nSection_ = 0;
};

// 力度档位 -> MusicXML <sound dynamics> 的百分比（用于导出时给出试听力度）。
int dynamicToMidiVolume(const std::string& dyn);

} // namespace m2j

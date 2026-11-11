#!/usr/bin/env python3
"""生成 midi2jianpu 的回归测试素材（标准 MIDI 文件，format 1）。

不依赖任何第三方库，只输出很小的 .mid 和 .lrc，便于在 CI / 新机器上
随时重建素材、复现问题。共生成 93 份 .mid 素材：sample / humanized / drums /
melisma / harmony / pickup / karaoke / meter / modulation / pentatonic /
tempolrc / expression / parts / chords / tempo_ramp / long_tie / polyphony /
smpte / format2 / aftertouch / retrigger / dangling / twolyrics /
pedal / allnotesoff / cc7volume / pedal_channels / pedal_hysteresis /
sostenuto / dyn_channels / wedge_tracks / oversized_len / voice_trio /
bendrange_channels / bendrange_timeline / bendrange_crosstrack /
running_meta / running_sysex / running_realtime / running_standard /
port_pedal / port_bend / port_prefix / port_prefix_base / port_standard /
sysex_gs_drum / sysex_xg_drum / sysex_gs_normal / sysex_gs_reset /
sysex_gm_reset / sysex_xg_reset / sysex_conductor / sysex_ch9_normal /
sysex_ch9_drum / sysex_unknown / sysex_port_drum / gate_gap / poly_gap / poly_gate /
rest_split / rest_midbar / beam_levels / beam_hooks / staff_switch / xml_structure /
grand_staff_wedge（另配 3 份 LRC 歌词）。
xml_structure 把三连音 / 和弦 / 多声部 backup / 变拍号 / 跨小节长音
叠进一份文件，供 check_xml_structure.py 审逐小节逐声部时值守恒。
staff_switch 是大谱表的换谱表压力样本：同一个声部在四个小节里分别
落在上行谱 / 下行谱 / 上行谱 / 前后各半，逼出「方向记号按小节与拍位
跟随音符所在谱表」这条口径。grand_staff_wedge 在大谱表上放一条跨谱表的
渐强楔形：起点在上行谱、终点落在下行谱，逼出「同一条 wedge 起止必须落
同一谱表」这条口径。beam_hooks 是钩形梁压力样本：同一梁组里孤立的十六分 / 三十二分音符，
次级梁既连不成段也不能整层丢掉，必须写 forward/backward hook。
beam_levels 是连梁的「压力样本」：4/4 四小节分别放全十六分、混合时值、
次级梁不连续、附点十六分，再变拍号到 6/8 两小节放三音连梁——在它之前
88 份素材里一个十六分音符都没有，MusicXML 的二级梁与 continue 状态只能靠
单测和临时探针覆盖，CI 从未在真实素材上审到。
meter 是把变拍号、变速、鼓轨、内置歌词、伴奏和弦混在一起的「压力样本」，
modulation 是中途转调（1=C → 1=D → 1=F），pentatonic 则是中国五声调式
（C 宫、G 徵调式，故意不写 0x59 逼自动猜调暴露宫音判错），tempo_ramp 用
每半拍一个 set_tempo 复刻 DAW 的渐快 / 渐慢曲线，long_tie 则是 6 拍 / 10 拍
长音跨小节，外加一条每小节一个 4 拍柱式和弦的钢琴轨，polyphony 则把钢琴
左右手放进同一个轨，测「同拍不同值不是和弦而是两个声部」。smpte 是
SMPTE 时间基准（0xE728 = 25fps×40），format2 则是两条互不相干的独立序列。
aftertouch 在每个音中间插 0xA0（3 字节事件，读错就整轨崩），retrigger 是同音
还没抬手又按下，dangling 是轨尾只按不抬的悬挂音，twolyrics 则把两段词放在两条
轨上（错开半拍，合并成一行就是乱码）。pedal 是踩着踏板只按 1/4 拍的钢琴和弦，
allnotesoff 用 CC123 代替 note-off，cc7volume 只写 CC7 主音量做渐强，
pedal_channels 则验证踏板不会给没踩的通道补时值。bendrange_channels 让通道 0
设 ±12 半音、通道 1 保持默认 ±2，bendrange_timeline 在同一通道中途把 ±12 改回
±2，bendrange_crosstrack 把 RPN 单独放在文件末尾的 setup 轨——三份都是弯音
范围「每通道、且随时间变化」的回归闸门。running_meta / running_sysex 让歌词
meta、SysEx 之后仍省略状态字节，running_realtime 在音符流里插 0xF8 时钟
（规范不改变 running status），running_standard 则是重写状态字节的规范基线：
四份都应读出 C4、D4 两个八分音符，少一个就说明事件流又错位了。
port_pedal 让 port 0 ch0 踩踏板、port 1 ch0 不踩，第三条轨不写端口（验证
端口不跨轨继承）；port_bend 让两个端口的 ch0 分别用 ±12 / ±2 弯音范围，
两份都是 FF 21 端口隔离的回归闸门。port_prefix 与 port_prefix_base 是同一首
曲子写 / 不写 FF 20（按规范 FF 20 只作用于 meta / SysEx，不重定向通道消息，
两份输出应逐字节一致），port_standard 则完全不写端口声明，锁住零回归。sysex_gs_drum / sysex_xg_drum
用 Roland GS / Yamaha XG 的 SysEx 把 ch0 指定为鼓，鼓件音高 36/38/42 不该
变成简谱里的低音；sysex_gs_normal 是 mode=0 的对照，一个音都不能丢；
sysex_gs_reset / sysex_gm_reset / sysex_xg_reset 在 tick 960 发三种 Reset，
前半段的鼓设定应被撤销；sysex_conductor 把 SysEx 放到文件最后一条轨，
锁住「解析顺序不能决定时间顺序」；sysex_ch9_normal 把通道 10 改回普通
钢琴，sysex_ch9_drum 则显式设回鼓；sysex_unknown 是不相干的 SysEx，
必须零回归；sysex_port_drum 验证鼓模式同样按 FF 21 端口隔离。bank_xg_drum /
bank_normal / bank_switch_back / bank_conductor / bank_port / bank_reset /
bank_ch9_plain / bank_no_program / program_timeline 覆盖 CC0/CC32 Bank Select
与 Program Change 的时间轴。diag_consistency 把中途换音色与 XG 鼓组放进同一首，
锁住 --list-tracks 诊断的乐器名与鼓轨标记必须和真实谱面一致。
track_pedal / track_rpn_setup / track_conductor /
track_leak / track_leak_base / track_empty / track_fmt2 是 --track 按轨隔离的
回归闸门：踏板、CC11/CC7、歌词、弯音写在别的轨上时不许串进选中轨，而
RPN、速度、拍号、调号、Marker 这些全局设定仍要按整份文件生效。truncated 是
「下载到一半」的损坏文件：MTrk 长度字段仍写着原值，轨尾只剩半个 note-on，
正确行为是只输出前两个真音并提示数据不完整，不能越过 buffer 末尾读幻影音符。
dyn_channels 让旋律(ch0)渐弱、伴奏(ch1)渐强，锁住渐强渐弱记号按通道归属：
伴奏的 cresc. 不该印到旋律谱上，全局段落名 marker 则仍要照常显示。
wedge_tracks 把渐弱旋律(ch0)与渐强伴奏(ch1)放进两条独立轨：旧版三端都
只输出第一轨的 dim.，第二轨的 cresc. 被「只印第一声部」的门控吞掉；修复后
文本 / HTML 两行谱各印自己的渐变，MusicXML 两个 part 各写一份 wedge，
而全局段落名 Verse 仍只出现一次。
soft_pedal 在 [1200, 2000) 踩 CC67 软踏板，两处强音里只有踏板内的那一处
力度被衰减（110→77），谱面的 '>' 从 2 个变成 1 个，时值完全不变。
chunk_resync 同时放一个 DAW 私有 chunk（XFIH）和少写 4 字节的 conductor
长度字段：旧解析器直接判「未提取到音符」，修复后应照常读出 4 个旋律音。
oversized_len 把首轨长度字段写成 0x00010000（越过整份文件），但第二轨
完整存在：旧实现把整份文件当成一轨的截断残片，误报「文件数据不完整」、
两轨并成一轨、--track 1 拿不到旋律；修复后按 EOT 锚点切开，两轨各归各位，
页眉只报「重同步 1 条轨道边界」。
voice_trio 把三个通道、三条反向 CC11 曲线塞进同一条轨（8 个四分音符 /
2 个二分音符 / 1 个全小节长音），逼 splitVoices 拆出 3 个 <voice>：锁住
MusicXML 每个声部各写自己的力度与楔形、direction 带 <voice> 归属、wedge
number 递增到 3，而全局段落名 Verse 仍只写一次。
rest_split 故意留下 3.75 拍 / 1.75 拍 / 1.25 拍三段 DAW 常见的不规则休止，
外加一段 3 拍附点二分休止作精确对照：休止符必须拆成标准时值之和，
每个 <note><rest/> 的 <duration> 与 <type>/<dot/> 严格一致，小节总时长不变。
前两小节文本谱照样守恒 4 拍；第三小节的 1.75 / 1.25 拍间隙在 v1.59.0
之前也按最近档记成 0. / 0，读出 3.5 拍（不是末小节惯例，是同一缺陷），
现在拆成 0. -= / 0 -= 后同样守恒 4 拍。
rest_midbar 把这两段不规则间隙挪到小节中间，专门守护「后面紧跟音头时
休止符也不许按最近档吸附」：旧版第一小节读 3.5 拍，修复后两小节都 4 拍。
html_dots 专测 HTML 谱面的附点：0.375 / 0.75 / 1.5 拍的单音、休止符与
和弦都必须在数字右侧画一个点，而 3 拍靠延音线表达（不画点）、三连音
不画点、和弦列只画一次点（挂在最低音那个 tone 上）。

用法：
    python tests/fixtures/make_fixture.py            # 写入本目录 sample.mid / sample.lrc
    python tests/fixtures/make_fixture.py out.mid    # 指定输出文件

素材内容（C 大调，4/4，120BPM，共 4 小节）：
    轨道 0  Conductor：速度/拍号/调号/标记
    轨道 1  Vocal    ：四个四分音符、二分音符、同音重复（应合并成延音）、
                       三连音、休止符，并带 p / ff / mf 三次力度变化
    轨道 2  Piano    ：C - F - C/G - F 的和弦进行，用于和弦检测
    轨道 3  Pad      ：两个跨越小节线的长音，用于验证 MusicXML 的切分与 <tie>
"""
import os
import struct
import sys

DIV = 480
BAR = DIV * 4


def vlq(n: int) -> bytes:
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def meta(kind: int, data: bytes) -> bytes:
    return b"\xFF" + bytes([kind]) + vlq(len(data)) + data


class Track:
    def __init__(self) -> None:
        self.ev = []
        self._order = 0

    def add(self, tick: int, raw: bytes) -> None:
        self.ev.append((tick, self._order, raw))
        self._order += 1

    def note(self, ch: int, pitch: int, vel: int, start: int, dur: int) -> None:
        self.add(start, bytes([0x90 | ch, pitch, vel]))
        self.add(start + dur, bytes([0x80 | ch, pitch, 0x40]))

    def cc(self, ch: int, ctrl: int, val: int, tick: int) -> None:
        self.add(tick, bytes([0xB0 | ch, ctrl, val]))

    def bend(self, ch: int, value: int, tick: int) -> None:
        """弯音轮事件（14 位值，8192 居中，16383 为推到顶）。"""
        self.add(tick, bytes([0xE0 | ch, value & 0x7F, (value >> 7) & 0x7F]))

    def bend_range(self, ch: int, semitones: int, tick: int, cents=None) -> None:
        """RPN 0x0000 弯音灵敏度；cents 为可选的 CC38 音分微调。"""
        self.cc(ch, 101, 0, tick)
        self.cc(ch, 100, 0, tick)
        self.cc(ch, 6, semitones, tick)
        if cents is not None:
            self.cc(ch, 38, cents, tick)
        self.cc(ch, 101, 127, tick)   # 退出 RPN 状态（真实导出器常见写法）
        self.cc(ch, 100, 127, tick)

    def program(self, ch: int, prog: int, tick: int) -> None:
        self.add(tick, bytes([0xC0 | ch, prog]))

    def render(self) -> bytes:
        out = bytearray()
        last = 0
        for tick, _, raw in sorted(self.ev, key=lambda x: (x[0], x[1])):
            out += vlq(tick - last) + raw
            last = tick
        out += vlq(0) + meta(0x2F, b"")
        return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


def build() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # 0 升降号 -> C 大调
    t0.add(0, meta(0x06, b"Intro"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    t1.cc(0, 11, 30, 0)        # p
    t1.cc(0, 11, 100, 3840)    # ff
    t1.cc(0, 11, 70, 5760)     # mf
    melody = [
        (60, 480, 0), (62, 480, 480), (64, 480, 960), (65, 480, 1440),
        (67, 960, 1920), (69, 480, 2880), (71, 480, 3360),
        (72, 480, 3840), (72, 480, 4320), (76, 960, 4800),      # 同音重复 -> 延音
        (67, 160, 5760), (67, 160, 5920), (67, 160, 6080),      # 三连音
        (69, 480, 6720), (71, 480, 7200),                       # 前面留 480 休止
    ]
    for pitch, dur, start in melody:
        t1.note(0, pitch, 90, start, dur)

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for p in (48, 52, 55):   # C
        t2.note(1, p, 70, 0, BAR)
    for p in (53, 57, 60):   # F
        t2.note(1, p, 70, BAR, BAR)
    for p in (48, 52, 55):   # C
        t2.note(1, p, 70, BAR * 2, 960)
    for p in (55, 59, 62):   # G
        t2.note(1, p, 70, BAR * 2 + 960, 960)
    for p in (53, 57, 60):   # F
        t2.note(1, p, 70, BAR * 3, BAR)

    # 轨道 3  Pad：专门覆盖「音符跨小节线」这个最容易出错的场景。
    # MIDI 允许一个音符跨越小节线，但 MusicXML 必须把它切开、段间加 <tie>，
    # 否则该小节的 duration 会超出拍号规定值，MuseScore 会报错或强行排乱。
    t3 = Track()
    t3.add(0, meta(0x03, b"Pad"))
    t3.program(2, 89, 0)
    t3.note(2, 48, 70, BAR * 0 + 1440, 960)    # m1 第 4 拍 -> m2 第 1 拍（短跨）
    t3.note(2, 60, 70, BAR * 1 + 960, 2880)    # m2 第 3 拍 -> m3 末（跨 5 拍，拆 half + whole）

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


LRC = """[ti:测试]
[ar:xueyuejue]
[00:00.00]风霜染尽客行袍
[00:02.00]江湖夜雨十年灯
[00:04.00]桃李春风一杯酒
[00:06.00]江湖夜雨十年灯
"""


# ---------------------------------------------------------------------------
# 第二份素材：模拟"真实录音转出的 MIDI"，专门覆盖时值量化与调性判定。
#
# 真实录音 MIDI 有两个特征会让朴素的记谱实现出错：
#   1) note-off 早于下一个音头（门限时间，DAW 默认常见 85%），
#      于是八分音符的时长是 0.425 拍而不是 0.5 拍；
#   2) 旋律是五声/六声的，缺某几个音级，K-S 调性识别容易判偏。
#
# 这里用 A 羽调式（音级 A B C D E G，没有 F / F#）、八分音符、85% 门限，
# 终止音落在 A。正确结果应是 1=C（a 小调，主音=6），且时值全部是八分音符。
# ---------------------------------------------------------------------------
HUMAN_PITCHES = [69, 67, 64, 67, 69, 72, 69, 67,   # A G E G A C A G
                 64, 67, 69, 67, 64, 62, 60, 62]   # E G A G E D C D
HUMAN_DIV = 960
HUMAN_GATE = 0.85          # 门限：实际时长 = 音头间隔 × 0.85
HUMAN_UNIT = 480           # 八分音符 = 480 tick
HUMAN_LEAD = 4 * 4 * HUMAN_DIV   # 开头空 4 小节，复现"前几小节全是休止"的情况


def build_humanized() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x51, (833333).to_bytes(3, "big")))      # 72 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x03, b"Conductor"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 53, 0)
    for i, p in enumerate(HUMAN_PITCHES):
        start = HUMAN_LEAD + i * HUMAN_UNIT
        dur = int(HUMAN_UNIT * HUMAN_GATE)                  # 408 tick
        t1.note(0, p, 90, start, dur)
    # 终止音拉长，落在 A 上（羽调式的终止）
    last = HUMAN_LEAD + len(HUMAN_PITCHES) * HUMAN_UNIT
    t1.note(0, 69, 90, last, int(HUMAN_DIV * 2 * HUMAN_GATE))   # 二分音符，1360 tick

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, HUMAN_DIV)
    return header + t0.render() + t1.render()


# ---------------------------------------------------------------------------
# 第三份素材：带打击乐轨的编曲工程（几乎每首歌的 MIDI 都是这个形状）。
#
# 通道 10（index 9）是 GM 规定的打击乐通道，音高是鼓件编号不是音级。这份素材
# 里鼓的音符数（192）远超旋律（8），旧实现会把鼓当成"旋律轨"，谱面变成
# `1,, #4,, #6,,` 的垃圾，和弦标记和歌词也跟着挂错地方。
#
# 正确结果：鼓音符全被丢弃、旋律轨判给单声部的 Vocal、Piano 仍是伴奏。
# ---------------------------------------------------------------------------
DRUM_BARS = 8


def build_drums() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))    # 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))         # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))                # C 大调

    # 旋律：单声部，8 个四分音符
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    for i, p in enumerate([60, 62, 64, 65, 67, 69, 71, 72]):
        t1.note(0, p, 90, i * BAR, BAR)

    # 钢琴：柱式和弦，每小节 3 个音（复音数 3，明显不是旋律）
    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for bar in range(DRUM_BARS):
        for p in (48, 52, 55):
            t2.note(1, p, 70, bar * BAR, BAR)

    # 鼓：通道 9，十六分音符踩镲 + 底鼓 + 军鼓，音符数远超旋律
    t3 = Track()
    t3.add(0, meta(0x03, b"Drums"))
    t3.program(9, 0, 0)
    for bar in range(DRUM_BARS):
        for k in range(16):
            st = bar * BAR + k * 120
            t3.note(9, 42 if k % 2 == 0 else 46, 100, st, 60)   # 踩镲
            if k % 4 == 0:
                t3.note(9, 36, 110, st, 120)                    # 底鼓
            if k % 4 == 2:
                t3.note(9, 38, 110, st, 120)                    # 军鼓

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


# ---------------------------------------------------------------------------
# 第四份素材：一行歌词跨两小节，而旋律音数多于字数。
#
# 这是真实歌曲的常态（衬腔、走句、一字多音）：LRC 一行 4 个字，旋律在两小节里
# 唱 8 个音。早先按「第 k 个音领第 k 个字」顺序发，4 个字全被第一小节吃完，
# 第二小节一个词都没有——看上去像漏词。正确行为是按行内音数铺开，两小节都
# 应该有字。
# ---------------------------------------------------------------------------
MELISMA_LRC = """[ti:一字多音]
[ar:xueyuejue]
[00:00.00]一二三四
[00:04.00]五六七八
"""


def build_melisma() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))    # 120BPM，一小节 2 秒
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))         # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))                # C 大调

    # 每小节 4 个四分音符，两小节 8 个音；歌词一行只有 4 个字
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    for i, p in enumerate([60, 62, 64, 65, 67, 69, 71, 72]):
        t1.note(0, p, 90, i * (BAR // 4), BAR // 4)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


# ---------------------------------------------------------------------------
# 第五份素材：一小节两个和弦 + 中途变速。
#
# 两件事都是真实流行歌的常态，也都曾被实现漏掉：
#   1) `| C  Am | F  G |` 一小节换两次和弦。按小节汇总伴奏音只会得到一个和弦，
#      后半小节的和声在谱面上根本不存在；
#   2) 副歌提速（120 → 90 这里反过来是降速），页眉只写第一个速度，
#      照着唱就会把后半首唱快。
#
# 正确结果：8 个小节，每小节一个起点和弦（标记行）+ 一个第三拍和弦（音符上方），
# 页眉写成 "♩=120 → 90(第5小节)"，第 5 小节上有 ♩=90 的速度标记。
# ---------------------------------------------------------------------------
HARMONY_BARS = 8
HARMONY_MEL = [72, 71, 69, 67]        # 1' 7 6 5
# (小节奇数, 前半小节和弦, 后半小节和弦)
HARMONY_PROG = [((48, 52, 55), (45, 48, 52)),    # C  -> Am
                ((41, 45, 48), (43, 47, 50))]    # F  -> G


def build_harmony() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))            # 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))                 # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))                        # C 大调
    t0.add(4 * BAR, meta(0x51, bytes([0x0A, 0x2C, 0x2B])))      # 第 5 小节起 90BPM

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    for bar in range(HARMONY_BARS):
        for b, p in enumerate(HARMONY_MEL):
            t1.note(0, p, 90, bar * BAR + b * DIV, DIV)

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for bar in range(HARMONY_BARS):
        first, second = HARMONY_PROG[bar % 2]
        for p in first:
            t2.note(1, p, 70, bar * BAR, 2 * DIV)
        for p in second:
            t2.note(1, p, 70, bar * BAR + 2 * DIV, 2 * DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


# ---------------------------------------------------------------------------
# 第六份素材：弱起（anacrusis）。
#
# 歌本里大量曲子是从弱拍进唱的：`| 5 | 1' 7 6 5 |`。MIDI 里这个 5 就从 tick 0
# 开始，于是第一小节天然不满四拍。若照 `tick / 小节长` 划小节线，整条网格会
# 整体左移一拍——谱面上每一小节的重音都落到第 4 拍，小节线全部错位。
#
# 正确结果：首小节单独成一栏且不编号（弱起 1 拍），后面两个完整小节编号 1、2；
# 和弦也要按新网格切窗，弱起小节是 G，第一个完整小节才是 C。
# ---------------------------------------------------------------------------
PICKUP_BARS = 2


def build_pickup() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))    # 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))         # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))                # C 大调

    # 旋律：弱起 1 拍（G5），然后两个完整小节的四分音符，末音正好收在小节线上
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    t1.note(0, 67, 90, 0, DIV)                                   # 弱起：5
    off = DIV
    for p in (72, 71, 69, 67, 64, 65, 67, 72):                   # 1' 7 6 5 | 3 4 5 1'
        t1.note(0, p, 90, off, DIV)
        off += DIV

    # 伴奏：弱起小节配属和弦 G，第一个完整小节才是主和弦 C —— 弱起进主和弦
    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for p in (43, 47, 50):                                        # G
        t2.note(1, p, 70, 0, DIV)
    for p in (48, 52, 55):                                        # C
        t2.note(1, p, 70, DIV, 2 * DIV)
    for p in (45, 48, 52):                                        # Am
        t2.note(1, p, 70, DIV + 2 * DIV, 2 * DIV)
    for p in (41, 45, 48):                                        # F
        t2.note(1, p, 70, BAR + DIV, 2 * DIV)
    for p in (43, 47, 50):                                        # G
        t2.note(1, p, 70, BAR + 3 * DIV, 2 * DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


def build_karaoke() -> bytes:
    """带 MIDI 内置歌词（0x05 lyric meta）的卡拉 OK 文件。

    真实 .mid 里的歌词事件比「一个事件一个汉字」脏得多，这份素材把常见写法
    一次塞齐，用来锁住规整规则：
      - @KMIDI / @TITLE 之类的文件元数据
      - "-" / "_" 表示一字多音的延续（不产生新音节，前一个字自然拖长）
      - "/" 换行、"+" 续行前缀
      - 中文标点、英文单词、尾随空格
    伴奏是柱式和弦（音符多、复音率高），歌词事件全在 Vocal 轨上 —— 顺便
    验证「歌词所在轨」能把旋律轨判对。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)                     # Voice Oohs
    # 两小节、8 个四分音符；歌词事件的 tick 与这些音头一致
    for i, pitch in enumerate((60, 62, 64, 65, 67, 69, 71, 72)):
        t1.note(0, pitch, 90, i * 480, 480)

    def lyric(tick: int, text: str) -> None:
        t1.add(tick, meta(0x05, text.encode("utf-8")))

    lyric(0, "@KMIDI KARAOKE FILE")   # 元数据，跳过
    lyric(0, "你")
    lyric(480, "-")                  # 延续 -> 「你」拖两个音
    lyric(960, "好，")                # 中文标点不占音节
    lyric(1440, "_")                 # 延续 -> 「好」拖两个音
    lyric(1920, "/")                 # 换行
    lyric(1920, "+世界")              # 续行前缀；两个字共享 [1920,2880)
    lyric(2880, "Hello ")            # 英文单词是一个音节，尾空格要 trim
    lyric(3360, "-")                 # 延续 -> Hello 拖两个音

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for bar, chord in ((0, (48, 52, 55)), (BAR, (53, 57, 60))):
        for p in chord:
            t2.note(1, p, 70, bar, BAR)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


def build_meter() -> bytes:
    """中途变拍号 + 变速 + 鼓轨 + 内置歌词 + 伴奏和弦的「混合压力」素材。

    真实编曲工程导出的 .mid 往往同时带着这些特征，分开测都过、混在一起就
    互相带偏（拍号取最后一个 -> 弱起误判 -> 网格平移 -> 歌词与和弦全错位）。
    时间轴（division=480）：

        4/4 × 2 小节 : 0    ~ 3840   (每小节 1920)
        3/4 × 2 小节 : 3840 ~ 6720   (每小节 1440)
        6/8 × 2 小节 : 6720 ~ 9600   (每小节 1440，6 个八分音符)

    另外故意在伴奏轨重复声明同样的 0x58，验证多轨重复拍号不会被当成变拍号；
    通道 10 放几个鼓点，验证打击乐不会污染旋律轨判定。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))    # 120 BPM
    t0.add(3840, meta(0x51, bytes([0x0A, 0x2C, 0x2A])))  # 666666us -> 90 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))          # 4/4
    t0.add(3840, meta(0x58, bytes([3, 2, 24, 8])))       # 3/4
    t0.add(6720, meta(0x58, bytes([6, 3, 24, 8])))       # 6/8
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)
    # 4/4：每小节 4 个四分音符
    for bar in range(2):
        for b in range(4):
            t1.note(0, 60 + b, 90, bar * 1920 + b * 480, 480)
    # 3/4：每小节 3 个四分音符
    for bar in range(2):
        for b in range(3):
            t1.note(0, 64 + b, 90, 3840 + bar * 1440 + b * 480, 480)
    # 6/8：每小节 6 个八分音符
    for bar in range(2):
        for b in range(6):
            t1.note(0, 67 + b, 90, 6720 + bar * 1440 + b * 240, 240)

    def lyric(tick: int, text: str) -> None:
        t1.add(tick, meta(0x05, text.encode("utf-8")))

    # 真实卡拉 OK 的写法：每个旋律音头一个事件，一字多音用 "-" 延续。
    # 只有 480 与 4320 两处延续，所以应当恰好产生 2 组连音线。
    onsets = [0, 480, 960, 1440, 1920, 2400, 2880, 3360,
              3840, 4320, 4800, 5280, 5760, 6240,
              6720, 6960, 7200, 7440, 7680, 7920,
              8160, 8400, 8640, 8880, 9120, 9360]
    words = "春风又绿江南岸明月何时照我还归去来兮风雪夜归人归"
    wi = 0
    for tick in onsets:
        if tick in (480, 4320):
            lyric(tick, "-")
        else:
            lyric(tick, words[wi])
            wi += 1

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    # 多轨重复声明同一拍号：不该被算成「变拍号」
    t2.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t2.add(3840, meta(0x58, bytes([3, 2, 24, 8])))
    bars = [
        (0,    1920, (48, 52, 55)),   # C
        (1920, 1920, (53, 57, 60)),   # F
        (3840, 1440, (55, 59, 62)),   # G
        (5280, 1440, (48, 52, 55)),   # C
        (6720, 1440, (53, 57, 60)),   # F
        (8160, 1440, (55, 59, 62)),   # G
    ]
    for start, length, chord in bars:
        for p in chord:
            t2.note(1, p, 70, start, length)

    t3 = Track()
    t3.add(0, meta(0x03, b"Drums"))
    for bar_start in (0, 1920, 3840, 5280, 6720, 8160):
        t3.note(9, 36, 100, bar_start, 120)   # channel index 9 = MIDI 通道 10

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


def build_modulation() -> bytes:
    """中途转调（1=C → 1=D → 1=F）的混合压力素材，12 小节 4/4。

    时间轴（division=480，每小节 1920）：
        第 1~4 小节  0     ~ 7680   C 大调 (0x59 sf=0)
        第 5~8 小节  7680  ~ 15360  D 大调 (0x59 sf=+2)，同时降速到 90 BPM
        第 9~12 小节 15360 ~ 23040  F 大调 (0x59 sf=-1)

    验证要点：
      * 同一个绝对音高 C4=60 在三段里应分别记作 1 / #6 / 5 —— 三段旋律
        每小节第一个音都故意放 C4，唱名不随调变就是没生效；
      * 伴奏在 F 段放 Bb 三和弦，降号调里应写 Bb 而不是 A#；
      * 转调点与变速点同落在第 5 小节，两种 attributes 要合进一个块；
      * 伴奏轨重复声明 0x59 不算转调；鼓轨、内置歌词、melisma 全部叠加，
        确认转调不会把歌词/和弦/时值带偏。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))     # 120 BPM
    t0.add(7680, meta(0x51, bytes([0x0A, 0x2C, 0x2A])))  # 666666us -> 90 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))                  # C 大调
    t0.add(7680, meta(0x59, bytes([2, 0])))                # D 大调
    t0.add(15360, meta(0x59, bytes([(-1) & 0xFF, 0])))     # F 大调

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)
    # 每段四个小节，每小节 4 个四分音符；每小节第一个音都是 C4=60。
    scales = [
        (60, 62, 64, 65),   # C 段：C D E F
        (60, 62, 64, 66),   # D 段：C D E F#
        (60, 65, 67, 69),   # F 段：C F G A
    ]
    words = "春风又绿江南岸明月何时照我还归去来兮风雪夜归人田园将芜胡不归既自以心为形役奚惆怅而独悲悟已往之不谏知来者之可追实迷途其未远觉今是而昨非"
    wi = 0
    onsets = []
    for seg in range(3):
        for bar in range(4):
            base = seg * 4 * 1920 + bar * 1920
            for beat in range(4):
                tick = base + beat * 480
                t1.note(0, scales[seg][beat], 90, tick, 480)
                onsets.append(tick)
    # 每个音头一个 0x05 事件；两处用 "-" 延续，应恰好产生 2 组连音线。
    for tick in onsets:
        if tick in (480, 8160):
            t1.add(tick, meta(0x05, b"-"))
        else:
            t1.add(tick, meta(0x05, words[wi % len(words)].encode("utf-8")))
            wi += 1

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    # 多轨重复声明调号：归一化时同 tick 去重，不能算成额外转调
    t2.add(0, meta(0x59, bytes([0, 0])))
    t2.add(7680, meta(0x59, bytes([2, 0])))
    chords = [
        (48, 52, 55),   # C
        (53, 57, 60),   # F
        (50, 54, 57),   # D
        (55, 59, 62),   # G
        (53, 57, 60),   # F
        (46, 50, 53),   # Bb —— 降号调里应写作 Bb 而非 A#
    ]
    for seg in range(3):
        for bar in range(4):
            start = seg * 4 * 1920 + bar * 1920
            chord = chords[seg * 2 + (bar % 2)]
            for p in chord:
                t2.note(1, p, 70, start, 1920)

    t3 = Track()
    t3.add(0, meta(0x03, b"Drums"))
    for bar in range(12):
        t3.note(9, 36, 100, bar * 1920, 120)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


def build_transpose_key() -> bytes:
    """--transpose 的调号素材：1=Bb（降号调）起步，第 5 小节转到 1=D（升号调）。

    8 小节 4/4，division=480：
        第 1~4 小节  0    ~ 7680   Bb 大调 (0x59 sf=-2)
        第 5~8 小节  7680 ~ 15360  D  大调 (0x59 sf=+2)

    验证要点：
      * 不带 --transpose 时页眉是「1=Bb 大调 → 1=D(第5小节)」，两段唱名
        都是 1 2 3 4 —— 调号已经换过，唱名不该跟着绝对音高走；
      * --transpose +2 后调号必须整体上移一个全音（「1=C → 1=E」），而不是
        只把音符移走、页眉还写着 1=Bb（那样唱名会变成 2 3 4 5）；
      * --transpose +1 要落到「1=B → 1=Eb」：跨出零点时取升降号最少的等价
        写法（Bb+1 是 5 个升号的 B，不是 7 个降号的 Cb）；
      * --transpose +12 只换八度，调号一个字都不许变。
      * 每段用足 7 个音级，避免被五声调式判定（那会改掉 do 的位置）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))        # 500000us -> 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))             # 4/4
    t0.add(0, meta(0x59, bytes([(-2) & 0xFF, 0])))          # Bb 大调
    t0.add(7680, meta(0x59, bytes([2, 0])))                 # D 大调

    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    t1.program(0, 73, 0)                                    # Flute
    # Bb 段（Bb C D Eb / F G A Bb 上下行），D 段（D E F# G / A B C# D）。
    bb = [70, 72, 74, 75, 77, 79, 81, 82, 82, 81, 79, 77, 75, 74, 72, 70]
    dd = [62, 64, 66, 67, 69, 71, 73, 74, 74, 73, 71, 69, 67, 66, 64, 62]
    for bar in range(8):
        scale = bb if bar < 4 else dd
        for beat in range(4):
            t1.note(0, scale[(bar % 4) * 4 + beat], 88,
                    bar * 1920 + beat * 480, 480)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_poly_gate() -> bytes:
    """复调轨的门限因子按声部估（v1.56.0）。

    钢琴双手：左手是每 240 tick 一个的八分连奏音（dur == ioi == 240，不
    携带门限信息），右手是 DAW 门限 70% 的四分音符（只响 336 tick、
    间隔 480）。整轨音头因此每 240 tick 一个，第一遍估门限时右手的
    dur/ioi = 336/240 = 1.4 被当成无效样本丢掉，左手 240/240 = 1.0 灌满
    样本，全局 gate 落到 1.0 —— 右手 12 个四分音符全被写成附点八分
    （1_. 2_. 3_. 4_.），MusicXML 补出 12 个 16 分休止，HTML 印出 12 个
    附点八分，三端拍数各不相同。

    正确行为：只对「整轨音头挡在本声部下一音头前面」的音另按本声部口径
    估一个 laneGate（这里 0.7）并还原；左手连奏音与末音都不套用还原。
    右手回到 1 2 3 4，左手 24 个八分音一字不改。末音收在小节线上，免得
    被判弱起。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    for i in range(24):                         # 左手：24 个八分连奏音
        t1.note(0, 48 + (i % 3), 70, i * 240, 240)
    scale = [60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65]
    for i, p in enumerate(scale):               # 右手：门限 70% 的四分音符
        t1.note(0, p, 90, i * DIV, 336 if i < 11 else DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_poly_gap() -> bytes:
    """复调轨按声部补空档（v1.55.0）。

    钢琴双手：左手每小节按住一个全音符，右手是 DAW 门限 85% 的四分音符
    （只响 400 tick、间隔 480）。整轨因此被判成复调（monophonic=false），
    v1.54.0 的「空档并进前一个音」被整轨开关一刀切掉：右手 4/4 一小节
    只剩 3 拍（1_. 2_. 3_. 4_.），MusicXML 却按原始时值补出 7 个 16 分
    休止，HTML 也印出 7 个附点八分——三端拍数各不相同。

    正确行为是按声部各自算「下一个音头」：右手补成四个四分音符，左手
    全音符仍单独成声部、时值一字不改。末音收在小节线上，免得被判弱起。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    for bar in range(2):                       # 左手：两个全音符
        t1.note(0, 48, 70, bar * 4 * DIV, 4 * DIV)
    for i, p in enumerate([60, 62, 64, 65, 67, 69, 71]):
        t1.note(0, p, 90, i * DIV, DIV - 80)   # 右手：只响 400 tick
    t1.note(0, 72, 90, 7 * DIV, DIV)           # 末音收在小节线

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_rest_split() -> bytes:
    """v1.58.0：不规则休止间隙必须拆成标准时值之和。

    4/4 三小节，非标准间隙都放在小节末尾，前两小节文本谱仍守恒 4 拍：
      小节 1  0 起十六分(120)      -> 小节末剩 1800(3.75 拍)
      小节 2  1920 起四分(480)     -> 小节末剩 1440(3 拍，附点二分，精确对照)
      小节 3  3840 起十六分(120)、4800 起十六分、5520 起八分
              -> 间隙 840(1.75 拍)、600(1.25 拍)，最后一小节按惯例允许
                 尾上休止不满
    旧版把三段分别写成 whole / half / quarter 休止，<duration> 精确但
    <type> 只是 durTypeDot() 兜底猜的近似值；修复后应拆成
    (附点二分 + 附点八分) / (附点四分 + 16 分) / (四分 + 16 分)。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"rest_split"))
    t0.add(0, meta(0x51, struct.pack(">I", 500000)[1:]))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    t.note(0, 60, 100, 0, 120)
    t.note(0, 62, 100, BAR, 480)
    t.note(0, 64, 100, BAR * 2, 120)
    t.note(0, 65, 100, BAR * 2 + 960, 120)
    t.note(0, 67, 100, BAR * 2 + 1680, 240)
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) + t0.render() + t.render()


def build_rest_midbar() -> bytes:
    """v1.59.0：小节中间的不规则休止必须精确拆段，拍数守恒。

    4/4 两小节：
      小节 1  0 起十六分(120) -> 960 起十六分 -> 1680 起八分(240)
              间隙 840(1.75 拍) 与 600(1.25 拍) 都夹在小节中间
      小节 2  四个四分音符（保证第 1 小节不是最后一行，拍数校验会查它）
    旧版把两段按最近档记成 0.(1.5 拍) / 0(1 拍)，一小节只读出 3.5 拍；
    修复后写成 0. -= / 0 -=，两小节都正好 4 拍。
html_rests 是 v1.61.0 的 HTML 休止符拆列样本：1.75 / 1.25 / 2.5 / 2.25 拍
四段不规则间隙分别拆成 1.5+0.25 / 1.0+0.25 / 2.0+0.5 / 2.0+0.25 两列，
另留 0.75 / 1.0 拍两个精确档作对照（单列，不许碎片化）；所有间隙与后音头
都落在小节内，避开 HTML 不补跨小节间隙的既有门控。HTML 里每个 0 列
画出来的拍数之和必须与文本谱、MusicXML 一致。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"rest_midbar"))
    t0.add(0, meta(0x51, struct.pack(">I", 500000)[1:]))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    t.note(0, 60, 100, 0, 120)
    t.note(0, 62, 100, 960, 120)
    t.note(0, 64, 100, 1680, 240)
    t.note(0, 65, 100, BAR + 0, 480)
    t.note(0, 67, 100, BAR + 480, 480)
    t.note(0, 69, 100, BAR + 960, 480)
    t.note(0, 71, 100, BAR + 1440, 480)
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) + t0.render() + t.render()




def build_html_rests() -> bytes:
    """v1.61.0：HTML 休止符按精确档位之和拆列，与文本谱 / MusicXML 对齐。

    4/4 四小节，DIV=480、BAR=1920。每段间隙后面都紧跟一个音头（触发
    exactRest），且间隙与后音头都落在同一小节内 —— HTML 的 openBar 会
    重置 prevEnd，音头若正好在小节起点，跨小节的间隙就不补休止符了。
      小节 1  0.25 音 + 840(1.75) + 0.25 音 + 600(1.25) + 0.5 音
      小节 2  0.25 音 + 360(0.75 精确档) + 0.25 音 + 1200(2.5) + 0.25 音
      小节 3  0.25 音 + 1080(2.25) + 0.25 音 + 480(1.0 精确档) + 0.25 音
      小节 4  四个四分音符（保证前三小节都不是末小节）
    旧版 HTML 把 1.75 / 1.25 / 2.5 / 2.25 拍按最近档画成 1.5 / 1 / 2 /
    2 拍；修复后拆成 quarter dotted+sixteenth / quarter+sixteenth /
    half+eighth / half+sixteenth，拍数守恒。0.75 与 1.0 拍是精确档，
    必须保持单列，不许碎片化。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"html_rests"))
    t0.add(0, meta(0x51, struct.pack(">I", 500000)[1:]))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    # 小节 1：1.75 拍 + 1.25 拍
    t.note(0, 60, 100, 0, 120)
    t.note(0, 62, 100, 960, 120)
    t.note(0, 64, 100, 1680, 240)
    # 小节 2：0.75 拍精确档 + 2.5 拍
    t.note(0, 65, 100, BAR + 0, 120)
    t.note(0, 67, 100, BAR + 480, 120)
    t.note(0, 69, 100, BAR + 1800, 120)
    # 小节 3：2.25 拍 + 1.0 拍精确档
    t.note(0, 71, 100, BAR * 2 + 0, 120)
    t.note(0, 72, 100, BAR * 2 + 1200, 120)
    t.note(0, 74, 100, BAR * 2 + 1800, 120)
    # 小节 4：四个四分音符收尾
    for k in range(4):
        t.note(0, 69 + k, 100, BAR * 3 + k * 480, 480)
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) + t0.render() + t.render()


def build_html_dots() -> bytes:
    """v1.60.0：HTML 附点标记的三端一致性素材。

    4/4 四小节，DIV=480：
      小节 1  0.75 / 0.375 / 0.375 / 1.5 / 1.0 拍的单音（首尾相接，不留空档）
      小节 2  0.75 拍单音 + 0.75 拍附点八分休止 + 1.5 拍单音 + 1.0 拍单音
              （首尾填满 1920 tick，不留不足半拍的尾休止）
      小节 3  两串 0.75 拍柱式和弦 + 一串 1.5 拍和弦 + 1.0 拍单音
      小节 4  3.0 拍长音（简谱 5 - -，靠延音线表达，不该画附点）+ 1.0 拍单音
    文本谱的附点后缀与 MusicXML 的 <dot/> 是既有的真值，HTML 必须跟上。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"html_dots"))
    t0.add(0, meta(0x51, struct.pack(">I", 500000)[1:]))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    # 小节 1：四种附点档位都不留空档，避免时值量化把它们拉长
    t.note(0, 60, 100, 0, 360)        # 0.75  附点八分
    t.note(0, 62, 100, 360, 180)      # 0.375 附点十六分
    t.note(0, 64, 100, 540, 180)      # 0.375 附点十六分
    t.note(0, 65, 100, 720, 720)      # 1.5   附点四分
    t.note(0, 67, 100, 1440, 480)     # 1.0   四分（精确档，不画点）
    # 小节 2：0.75 拍附点八分单音 + 0.75 拍附点八分休止 + 1.5 拍附点四分
    # + 1.0 拍四分，正好填满 1920 tick；不留 0.25 拍尾休止（不足半拍门槛
    # 会被吞掉，check_bar_beats.py 会报这一小节只有 3.75 拍）。
    t.note(0, 69, 100, 1920, 360)
    t.note(0, 71, 100, 2640, 720)     # 前面 2280..2640 留 360 附点八分休止
    t.note(0, 72, 100, 3360, 480)
    # 小节 3：柱式和弦走 chordColumn，附点只能画一次
    for p in (60, 64, 67):
        t.note(1, p, 90, 3840, 360)
    for p in (62, 65, 69):
        t.note(1, p, 90, 4200, 360)
    for p in (59, 62, 67):
        t.note(1, p, 90, 4560, 720)
    t.note(0, 74, 100, 5280, 480)
    # 小节 4：3 拍长音靠延音线表达，不该画附点
    t.note(0, 72, 100, 5760, 1440)
    t.note(0, 71, 100, 7200, 480)
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) + t0.render() + t.render()


def build_gate_gap() -> bytes:
    """门限衰减留下的空档必须并进前一个音（v1.54.0）。

    真实演奏的 note-off 普遍早于下一个音头（DAW 的门限默认值多在 85%~90%），
    所以四分音符常常只响 400 tick、间隔 480。谱面照原样记就是「附点八分 +
    看不见的十六分休止」：文本谱一小节只剩 3 拍，MusicXML 却单独补出一串
    16 分休止，三端对不上。正确行为是把「写不出休止符」的空档并进前一个音，
    一小节就是四个四分音符。

    第 2 小节留一个真正的四分休止（空档 480 tick）作反向对照：合并只针对
    不足半拍、且不到音长三分之一的空档，真休止一个都不能丢。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    for i, p in enumerate([60, 62, 64, 65]):   # 第 1 小节：门限四分音符
        t1.note(0, p, 90, i * DIV, DIV - 80)   # 只响 400 tick
    t1.note(0, 67, 90, 4 * DIV, DIV)           # 第 2 小节：四分音符 + 四分休止
    t1.note(0, 69, 90, 6 * DIV, DIV - 80)      # 再一个门限音
    t1.note(0, 71, 90, 7 * DIV, DIV)           # 末音收在小节线（免被判成弱起）

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_overfull() -> bytes:
    """时值折算不许把小节撑过拍号；倚音不许吞掉前面的休止（v1.53.0）。

    3 小节 4/4，division=480（末音收在小节线上，不会被误判成弱起）：
        第 1 小节  0    ~ 1920   四个四分音符，把小节填满
        第 2 小节  1920 ~ 3840   16 分音符 + 3.75 拍休止
        第 3 小节  3840 ~ 5760   四分音符 + 1 拍休止 + 倚音 D5 + 二分音符

    两个缺陷：
      * durationSuffix() 把表外时值吸附到最近档，3.75 拍落在 3 与 4 之间时
        向上取整成 4 拍（`0 - - -`）——那一小节读成 4.25 拍，后面每小节的
        拍位整体错位。修复后拆成 `0 - -` + `-_.`，正好 3.75 拍。
      * 倚音整颗跳过时把前面的空白一起跳过（先跳倚音再算 gap），第 3 小节
        那 1 拍休止会整块消失，`0 (2')5'` 被印成 `(2')5'`，小节少一拍。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    t1.program(0, 73, 0)                               # Flute
    # 第 1 小节铺满，开头就不留白，弱起判定不会把网格整体平移
    for i, pitch in enumerate([72, 76, 79, 84]):
        t1.note(0, pitch, 88, i * 480, 480)
    # 第 2 小节：16 分音符之后空 3.75 拍（休止撑爆的路径）
    t1.note(0, 72, 88, 1920, 120)
    # 第 3 小节：四分音符 + 1 拍休止，随后是紧贴主音的倚音
    t1.note(0, 72, 88, 3840, 480)
    t1.note(0, 74, 90, 4770, 14)                       # 倚音 D5（极短，紧跟 G5）
    t1.note(0, 79, 88, 4800, 960)                      # 主音 G5，二分音符

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_pentatonic() -> bytes:
    """中国五声调式压力素材：C 宫五声、终止音 G 的「G 徵调式」。

    故意不写 0x59 调号，逼 estimateKey 走自动猜调：Krumhansl-Schmuckler
    的七声大/小调轮廓会把这首判成 G 大调（1=G），整份谱子的唱名全错。
    正确结果应按宫音记成 1=C，终止音 G 记作 5，页眉标「G 徵调式」。

    8 小节 4/4，旋律只使用 C D E G A 五个音级；叠加同样只用五声音的钢琴
    伴奏、鼓轨和 0x05 内置歌词，确认调式判定不会被伴奏/歌词/鼓轨带偏。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    # 关键：这里不写 0x59，自动猜调链路才会被走到
    t0.add(0, meta(0x06, b"Pentatonic"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)
    melody = [
        60, 62, 64, 67,   69, 67, 64, 62,
        60, 62, 64, 67,   69, 67, 64, 60,
        67, 69, 72, 74,   76, 74, 72, 67,
        60, 62, 64, 67,   69, 67, 64, 67,   # 末音 G = 终止音（徵）
    ]
    words = "春江潮水连海平海上明月共潮生滟滟随波千万里何处春江无月明江流宛转绕芳甸月照花林皆似霰"
    for i, pitch in enumerate(melody):
        tick = i * 480
        t1.note(0, pitch, 90, tick, 480)
        t1.add(tick, meta(0x05, words[i % len(words)].encode("utf-8")))

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    # 低音与和弦音都取 C D E G A，不引入偏音
    chords = [
        (48, 55, 64), (50, 57, 62), (52, 55, 60), (50, 57, 64),
        (48, 55, 64), (50, 57, 62), (48, 55, 60), (43, 55, 62),
    ]
    for bar, chord in enumerate(chords):
        for p in chord:
            t2.note(1, p, 70, bar * 1920, 1920)

    t3 = Track()
    t3.add(0, meta(0x03, b"Drums"))
    for bar in range(8):
        t3.note(9, 36, 100, bar * 1920, 120)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


# ---- LRC × tempo map 素材 ----
# 8 小节 4/4：前 4 小节 120 BPM（每小节 2 秒），第 5 小节起 60 BPM
# （每小节 4 秒）。每小节 4 个四分音符，LRC 每行 4 个字，时间戳写的是
# **真实秒**，正好落在各小节起点：0 / 2 / 4 / 6 / 8 / 12 / 16 / 20 秒。
#
# 旧解析器按曲首 120 BPM 一把换算：12 秒被算成 tick 11520（应为 9600，
# 整整差一小节），16 / 20 秒甚至超出曲终，后半段歌词直接消失。
# 所以这份素材能一眼看出「tempo map 到底接没接上」。
TEMPOLRC_LINES = [
    (0.0, "春风又绿"), (2.0, "江南岸边"),
    (4.0, "明月何时"), (6.0, "照我还乡"),
    (8.0, "小楼昨夜"), (12.0, "又东风起"),
    (16.0, "故国不堪"), (20.0, "回首月中"),
]


def _fmt_lrc_ts(sec: float) -> str:
    mm = int(sec // 60)
    rest = sec - mm * 60
    return f"[{mm:02d}:{rest:05.2f}]"


def build_tempolrc_lrc() -> str:
    return "\n".join(_fmt_lrc_ts(s) + w for s, w in TEMPOLRC_LINES) + "\n"


def build_tempolrc_offset_lrc() -> str:
    # 每个时间戳 +2 秒、再用 [offset:-2000] 拉回来，产物必须与主 LRC 等价。
    # 负 offset、大小写、元数据混排都在这一份里覆盖。
    head = "[ti:变速歌词]\n[OFFSET:-2000]\n"
    return head + "\n".join(_fmt_lrc_ts(s + 2.0) + w
                            for s, w in TEMPOLRC_LINES) + "\n"


def build_tempolrc_multi_lrc() -> str:
    # 一行两个时间戳：同一句词在 0 秒和 8 秒各出现一次（副歌重复）。
    return ("[00:00.00][00:08.00]重复一句\n"
            "[00:02.00]第二段词\n")


def build_tempolrc() -> bytes:
    """8 小节 4/4，tick 7680（第 5 小节）把 120 BPM 降到 60 BPM。"""
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # 0 升降号 -> C 大调
    t0.add(4 * BAR, meta(0x51, bytes([0x0F, 0x42, 0x40])))  # 1000000us -> 60BPM
    t0.add(4 * BAR, meta(0x06, b"Ritardando"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)
    scale = [60, 62, 64, 65, 67, 69, 71, 72]
    for i in range(32):                 # 8 小节 × 4 个四分音符
        t1.note(0, scale[i % len(scale)], 90, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_expression() -> bytes:
    """演奏表情压力素材：CC11 渐强/渐弱、note-on 力度重音、断奏、
    弯音上滑/下滑/颤音、0x06 段落标记，全部混在同一条旋律上。

    旋律刻意做成 32 个**等间隔四分音符**：MIDI 没有显式 staccato 事件，
    短 note-off 既可能是断奏也可能是「短音符 + 休止」，只有在没有休止的
    等间隔音符流里才能唯一判定，这正是 ExpressionMap 的判据前提。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调，避免五声校正干扰
    t0.add(0, meta(0x06, b"Verse"))
    t0.add(4 * BAR, meta(0x06, b"Chorus"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 54, 0)
    # CC11：第 1~2 小节渐强，第 5~6 小节渐弱；2400 的 95 把两段断开，
    # 否则会被算法连成一整条单调链。
    for tick, val in ((0, 30), (480, 45), (960, 60), (1440, 80), (1920, 100),
                      (2400, 95),
                      (7680, 110), (8160, 90), (8640, 70), (9120, 50), (9600, 25)):
        t1.cc(0, 11, val, tick)

    def bend(tick: int, value: int) -> None:
        # 0xE0 的数据是 14 位：低 7 位在前、高 7 位在后
        t1.add(tick, bytes([0xE0, value & 0x7F, (value >> 7) & 0x7F]))

    # 上滑 / 下滑 / 颤音分别落在第 11、13、15 个音（tick 4800/5760/6720）
    # 的发声区间内；回中事件刻意放到区间之外，免得把单调链折断。
    for tick, v in ((4800, 8192), (4900, 9000), (5000, 10000), (5100, 11000)):
        bend(tick, v)
    bend(5250, 8192)
    for tick, v in ((5760, 8192), (5860, 7400), (5960, 6600), (6060, 5800)):
        bend(tick, v)
    bend(6220, 8192)
    for tick, v in ((6720, 8192), (6800, 10000), (6880, 6500),
                    (6960, 10500), (7040, 6200), (7120, 10000)):
        bend(tick, v)
    bend(7250, 8192)

    scale = ([60, 62, 64, 65, 67, 69, 71, 72,
              72, 71, 69, 67, 65, 64, 62, 60] * 2)
    for i, pitch in enumerate(scale):
        tick = i * 480
        vel = 120 if i % 8 == 3 else 80      # 每 8 个音一个重音
        raw = 413                           # 0.86 门限，DAW 常见默认值
        if i == 1:
            raw = 120                       # 极短 -> 断奏（staccatissimo）
        elif i == 2:
            raw = 210                       # 偏短 -> 断奏（staccato）
        t1.note(0, pitch, vel, tick, raw)

    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    chords = [(48, 52, 55), (53, 57, 60), (55, 59, 62), (53, 57, 60)] * 2
    for bar, chord in enumerate(chords):
        for p in chord:
            t2.note(1, p, 70, bar * BAR, BAR)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


# ---------------------------------------------------------------------------
# 声部 / 乐器标注 + 倚音（装饰音）的混合素材。
#
# 三条含音符的声部，各带不同的 GM 音色号（长笛 / 钢琴 / 原声贝斯），用来验证：
#   1) 页眉的「声部：Vocal·长笛, Piano·钢琴, Bass·原声贝斯」标注；
#   2) MusicXML 的 <score-instrument> 写了对应乐器名；
#   3) 旋律轨（单声部的长笛）被正确判定为主旋律，柱式和弦的钢琴、低音贝斯
#      仍是伴奏，不会抢走旋律身份。
# 旋律第一拍前放一个极短的装饰音（B4，14 tick），紧跟 C5 长音 —— 既验证倚音被
# 判定（渲染成主音前的小字装饰音），又验证它不会被误判成断奏或独立占一格。
# ---------------------------------------------------------------------------
def build_parts() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    # 旋律：单声部长笛（GM 73 -> 长笛），第 1 拍前带一个极短倚音 B4->C5
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 73, 0)
    t1.note(0, 59, 90, 460, 14)                         # 倚音 B4（极短，紧跟 C5）
    melody = [
        (60, 480, 480), (62, 480, 960), (64, 480, 1440), (65, 480, 1920),
        (67, 960, 2400), (69, 480, 3360), (71, 480, 3840),
        (72, 480, 4320), (72, 480, 4800), (76, 960, 5280),
        (74, 480, 6240), (72, 480, 6720), (71, 480, 7200), (69, 960, 7680),
    ]
    for pitch, dur, start in melody:
        t1.note(0, pitch, 90, start, dur)

    # 钢琴：每小节一个柱式和弦（复音数 3，明显不是旋律），GM 0 -> 钢琴
    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    for bar, chord in enumerate([(48, 52, 55), (53, 57, 60),
                                 (48, 52, 55), (53, 57, 60)]):
        for p in chord:
            t2.note(1, p, 70, bar * BAR, BAR)

    # 贝斯：每小节两个长音的单声部低音线，GM 32 -> 原声贝斯
    t3 = Track()
    t3.add(0, meta(0x03, b"Bass"))
    t3.program(2, 32, 0)
    bass = [(48, 0), (48, 960), (43, 1920), (43, 2880),
            (48, 3840), (48, 4800), (43, 5760), (43, 6720)]
    for p, start in bass:
        t3.note(2, p, 80, start, 960)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 4, DIV)
    return header + t0.render() + t1.render() + t2.render() + t3.render()


# ---------------------------------------------------------------------------
# 和弦转位 + 七和弦扩充素材。
#
# 四小节各放一个和声窗（960 tick 一个），刻意只改「低音」和「特征音」，
# 让谱面上能一眼看出转位与和弦类型的区别：
#   第 1 窗  C/E      —— 音级仍是 C E G，但 E 在低音，必须写斜杠
#   第 2 窗  Bm7b5    —— 半减七（B D F A），旧版会只标 Bdim 或干脆不标
#   第 3 窗  G7sus4   —— 挂四七（G C D F），旧版会标成 C sus4 或 G7
#   第 4 窗  C        —— 原位三和弦，用来验证「低音=根音时不写斜杠」
# 旋律用八个上行四分音符（单声部、音域宽），保证旋律轨判定选中它，
# 和弦音全部落在伴奏轨上，不被当成旋律。
# ---------------------------------------------------------------------------
def build_chords() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    # 旋律：C5 起的上行音阶，8 个四分音符 = 2 小节
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)
    for i, p in enumerate((72, 74, 76, 77, 79, 81, 83, 84)):
        t1.note(0, p, 90, i * DIV, DIV)

    # 伴奏：四窗各一个和弦，窗长 960 tick（4/4 的半小节）
    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    windows = [
        (0,    (52, 55, 60)),        # C/E   —— E3 在低音
        (960,  (47, 50, 53, 57)),    # Bm7b5 —— B D F A
        (1920, (43, 48, 50, 53)),    # G7sus4—— G C D F
        (2880, (48, 52, 55)),        # C     —— 原位
    ]
    for start, chord in windows:
        for p in chord:
            t2.note(1, p, 70, start, 960)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


# ---------------------------------------------------------------------------
# 渐快 / 渐慢（tempo ramp）素材。
#
# DAW 里的 rit. 不是一条曲线，而是几十个连续的 set_tempo 事件。这份素材就是
# 按 DAW 的真实写法造的：每半拍一个自动化节点，一段 accel. 13 个点、一段
# rit. 13 个点，中间夹一段恒定速度，最后阶跃回到 120。
#
#   tick        事件
#   0           ♩=100            曲首速度
#   1920..4800  accel 100→140    每 240 tick 一个点（13 个）
#   4800..5760  停留在 140
#   5760..8640  rit 140→80       每 240 tick 一个点（13 个）
#   8640..13440 停留在 80
#   13440       ♩=120            a tempo（阶跃，不是渐变）
#
# 期望：叠成 4 个记号（♩=100 / accel. 100→140 / rit. 140→80 / ♩=120）。
# 旧版会把 28 个点全当变速列出来，页眉挤满「(第2小节) → (第2小节) → …」。
# ---------------------------------------------------------------------------
def build_tempo_ramp() -> bytes:
    def us(bpm: float) -> bytes:
        micro = int(round(60000000.0 / bpm))
        return bytes([(micro >> 16) & 0xFF, (micro >> 8) & 0xFF, micro & 0xFF])

    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    def ramp(start: int, end: int, bpm_from: float, bpm_to: float) -> None:
        """每半拍一个自动化节点，与 DAW 画速度曲线时写出的东西一致。"""
        steps = (end - start) // 240
        for k in range(steps + 1):
            bpm = bpm_from + (bpm_to - bpm_from) * k / steps
            t0.add(start + k * 240, meta(0x51, us(bpm)))

    t0.add(0, meta(0x51, us(100)))                     # 曲首 100
    ramp(1920, 4800, 100.0, 140.0)                     # 渐快
    ramp(5760, 8640, 140.0, 80.0)                      # 渐慢
    t0.add(13440, meta(0x51, us(120)))                 # a tempo（阶跃）

    # 旋律：32 个四分音符（8 小节），C 大调音阶往返，便于肉眼核对小节号
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 73, 0)
    scale = [60, 62, 64, 65, 67, 69, 71, 72]
    for i in range(32):
        oct_up = 12 if (i // 8) % 2 else 0
        t1.note(0, scale[i % 8] + oct_up, 90, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


# ---------------------------------------------------------------------------
# 长音跨小节（延音线）素材。
#
# 简谱的 `-` 是「延长一拍」，跨过小节线就必须在新的一小节继续写 `-`，
# 也就是靠延音线分写。一个 6 拍长音的正确写法是：
#     | 3 - - - | - - 4 5 |
# 而不是「4 拍 + 2 拍休止」。可量化阶段曾把超过全音符的时值钳到 4 拍，
# 长音被截断、剩下的时间变成末尾休止符，谱面上凭空多出一段静音。
#
#   bar  0  1234      四个四分音符
#   bar  1  5671'     四个四分音符
#   bar  2  1' 7 6 -  两拍后接二分
#   bar  3  3 起 6 拍 —— 跨到 bar4 第 2 拍（1 个延音段）
#   bar  4  - - 4 5   前两拍由上一小节的延音线占满
#   bar  5  6 7 1' 2'
#   bar  6  1' 起 10 拍 —— 跨到 bar8 第 2 拍（2 个延音段）
#   bar  7  - - - -
#   bar  8  - - 7 6
#
# 另配一条钢琴柱式和弦轨（每小节一个 4 拍三和弦）：复调轨没有「响到下一
# 音头」兜底，门限还原若作用在 4 拍的和弦上会把它放成 4.75 拍，整段小节线
# 错位。两条轨一起才锁得住「长音不截断 + 长音不放大」。
# ---------------------------------------------------------------------------
def build_long_tie() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # 明确 C 大调，唱名可预期

    BAR = DIV * 4
    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 73, 0)                               # 长笛
    for i, p in enumerate((60, 62, 64, 65)):
        t1.note(0, p, 90, i * DIV, DIV)                # bar0
    for i, p in enumerate((67, 69, 71, 72)):
        t1.note(0, p, 90, BAR + i * DIV, DIV)          # bar1
    t1.note(0, 72, 90, 2 * BAR, DIV)                   # bar2
    t1.note(0, 71, 90, 2 * BAR + DIV, DIV)
    t1.note(0, 69, 90, 2 * BAR + 2 * DIV, 2 * DIV)
    t1.note(0, 64, 90, 3 * BAR, 6 * DIV)               # bar3 起 6 拍，跨到 bar4
    t1.note(0, 65, 90, 4 * BAR + 2 * DIV, DIV)         # bar4 第 3 拍
    t1.note(0, 67, 90, 4 * BAR + 3 * DIV, DIV)
    for i, p in enumerate((69, 71, 72, 74)):
        t1.note(0, p, 90, 5 * BAR + i * DIV, DIV)      # bar5
    t1.note(0, 72, 90, 6 * BAR, 10 * DIV)              # bar6 起 10 拍，跨两小节
    t1.note(0, 71, 90, 8 * BAR + 2 * DIV, DIV)         # bar8 第 3 拍
    t1.note(0, 69, 90, 8 * BAR + 3 * DIV, DIV)

    # 钢琴：每小节一个 4 拍柱式和弦（复调，用来验证长音不被门限放大）
    t2 = Track()
    t2.add(0, meta(0x03, b"Piano"))
    t2.program(1, 0, 0)
    triads = [
        (48, 52, 55), (53, 57, 60), (55, 59, 62), (57, 60, 64), (59, 62, 65),
        (55, 59, 62), (57, 60, 64), (53, 57, 60), (48, 52, 55),
    ]
    for bar, chord in enumerate(triads):
        for p in chord:
            t2.note(1, p, 70, bar * BAR, 4 * DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


# ---------------------------------------------------------------------------
# 同轨复调（一条轨里两个声部）素材。
#
# 钢琴 MIDI 最常见的形态就是左右手写进同一个轨：右手旋律、左手长音铺底，
# 两边同时发声、时值还各不相同。若不分声部，长音和短音会被当成「同一拍的
# 和弦」塞进一格，而 <chord/> 的成员时值不等，小节总时值直接溢出
# （4/4 写成 8 拍），导出的 MusicXML 是有问题的文件。
#
#   bar 0  右手 5 6 7 1'   左手 1,（全音符）
#   bar 1  右手 2' 3' 4' 5'  左手 2,
#   bar 2  右手 6'(2) 5'(2)   左手 3, 从第 3 拍起 4 拍，跨到 bar3 第 2 拍
#   bar 3  - - 4' 3'        左手 5, 从第 3 拍起 2 拍
#
# bar2 那个跨小节音同时锁两件事：声部内延音线（<tie>）、以及换声部时的
# <backup> 不能把上一声部的尾巴吃掉。
# ---------------------------------------------------------------------------
def build_polyphony() -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    BAR = DIV * 4
    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)                                # 钢琴
    # 右手（高音声部）
    for i, p in enumerate((72, 74, 76, 77)):
        t1.note(0, p, 90, i * DIV, DIV)                # bar0
    for i, p in enumerate((79, 81, 83, 84)):
        t1.note(0, p, 90, BAR + i * DIV, DIV)          # bar1
    t1.note(0, 86, 90, 2 * BAR, 2 * DIV)               # bar2
    t1.note(0, 84, 90, 2 * BAR + 2 * DIV, 2 * DIV)
    t1.note(0, 83, 90, 3 * BAR + 2 * DIV, DIV)         # bar3 第 3 拍
    t1.note(0, 81, 90, 3 * BAR + 3 * DIV, DIV)
    # 左手（低音声部，与右手同时进行）
    t1.note(0, 48, 70, 0, 4 * DIV)                     # bar0 全音符
    t1.note(0, 50, 70, BAR, 4 * DIV)                   # bar1
    t1.note(0, 52, 70, 2 * BAR + 2 * DIV, 4 * DIV)     # bar2 第 3 拍起 4 拍（跨小节）
    t1.note(0, 55, 70, 3 * BAR + 2 * DIV, 2 * DIV)     # bar3 第 3 拍

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_smpte() -> bytes:
    """SMPTE 时间基准（division 高字节为负）的素材。

    25 fps x 40 tick = 1000 tick/秒；曲首 120BPM 时一拍正好 500 tick。
    旧实现把 0xE728 当无符号读成 59176，16 拍会被压进 1 小节。
    第 9 拍另插一个 set_tempo 400BPM：SMPTE 下 tempo 不改变绝对时间，
    解析层必须把它归一掉，否则后半程的秒数会被二次缩放。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C 大调

    TPB = 500                                          # 每拍 500 tick
    t1 = Track()
    t1.add(0, meta(0x03, b"Smpte Vocal"))
    t1.add(8 * TPB, meta(0x51, bytes([0x02, 0x49, 0xF0])))   # 150000us -> 400BPM
    for i, p in enumerate((60, 62, 64, 65, 67, 69, 71, 72,
                           72, 71, 69, 67, 65, 64, 62, 60)):
        t1.note(0, p, 96, i * TPB, TPB)

    # 0xE7 = -25 fps，0x28 = 40 tick/frame
    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, 0xE728)
    return header + t0.render() + t1.render()


def build_format2() -> bytes:
    """format 2 素材：两条轨是两段互相独立的音乐，不是同一首的两个声部。

    SongA 4 小节 16 个四分音符，SongB 只有 2 小节 8 个音。旧实现把它们并到
    同一条时间轴上，两段小节线对不齐；现在只转 SongA，其余轨丢弃。
    """
    a = Track()
    a.add(0, meta(0x03, b"SongA"))
    for i, p in enumerate((60, 62, 64, 65, 67, 69, 71, 72,
                           72, 71, 69, 67, 65, 64, 62, 60)):
        a.note(0, p, 96, i * DIV, DIV)

    b = Track()
    b.add(0, meta(0x03, b"SongB"))
    for i, p in enumerate((72, 71, 69, 67, 65, 64, 62, 60)):
        b.note(0, p, 96, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 2, 2, DIV)
    return header + a.render() + b.render()


MELODY = (60, 62, 64, 65, 67, 69, 71, 72, 72, 71, 69, 67, 65, 64, 62, 60)


def build_aftertouch() -> bytes:
    """复音触后（0xA0）素材：每个音中间插一个 3 字节的触后事件。

    0xA0 是「状态 + 音高 + 压力」，旧实现落到兜底分支只跳 1 字节，遇到第一个
    触后事件就和事件流错位，16 个音的轨最后连一个音都提不出来（连「只有鼓轨」
    的误报都不是，直接报「未提取到音符」）。1000 tick 一个音，4 小节 4/4。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4

    t1 = Track()
    t1.add(0, meta(0x03, b"Aftertouch Vocal"))
    for i, p in enumerate(MELODY):
        t1.note(0, p, 96, i * DIV, DIV)
        t1.add(i * DIV + DIV // 2, bytes([0xA0, p, 0x50]))

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_retrigger() -> bytes:
    """同音重触发素材：一个键还没抬手就又按下。

    第 1 拍按下 C4 之后，第 2 拍又按了一次 C4（前一个音没有 note-off），
    直到第 3 拍才抬手。旧实现用 active[key] 覆盖，第一个 C4 凭空消失，
    8 个音只剩 7 个；现在前一个音在重触发点收尾，两个音都在。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Retrig Vocal"))
    t1.add(0, bytes([0x90, 60, 96]))
    t1.add(DIV, bytes([0x90, 60, 64]))            # 重触发，前一个音没有 note-off
    t1.add(DIV * 2, bytes([0x80, 60, 0x40]))
    for i, p in enumerate((64, 65, 67, 69, 71, 72)):
        t1.note(0, p, 96, (i + 2) * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_dangling() -> bytes:
    """轨尾悬挂音素材：最后一个音只按不抬，轨就结束了。

    旧实现把还按着的音整条丢掉：16 个音的 C 大调音阶只剩 15 个，少掉的那个
    C 还让调性被猜成 F / D 小调。现在按本轨最后一个事件补收。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    # 故意不写 0x59 调号：少掉的那个音过去会把自动猜调带偏成 F / D 小调，
    # 写了调号就掩盖了这个问题。

    t1 = Track()
    t1.add(0, meta(0x03, b"Dangling Vocal"))
    for i in range(15):
        t1.note(0, MELODY[i], 96, i * DIV, DIV)
    t1.add(15 * DIV, bytes([0x90, MELODY[15], 96]))    # 只按不抬
    t1.add(16 * DIV, meta(0x01, b"end"))               # 轨尾，没有 note-off

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_twolyrics() -> bytes:
    """双歌词轨素材：第一段词、第二段词各占一条轨。

    第二段词错开半拍（卡拉 OK 文件里两段词的时间本来就不重合）。旧实现按 tick
    把所有轨的 0x05 合并成一行，得到「春夜来风雨声花落」这种交错乱码，音节数
    也从 10 变成 20。现在只取音节最多的那条轨，其余整轨忽略。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    for i, p in enumerate(MELODY[:8]):
        t1.note(0, p, 96, i * DIV, DIV)
    for i, ch in enumerate("春眠不觉晓处处闻啼鸟"):
        t1.add(i * DIV, meta(0x05, ch.encode("utf-8")))

    t2 = Track()
    t2.add(0, meta(0x03, b"Lyric2"))
    for i, ch in enumerate("夜来风雨声花落知多少"):
        t2.add(i * DIV + DIV // 2, meta(0x05, ch.encode("utf-8")))

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


def build_pedal() -> bytes:
    """CC64 延音踏板素材：琴键只按 1/4 拍，踏板一直踩到第 8 拍。

    钢琴 MIDI 里非常常见的写法——DAW 把 note-off 紧贴 note-on 导出，真正的
    音响长度由踏板决定。旧实现只看按键时长，得到「1= 3= 5= 0 - - -」：
    三个十六分音符加一长串休止，完全不是听感。现在把每个音补到踏板抬起
    （或同键重新击打，本素材第 4 拍正好重按一次），两小节各成一个
    4 拍的持续和弦。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.cc(0, 64, 127, 0)                     # 踩下
    t1.cc(0, 64, 0, 8 * DIV)                 # 第 8 拍抬起
    for bar in range(2):
        for pitch in (60, 64, 67):           # C 大三和弦
            t1.note(0, pitch, 96, bar * 4 * DIV, DIV // 4)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_allnotesoff() -> bytes:
    """CC123 All Notes Off 素材：导出器用清音代替 note-off。

    第一个音只按不抬，tick=480 写一条 CC123 强制清音。旧实现不认这个控制器，
    那个音会被轨尾补收成横跨整首曲子的幽灵长音，还因此把谱面拆成两个假声部。
    现在把它截到 CC123 所在 tick（正好 1 拍）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.add(0, bytes([0x90, 60, 96]))         # 只按不抬
    t1.cc(0, 123, 0, DIV)                    # All Notes Off
    for i, pitch in enumerate((62, 64, 65, 67, 69, 71)):
        t1.note(0, pitch, 96, (i + 2) * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_cc7volume() -> bytes:
    """CC7 主音量素材：整首渐强，一个 CC11 都不写。

    GM 里 CC7 是主音量、CC11 是表情，两者都是强弱来源，但旧实现只认
    CC11 —— 这类文件转出来一个力度记号都没有。现在 CC7 并入力度表，
    同一 (tick, 通道) 若 CC11 也在则让位给表情（避免凭空多一档）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Strings"))
    scale = (60, 62, 64, 65, 67, 69, 71, 72)
    for i, pitch in enumerate(scale):
        t1.cc(0, 7, 20 + i * 14, i * DIV)   # 20 → 118，跨好几档
        t1.note(0, pitch, 96, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_pedal_channels() -> bytes:
    """踏板按通道隔离素材：通道 0 踩踏板，通道 1 从头到尾没踩。

    踏板是通道级控制器，不能一踩就给全曲所有乐器补时值。同时验证
    「踏板抬起之后才松键的音」不该被补长。
    预期四个音的时值：1920 / 120 / 120 / 120 tick。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.cc(0, 64, 127, 0)
    t1.cc(0, 64, 0, 4 * DIV)
    t1.note(0, 60, 96, 0, DIV // 4)          # 踏板内抬手 → 补到 4 拍
    t1.note(0, 64, 96, 4 * DIV, DIV // 4)    # 抬踏板之后松键 → 保持 1/4 拍

    t2 = Track()
    t2.add(0, meta(0x03, b"Bass"))
    t2.note(1, 48, 96, 2 * DIV, DIV // 4)    # 通道 1 没踩踏板 → 保持 1/4 拍
    t2.note(1, 55, 96, 6 * DIV, DIV // 4)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


def build_pedal_hysteresis() -> bytes:
    """CC64 踏板迟滞素材：连续踏板包络 127 → 50 → 0。

    真实踏板（尤其连续量输出）松开时不会一步从 127 蹦到 0，而是在中间值
    停留——50 恰好落在迟滞死区 33..63。旧实现用二值 value>=64 判定，50 一出现
    就当「抬起」，音符只延到 1 拍；正确的迟滞判定让 50 保持踩下、音延到真正的
    释放点 0（第 2 拍）。一个音 C4 只按 1/4 拍，踏板 0 踩、480 半踩、960 松。
    预期：补长 1 个音（3 个踏板事件）、音符时值 960 tick（2 拍）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.cc(0, 64, 127, 0)                     # 踩下
    t1.cc(0, 64, 50, DIV)                    # 死区中间值：迟滞后仍算踩着
    t1.cc(0, 64, 0, 2 * DIV)                 # 第 2 拍真正松开
    t1.note(0, 60, 96, 0, DIV // 4)          # C4 只按 1/4 拍

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_bendrange_channels() -> bytes:
    """RPN 弯音范围按通道隔离：通道 0 设 ±12，通道 1 仍是默认 ±2。

    两个通道都把弯音轮推到顶（16383）。旧实现把范围压成一个全局量，
    「最后一个 RPN 覆盖全曲」，通道 1 的 2 半音颤音被放大成 12 半音滑音。
    预期 MusicXML 音高：C5（通道 0）、D4（通道 1）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    lead = Track()
    lead.add(0, meta(0x03, b"Lead"))
    lead.bend_range(0, 12, 0)
    lead.note(0, 60, 96, 0, DIV)
    lead.bend(0, 16383, 0)
    lead.bend(0, 8192, DIV + 1)

    flute = Track()
    flute.add(0, meta(0x03, b"Flute"))
    flute.note(1, 60, 96, 2 * DIV, DIV)
    flute.bend(1, 16383, 2 * DIV)
    flute.bend(1, 8192, 3 * DIV + 1)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + lead.render() + flute.render()


def build_bendrange_timeline() -> bytes:
    """同一通道中途改范围：0 tick 设 ±12，960 tick 改回 ±2。

    旧实现只存最终值 2，会把它之前的那个音也算成 +2 半音（倒灌）。
    预期 MusicXML 音高：C5（前）、D4（后）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    lead = Track()
    lead.add(0, meta(0x03, b"Lead"))
    lead.bend_range(0, 12, 0)
    lead.note(0, 60, 96, 0, DIV)
    lead.bend(0, 16383, 0)
    lead.bend(0, 8192, DIV + 1)
    lead.bend_range(0, 2, 2 * DIV)
    lead.note(0, 60, 96, 2 * DIV, DIV)
    lead.bend(0, 16383, 2 * DIV)
    lead.bend(0, 8192, 3 * DIV + 1)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + lead.render()


def build_bendrange_crosstrack() -> bytes:
    """RPN 写在文件顺序靠后的 setup 轨上，弯音写在最前面的旋律轨里。

    真实工程常把控制器集中在 setup / conductor 轨；按解析顺序即时套用
    （或让 RPN 状态每轨重置）都会漏掉它。预期 MusicXML 音高仍为 C5。
    """
    lead = Track()
    lead.add(0, meta(0x03, b"Lead"))
    lead.note(0, 60, 96, 0, DIV)
    lead.bend(0, 16383, 0)
    lead.bend(0, 8192, DIV + 1)

    setup = Track()
    setup.add(0, meta(0x03, b"Setup"))
    setup.bend_range(0, 12, 0)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + lead.render() + setup.render()


def _running_melody(middle: bytes, tail_full_status: bool = False) -> Track:
    """C4 八分 → 中间系统事件 → D4 八分；后半段刻意省略状态字节。

    Track.add 原样写 raw，不做任何状态补全，所以这里能造出真实导出器
    才会写的「省略 status」字节流。
    """
    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    t.add(0, bytes([0x90, 60, 100]))               # C4 on，完整 status
    t.add(DIV // 2, bytes([60, 0]))                 # C4 off，省略 status
    t.add(DIV // 2, middle)                         # 同 tick 插入系统事件
    t.add(DIV, bytes([0x90, 62, 100]) if tail_full_status else bytes([62, 100]))
    t.add(DIV + DIV // 2,
          bytes([0x90, 62, 0]) if tail_full_status else bytes([62, 0]))
    return t


def _running_smf(t: Track) -> bytes:
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) + t0.render() + t.render()


def build_running_meta() -> bytes:
    """歌词 meta 之后仍省略状态字节。旧实现把 0xFF 当成 running status，
    把 D4 的音高 62 读成 meta 类型、力度 100 读成 meta 长度，整段音符丢失。"""
    return _running_smf(_running_melody(meta(0x05, b"a")))


def build_running_sysex() -> bytes:
    """SysEx（0xF0 … 0xF7）之后省略状态字节，同样按上一条通道状态补回。"""
    return _running_smf(_running_melody(b"\xF0\x05\x7E\x7F\x09\x01\xF7"))


def build_running_realtime() -> bytes:
    """0xF8 MIDI clock 插在音符流中间：规范明确不改变 running status，
    后面的音符无需补回也应正常读到。"""
    return _running_smf(_running_melody(b"\xF8"))


def build_running_standard() -> bytes:
    """零回归基线：meta 之后规规矩矩重写完整状态字节的规范写法。"""
    return _running_smf(_running_melody(meta(0x05, b"a"), tail_full_status=True))


# ---- MIDI Port（FF 21）与 Channel Prefix（FF 20）----
# 端口是 (port, channel) 二元组的一半：port 0 ch0 与 port 1 ch0 是两台设备
# 上的两件乐器。旧实现只读低 4 位通道号，踏板 / 弯音范围 / 力度 / 音色全串。
def _port_smf(tracks) -> bytes:
    return (b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), DIV)
            + b"".join(t.render() for t in tracks))


def _port_conductor() -> Track:
    t = Track()
    t.add(0, meta(0x03, b"Conductor"))
    t.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t.add(0, meta(0x59, bytes([0, 0])))
    return t


def build_port_pedal() -> bytes:
    """port 0 ch0 踩踏板；port 1 ch0 不踩；第三条轨不写端口（应重置回 0）。

    三个音错开一拍、音高各不同，都在踏板区间 [0, 4 拍) 内。
    正确：C4 补长到 4 拍、D4 保持 1 拍、E4 补长到踏板抬起；
    错误：三个音全被补长（全被当成 channel 0）。
    """
    p0 = Track()
    p0.add(0, meta(0x21, bytes([0])))
    p0.add(0, meta(0x03, b"Piano port 0"))
    p0.cc(0, 64, 127, 0)
    p0.note(0, 60, 96, 0, DIV)
    p0.cc(0, 64, 0, 4 * DIV)

    p1 = Track()
    p1.add(0, meta(0x21, bytes([1])))
    p1.add(0, meta(0x03, b"Flute port 1"))
    p1.note(0, 62, 96, DIV, DIV)

    inherited = Track()
    inherited.add(0, meta(0x03, b"Pad no port meta"))
    inherited.note(0, 64, 96, 2 * DIV, DIV)
    return _port_smf([_port_conductor(), p0, p1, inherited])


def build_port_bend() -> bytes:
    """port 0 ch0 设 RPN ±12 半音；port 1 ch0 用默认 ±2，两者都推满弯音轮。

    正确：C4 -> C5、D4 -> E4；错误：两者都按 ±12 变成 C5 / D5。
    """
    p0 = Track()
    p0.add(0, meta(0x21, bytes([0])))
    p0.add(0, meta(0x03, b"Lead port 0"))
    p0.bend_range(0, 12, 0)
    p0.note(0, 60, 96, 0, DIV)
    p0.bend(0, 16383, 0)
    p0.bend(0, 8192, DIV + 1)

    p1 = Track()
    p1.add(0, meta(0x21, bytes([1])))
    p1.add(0, meta(0x03, b"Flute port 1"))
    p1.note(0, 62, 96, 2 * DIV, DIV)
    p1.bend(0, 16383, 2 * DIV)
    p1.bend(0, 8192, 3 * DIV + 1)
    return _port_smf([_port_conductor(), p0, p1])


def _port_prefix(with_prefix: bool) -> bytes:
    t = Track()
    if with_prefix:
        t.add(0, meta(0x20, bytes([2])))
    t.add(0, meta(0x03, b"Prefix"))
    t.cc(0, 11, 40, 0)
    t.note(0, 60, 96, 0, DIV)
    t.note(0, 62, 96, DIV, DIV)
    return _port_smf([_port_conductor(), t])


def build_port_prefix() -> bytes:
    """FF 20 按规范只作用于后续 meta / SysEx，不能把 ch0 音重定向到 ch2。"""
    return _port_prefix(True)


def build_port_prefix_base() -> bytes:
    """同一份音乐但不写 FF 20：两份输出应逐字节一致。"""
    return _port_prefix(False)


def build_port_standard() -> bytes:
    """全程不写端口声明的单通道踏板：行为必须与旧版逐字节一致。"""
    t = Track()
    t.add(0, meta(0x03, b"Piano"))
    t.cc(0, 64, 127, 0)
    t.note(0, 60, 96, 0, DIV)
    t.cc(0, 64, 0, 4 * DIV)
    t.note(0, 62, 96, DIV, DIV)
    return _port_smf([_port_conductor(), t])



# ---- SysEx 鼓模式（Roland GS / Yamaha XG）----
# GS/XG 可以用 0xF0 SysEx 把任意 part 指定为节奏声部（鼓），也可以把通道 10
# 改回普通乐器；GM/GS/XG Reset 则恢复「只有通道 10 是鼓」的默认设定。旧实现
# 把 SysEx 整段跳过、只按「通道 9 是鼓」硬判定，于是 GS/XG 文件里被设成鼓的
# 通道会输出一串 `1,, 2,, #4,,` 假旋律，还会因为音符最多抢走旋律轨身份。
def sysex(payload: bytes) -> bytes:
    """把完整 SysEx（F0 ... F7）包成 SMF 的 F0 <len> <payload> 事件。"""
    return b"\xF0" + vlq(len(payload)) + payload


def _gs_checksum(addr_data) -> int:
    return (128 - (sum(addr_data) & 0x7F)) & 0x7F


def gs_part_mode(ch: int, mode: int) -> bytes:
    """F0 41 10 42 12 40 1n 15 mm cs F7 : GS 'Use for Rhythm Part'。"""
    addr = [0x40, 0x10 + (ch & 0x0F), 0x15]
    return sysex(bytes([0xF0, 0x41, 0x10, 0x42, 0x12] + addr +
                       [mode & 0x7F, _gs_checksum(addr + [mode & 0x7F]), 0xF7]))


def xg_part_mode(ch: int, mode: int) -> bytes:
    """F0 43 10 4C 08 pp 07 mm F7 : XG 'Part Mode' 参数变更。"""
    return sysex(bytes([0xF0, 0x43, 0x10, 0x4C, 0x08, ch & 0x0F,
                        0x07, mode & 0x7F, 0xF7]))


GS_RESET = sysex(bytes([0xF0, 0x41, 0x10, 0x42, 0x12,
                        0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7]))
XG_RESET = sysex(bytes([0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7]))
GM_RESET = sysex(bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]))
OTHER_SYSEX = sysex(bytes([0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7]))


def _sysex_conductor(*events) -> Track:
    t = _port_conductor()
    for tick, raw in events:
        t.add(tick, raw)
    return t


def _sysex_drums(ch: int = 0) -> Track:
    """36 底鼓 / 38 军鼓 / 42 闭镲：当旋律转出来就是一串低音垃圾。"""
    t = Track()
    t.add(0, meta(0x03, b"Drums"))
    for i, p in enumerate([36, 38, 42, 36, 38, 42, 36, 38]):
        t.note(ch, p, 100, i * (DIV // 2), DIV // 4)
    return t


def _sysex_melody(ch: int = 1) -> Track:
    t = Track()
    t.add(0, meta(0x03, b"Melody"))
    for i, p in enumerate([60, 62, 64, 65, 67, 65, 64, 62]):
        t.note(ch, p, 90, i * (DIV // 2), DIV // 2 - 20)
    return t


def _sysex_ch9_melody() -> Track:
    t = Track()
    t.add(0, meta(0x03, b"Ch9 Piano"))
    for i, p in enumerate([60, 62, 64, 65, 67, 65, 64, 62]):
        t.note(9, p, 90, i * (DIV // 2), DIV // 2 - 20)
    return t


def build_sysex_gs_drum() -> bytes:
    """GS 把 ch0 设成鼓：8 个鼓件音必须丢，8 个旋律音必须留。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(0, 1))),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_xg_drum() -> bytes:
    """同一首曲子改用 Yamaha XG 的 Part Mode 设定，结果应完全一致。"""
    return _port_smf([_sysex_conductor((0, xg_part_mode(0, 1))),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_gs_normal() -> bytes:
    """mode=0 是「普通乐器」：对照基线，16 个音一个都不能少。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(0, 0))),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_gs_reset() -> bytes:
    """tick 960 发 GS Reset：前 4 个鼓件音丢、后 4 个恢复成普通乐器。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(0, 1)),
                                       (2 * DIV, GS_RESET)),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_gm_reset() -> bytes:
    """GM System On 同样把音源恢复默认。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(0, 1)),
                                       (2 * DIV, GM_RESET)),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_xg_reset() -> bytes:
    """XG System On 同样把音源恢复默认。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(0, 1)),
                                       (2 * DIV, XG_RESET)),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_conductor() -> bytes:
    """SysEx 写在文件最后一条轨（tick 仍是 0）：解析顺序不能决定时间顺序。"""
    return _port_smf([_sysex_drums(0), _sysex_melody(1),
                      _sysex_conductor((0, gs_part_mode(0, 1)))])


def build_sysex_ch9_normal() -> bytes:
    """GS 把通道 10 改回普通钢琴：ch9 上的 8 个旋律音必须全部留下。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(9, 0))),
                      _sysex_ch9_melody(), _sysex_melody(1)])


def build_sysex_ch9_drum() -> bytes:
    """显式把通道 10 设为鼓：与默认判定一致，8 个音仍应丢掉。"""
    return _port_smf([_sysex_conductor((0, gs_part_mode(9, 1))),
                      _sysex_ch9_melody(), _sysex_melody(1)])


def build_sysex_unknown() -> bytes:
    """无关 SysEx 零回归：识别不了就不能改动任何音符。"""
    return _port_smf([_sysex_conductor((0, OTHER_SYSEX)),
                      _sysex_drums(0), _sysex_melody(1)])


def build_sysex_port_drum() -> bytes:
    """port 1 的 ch0 设鼓，port 0 的 ch0 仍是旋律：鼓模式也按端口隔离。"""
    p0 = Track()
    p0.add(0, meta(0x03, b"Port0 Melody"))
    for i, p in enumerate([60, 62, 64, 65]):
        p0.note(0, p, 90, i * DIV, DIV - 80)
    p1 = Track()
    p1.add(0, meta(0x03, b"Port1 Drums"))
    p1.add(0, meta(0x21, bytes([1])))
    p1.add(0, gs_part_mode(0, 1))
    for i, p in enumerate([36, 38, 42, 36]):
        p1.note(0, p, 100, i * DIV, 200)
    return _port_smf([_port_conductor(), p0, p1])



# ---- Bank Select / Program Change 时间轴（Yamaha XG 鼓组库）----
# CC0 Bank MSB = 127 是 XG 的鼓组库：任意 part 都能被切成节奏声部，和
# GS/XG SysEx 的 Part Mode 是并列的两条路径。旧实现把 CC0 / CC32 当普通
# 两字节跳过，鼓件音高就变成了 `1,, 2,, #4,,` 假旋律。0xC0 过去也只往
# channelProgram 写最后一个音色号，中途换乐器时前半首的声部会被错标。
def _bank_events(track: Track, tick: int, ch: int, msb: int,
                 lsb: int = 0, program: int = 0) -> None:
    """CC0 + CC32 + 0xC0 三个独立 SMF 事件，每个都自带 delta-time。"""
    track.cc(ch, 0, msb, tick)
    track.cc(ch, 32, lsb, tick)
    track.program(ch, program, tick)


def _bank_setup(tick: int, ch: int, msb: int, name: bytes = b"XG Setup") -> Track:
    t = Track()
    t.add(0, meta(0x03, name))
    _bank_events(t, tick, ch, msb)
    return t


def _bank_piano_ch0() -> Track:
    t = Track()
    t.add(0, meta(0x03, b"Ch0 Piano"))
    for i, p in enumerate([60, 62, 64, 65, 67, 65, 64, 62]):
        t.note(0, p, 90, i * (DIV // 2), DIV // 2 - 20)
    return t


def build_bank_xg_drum() -> bytes:
    """CC0=127 把 ch0 设成 XG 鼓组：8 个鼓件音丢，8 个旋律音留。"""
    return _port_smf([_port_conductor(), _bank_setup(0, 0, 0x7F),
                      _sysex_drums(0), _sysex_melody(1)])


def build_bank_normal() -> bytes:
    """CC0=0 的普通音色库：对照基线，16 个音一个都不能少。"""
    return _port_smf([_port_conductor(), _bank_setup(0, 0, 0x00, b"GM Setup"),
                      _bank_piano_ch0(), _sysex_melody(1)])


def build_bank_switch_back() -> bytes:
    """tick 960 把 CC0 改回 0：前半鼓件丢，后半 4 个旋律音留。"""
    setup = Track()
    setup.add(0, meta(0x03, b"Bank Switch"))
    _bank_events(setup, 0, 0, 0x7F)
    _bank_events(setup, 2 * DIV, 0, 0x00)
    t = Track()
    t.add(0, meta(0x03, b"Ch0 Split"))
    for i, p in enumerate([36, 38, 42, 36]):
        t.note(0, p, 100, i * (DIV // 2), DIV // 4)
    for i, p in enumerate([60, 62, 64, 65]):
        t.note(0, p, 90, 2 * DIV + i * (DIV // 2), DIV // 2 - 20)
    return _port_smf([_port_conductor(), setup, t, _sysex_melody(1)])


def build_bank_conductor() -> bytes:
    """Bank 设定写在文件最后一条轨（tick 0）：解析顺序不能压过时间顺序。"""
    return _port_smf([_port_conductor(), _sysex_drums(0), _sysex_melody(1),
                      _bank_setup(0, 0, 0x7F, b"Late Setup")])


def build_bank_port() -> bytes:
    """FF 21 port 1 的 ch0 设鼓组，port 0 的 ch0 仍是旋律。"""
    p0 = Track()
    p0.add(0, meta(0x03, b"Port0 Ch0"))
    for i, p in enumerate([60, 62, 64, 65, 67, 65, 64, 62]):
        p0.note(0, p, 90, i * (DIV // 2), DIV // 2 - 20)
    p1 = Track()
    p1.add(0, meta(0x03, b"Port1 Ch0"))
    p1.add(0, meta(0x21, bytes([1])))
    for i, p in enumerate([36, 38, 42, 36, 38, 42, 36, 38]):
        p1.note(0, p, 100, i * (DIV // 2), DIV // 4)
    late = Track()
    late.add(0, meta(0x21, bytes([1])))
    _bank_events(late, 0, 0, 0x7F)
    return _port_smf([_port_conductor(), p0, p1, late])


def build_bank_reset() -> bytes:
    """GM Reset 撤销鼓组：只有 Reset 之前的 2 个鼓件音该丢。"""
    setup = Track()
    setup.add(0, meta(0x03, b"Reset Setup"))
    _bank_events(setup, 0, 0, 0x7F)
    setup.add(DIV, GM_RESET)
    return _port_smf([_port_conductor(), setup, _bank_piano_ch0(),
                      _sysex_melody(1)])


def build_bank_ch9_plain() -> bytes:
    """通道 10 发普通 Bank Select + 0xC0：默认鼓通道不能被改回旋律。"""
    t = Track()
    t.add(0, meta(0x03, b"Ch9 Drums"))
    _bank_events(t, 0, 9, 0x00)
    for i, p in enumerate([36, 38, 42, 36]):
        t.note(9, p, 100, i * (DIV // 2), DIV // 4)
    return _port_smf([_port_conductor(), t, _sysex_melody(1)])


def build_bank_no_program() -> bytes:
    """只有 CC0=127、没有随后的 0xC0：Bank Select 只是预备状态，不切鼓。"""
    t = Track()
    t.add(0, meta(0x03, b"No Program"))
    t.cc(0, 0, 0x7F, 0)
    for i, p in enumerate([60, 62, 64, 65]):
        t.note(0, p, 90, i * (DIV // 2), DIV // 2 - 20)
    return _port_smf([_port_conductor(), t, _sysex_melody(1)])


def build_program_timeline() -> bytes:
    """ch0 前半钢琴 prog 0、后半小提琴 prog 40：声部乐器名按首音时刻取。"""
    tl = Track()
    tl.add(0, meta(0x03, b"Lead"))
    tl.program(0, 0, 0)
    for i, p in enumerate([60, 62, 64, 65]):
        tl.note(0, p, 90, i * (DIV // 2), DIV // 2 - 20)
    tl.program(0, 40, 2 * DIV)
    for i, p in enumerate([67, 69, 71, 72]):
        tl.note(0, p, 90, 2 * DIV + i * (DIV // 2), DIV // 2 - 20)
    return _port_smf([_port_conductor(), tl, _sysex_melody(1)])


def build_diag_consistency() -> bytes:
    """--list-tracks 诊断一致性：中途换音色 + XG 鼓组同曲（v1.37.0）。

    Lead 轨 ch1 前半钢琴(prog 0)、后半小提琴(prog 40)：诊断与谱面都必须
    标「钢琴」，旧实现读 channelProgram 末值会报「小提琴」。
    XG Part Mode 把 ch0 设成鼓组，Drums 轨 8 个鼓件音必须被标成「鼓轨
    (跳过 8 鼓件音)」，旧实现漏记按轨鼓件音、只显示「旋律 0 音」。
    """
    lead = Track()
    lead.add(0, meta(0x03, b"Lead"))
    lead.program(1, 0, 0)
    for i, p in enumerate([60, 62, 64, 65]):
        lead.note(1, p, 90, i * (DIV // 2), DIV // 2 - 20)
    lead.program(1, 40, 4 * DIV)
    for i, p in enumerate([67, 69, 71, 72]):
        lead.note(1, p, 90, 4 * DIV + i * (DIV // 2), DIV // 2 - 20)
    return _port_smf([_sysex_conductor((0, xg_part_mode(0, 1))),
                      _sysex_drums(0), lead, _sysex_melody(2)])



def build_sostenuto() -> bytes:
    """CC66 持音踏板：只保持踩下那一刻正按着的键（v1.39.0）。

    真钢琴的中踏板（Sostenuto）与右踏板（CC64）语义相反：CC64 是「抬手时
    踩着就延长」，会把踩下之后新弹的音也留住；CC66 是「踩下时已按着的键
    被保持」，之后新弹的音照常结束。旧实现把 0xB0 里的 CC66 当普通两字节
    跳过，和弦音只写 1 拍，谱面缺掉整个持续声部。

    结构：C4/E4/G4 在 tick 0 按下 -> tick 240 踩 CC66 -> tick 480 抬手，
    同 tick 新弹 D4（tick 960 抬手）-> tick 1920 抬 CC66。
    预期：三个和弦音 1920 tick（whole），D4 仍是 480 tick（quarter）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    for pit in (60, 64, 67):
        t1.add(0, bytes([0x90, pit, 96]))          # 和弦先按下
    t1.cc(0, 66, 127, 240)                        # 踩下 Sostenuto
    for pit in (60, 64, 67):
        t1.add(480, bytes([0x80, pit, 0x40]))     # 琴键抬手
    t1.note(0, 62, 96, 480, 480)                  # 之后新弹的音，不该被保持
    t1.cc(0, 66, 0, 1920)                         # 抬起 Sostenuto

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_dyn_channels() -> bytes:
    """CC11 渐强渐弱记号的通道归属（v1.40.0）。

    旧实现把 CC11 渐变按通道分组识别，却把生成的 ExpressionMark 挤进
    一条不区分通道的 marks_：同 tick 的「旋律渐弱」与「伴奏渐强」合并时
    后写覆盖先写，谱面于是印 [cresc.]，同一行力度却写 [ff]→[pp]。
    正确行为：旋律谱只印 [dim.]，全局段落名 [Verse] 照常显示；页眉的
    「渐强 1 · 渐弱 1」是整份文件的检测统计，两条都应保留。

    结构：2 小节，ch0 八个四分音符 + CC11 120→10，ch1 两个全音符 +
    CC11 20→127，conductor 轨写一个 0x06 段落名。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))
    t0.add(0, meta(0x06, b"Verse"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    for i in range(8):
        t1.note(0, 60 + (i % 4), 96, i * DIV, DIV)
    t1.note(1, 48, 64, 0, BAR)
    t1.note(1, 48, 64, BAR, BAR)

    down = (120, 100, 80, 60, 40, 20, 10)
    up = (20, 40, 60, 80, 100, 120, 127)
    for i in range(7):
        t1.cc(0, 11, down[i], i * 240)
        t1.cc(1, 11, up[i], i * 240)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_wedge_tracks() -> bytes:
    """通道级 wedge 不再只印第一声部（v1.43.0）。

    v1.40.0 已让 ExpressionMark 带 channel，但三个输出层仍把 wedge 与全局
    段落名一起塞进「只印第一声部」的门控：文本谱 if (chords)、HTML
    if (showTempo)、MusicXML if (partIdx == 1)。于是第二条轨（乃至同轨的
    第二声部）自己的 cresc. 连同 wedge stop 一起消失，和注释写的「按通道
    各自写」正好相反。正确行为：两行谱 / 两个 part 各写自己的渐变，Verse
    这种全局 marker 仍只写一次。

    结构：2 小节；conductor 轨写 0x06 段落名 Verse；Vocal 轨 ch0 八个四分
    音符 + CC11 120→10（渐弱）；Pad 轨 ch1 两个全音符 + CC11 20→127（渐强）。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))
    t0.add(0, meta(0x06, b"Verse"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    for i in range(8):
        t1.note(0, 60 + (i % 4), 96, i * DIV, DIV)
    down = (120, 100, 80, 60, 40, 20, 10)
    for i, v in enumerate(down):
        t1.cc(0, 11, v, i * 240)

    t2 = Track()
    t2.add(0, meta(0x03, b"Pad"))
    t2.note(1, 48, 64, 0, BAR)
    t2.note(1, 48, 64, BAR, BAR)
    up = (20, 40, 60, 80, 100, 120, 127)
    for i, v in enumerate(up):
        t2.cc(1, 11, v, i * 240)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIV)
    return header + t0.render() + t1.render() + t2.render()


def build_soft_pedal() -> bytes:
    """CC67 软踏板只衰减踩下期间起音音符的力度（v1.41.0）。

    旧实现把 CC67 当普通控制器跳过：踩不踩软踏板，谱面逐字节相同。
    真钢琴踩左踏板是击弦变轻，按键时长不变，所以正确行为是只改 velocity：
    重音记号、力度变化分析、网页播放用的音符 JSON 都会跟着变，时值不动。

    结构：2 小节 8 个四分音符，力度 60/110/60/110/60/60/60/60 —— 两处
    110 本该各印一个 '>'。软踏板只在 [1200, 2000) 踩下，第 4 个音
    (tick 1440, vel 110) 与第 5 个音 (tick 1920, vel 60) 落在区间内，
    110→77、60→42，于是第 2 个 '>' 消失，谱面只剩 1 个。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    vels = (60, 110, 60, 110, 60, 60, 60, 60)
    for i, v in enumerate(vels):
        t1.note(0, 60 + i, v, i * DIV, 400)
    t1.cc(0, 67, 127, 1200)
    t1.cc(0, 67, 0, 2000)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + t0.render() + t1.render()


def build_truncated() -> bytes:
    """MTrk 声明长度越过文件末尾，轨尾只剩半个 note-on（v1.38.0）。

    旧解析器的通道消息分支直接 d[pos++] 不检查边界，越过文件末尾后会把
    堆里相邻内存当成 delta / 状态字节继续解析：这份 2 音素材曾读出 10 个
    幻影音符，调性也被带偏。修复后应只读出 2 个音并提示「文件数据不完整」。
    """
    t = Track()
    t.add(0, meta(0x03, b"Vocal"))
    t.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t.note(0, 60, 90, 0, DIV)
    t.note(0, 62, 90, DIV, DIV)
    blob = bytearray(b"MThd" + struct.pack(">IHHH", 6, 0, 1, DIV) + t.render())
    # MThd 固定 14 字节，第一轨的 MTrk 长度字段在 offset 18。
    declared = struct.unpack(">I", bytes(blob[18:22]))[0] + 200
    blob[18:22] = struct.pack(">I", declared)
    blob += bytes([0x00, 0x90, 0x3C])   # delta 0 + note-on + 音高，缺 velocity
    return bytes(blob)


# ---- --track / 网页段落选择：按轨隔离的回归素材 ----
#
# 旧实现只在 main.cpp 末尾按 track 丢音符，歌词 / CC11 / CC7 / 踏板 / 清音 /
# 弯音事件表仍来自全部轨，而且踏板等语义修正在解析阶段就改过时值了。
# 下面几份素材分别把「该隔离的」放到别的轨、把「必须保留的」放到别的轨。
_TRACK_MELODY = (60, 62, 64, 65, 67, 65, 64, 62)


def _track_melody(ch=0, name=b"Vocal", count=8, dur=120) -> Track:
    t = Track()
    t.add(0, meta(0x03, name))
    for i in range(count):
        t.note(ch, _TRACK_MELODY[i % 8], 90, i * DIV, dur)
    return t


def build_track_pedal() -> bytes:
    """CC64 踏板写在轨 1、且与旋律**同一通道**：--track 0 不该补长任何音。"""
    ped = Track()
    ped.add(0, meta(0x03, b"Pedal"))
    ped.cc(0, 64, 127, 0)
    ped.cc(0, 64, 0, 8 * DIV)
    return _port_smf([_track_melody(), ped])


def build_track_rpn_setup() -> bytes:
    """RPN 弯音范围写在轨 0 的 setup 上：--track 1 仍要读到 ±12 半音。"""
    setup = Track()
    setup.add(0, meta(0x03, b"Setup"))
    setup.bend_range(0, 12, 0)
    voc = Track()
    voc.add(0, meta(0x03, b"Vocal"))
    voc.bend(0, 16383, 0)
    for i in range(4):
        voc.note(0, 60 + 2 * i, 90, i * DIV, DIV - 40)
    return _port_smf([setup, voc])


def build_track_conductor() -> bytes:
    """速度 / 拍号 / 调号 / Marker 全在轨 0：--track 1 必须照常生效。"""
    c = Track()
    c.add(0, meta(0x03, b"Conductor"))
    c.add(0, meta(0x51, bytes([0x0A, 0x2C, 0x2A])))   # 666666us -> 90 BPM
    c.add(0, meta(0x58, bytes([3, 2, 24, 8])))        # 3/4
    c.add(0, meta(0x59, bytes([2, 0])))               # D 大调
    c.add(0, meta(0x06, b"Verse"))
    return _port_smf([c, _track_melody()])


def build_track_leak() -> bytes:
    """歌词 / CC11 / CC7 / 弯音 / 8 个音符全在轨 1：--track 0 一个都不许串。"""
    other = Track()
    other.add(0, meta(0x03, b"Guitar"))
    for i in range(8):
        other.note(1, 48 + i, 60, i * DIV, DIV - 40)
        other.cc(1, 11, 20, i * DIV)
        other.cc(1, 7, 20, i * DIV)
        other.add(i * DIV, meta(0x05, ("伴%d" % i).encode()))
    other.bend(1, 16383, 0)
    return _port_smf([_track_melody(), other])


def build_track_leak_base() -> bytes:
    """只含轨 0 的基线：--track 0 的谱面应与它逐行一致。"""
    return _port_smf([_track_melody()])


def build_track_empty() -> bytes:
    """轨 0 只有 meta 没有音符：--track 0 应明确报「轨道 0 无音符」。"""
    empty = Track()
    empty.add(0, meta(0x03, b"Empty"))
    return _port_smf([empty, _track_melody()])


def build_track_fmt2() -> bytes:
    """format 2 两段：选第 2 段时「共 2 段」的提示不能因为只剩一轨而消失。"""
    a = Track()
    a.add(0, meta(0x03, b"SongA"))
    for i in range(4):
        a.note(0, 60 + i, 90, i * DIV, DIV - 40)
    b = Track()
    b.add(0, meta(0x03, b"SongB"))
    for i in range(4):
        b.note(1, 67 - i, 90, i * DIV, DIV - 40)
        b.add(i * DIV, meta(0x05, ("乙%d" % i).encode()))
    return (b"MThd" + struct.pack(">IHHH", 6, 2, 2, DIV)
            + a.render() + b.render())


def build_chunk_resync() -> bytes:
    """SMF chunk 级边界重同步（v1.42.0）。

    两个真实损坏形态叠在一份文件里：
      1) MThd 之后夹一个 DAW 私有扩展块 XFIH（规范允许，旧实现要求每个
         chunk 都必须是 MTrk，遇到就停掉整个循环）；
      2) conductor 轨的 MTrk 长度字段少写 4 字节（旧实现下一轮循环在
         EOT 处读不到 "MTrk"，旋律轨整条丢失，CLI 退出码 3）。
    修复后应跳过 XFIH、按 EOT 锚点把 conductor 边界补回，仍读出 4 个音，
    并在页眉写明「跳过 1 个非 MTrk 扩展块、重同步 1 条轨道边界」。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    for i in range(4):
        t1.note(0, 60 + i, 96, i * DIV, 400)

    cond = bytearray(t0.render())
    declared = struct.unpack(">I", bytes(cond[4:8]))[0] - 4
    cond[4:8] = struct.pack(">I", declared)

    private = b"XFIH" + struct.pack(">I", 8) + b"DAWv1.0\x00"
    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return header + private + bytes(cond) + t1.render()


def build_oversized_len() -> bytes:
    # MTrk 声明长度越过文件末尾，但后续轨完整（v1.44.0）。
    # v1.42 的重同步只在 trkEnd <= size 时生效，遇到这种「长度字段写坏」
    # 会整份跳过，硬当成 v1.38 的真·截断文件：两轨并成一轨、--track 1
    # 选不到旋律、页眉误报数据不完整。修复后应按 EOT 锚点切开，读出 4 个音。
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    for i in range(4):
        t1.note(0, 60 + i, 96, i * DIV, 400)

    cond = bytearray(t0.render())
    cond[4:8] = struct.pack(">I", 0x00010000)
    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + bytes(cond) + t1.render())


def build_voice_trio() -> bytes:
    # 同轨三声部的 MusicXML direction 归属（v1.45.0）。
    # 旧实现整份 part 只取一个主导通道，第二 / 第三声部的 crescendo 与
    # p→ff 力度档位整条丢失；修复后每个 <voice> 各写一份，wedge number
    # 按声部递增，direction 用 <voice> 标明归属。
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))
    t0.add(0, meta(0x06, b"Verse"))

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    for i in range(8):
        t1.note(0, 76 + (i % 4), 96, i * DIV, DIV)
    t1.note(1, 60, 80, 0, BAR)
    t1.note(1, 62, 80, BAR, BAR)
    t1.note(2, 48, 64, 0, BAR * 2)

    up = (20, 40, 60, 80, 100, 120, 127)
    down = (120, 100, 80, 60, 40, 20, 10)
    mid_up = (30, 45, 60, 75, 90, 105, 120)
    for i in range(7):
        t1.cc(0, 11, up[i], i * 240)
        t1.cc(1, 11, down[i], i * 240)
        t1.cc(2, 11, mid_up[i], i * 240)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_grand_staff() -> bytes:
    # 键盘大谱表（v1.47.0）。
    # 单条钢琴轨（program 0）音域从 C2(36) 到 C6(84)，跨度 48 半音且跨中音 C，
    # 旧实现整份 part 只挑一个谱号，右手旋律被塞进低音谱号（或左手被塞进高音
    # 谱号）满屏加线。修复后自动排成上下两行谱：中音 C 及以上进上行谱（G 谱
    # 号），以下进下行谱（F 谱号），小节内用 <backup> 在两行谱之间切游标。
    # 第 1 小节两手同时进入，专门压「上行谱写满后补休止再退回小节开头写下行
    # 谱」这条路径；第 2 小节留空，覆盖大谱表的整小节双休止。
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)                                # Acoustic Grand Piano

    # 右手：高音谱号区域
    for i, p in enumerate((72, 74, 76, 77)):           # C5 D5 E5 F5
        t1.note(0, p, 88, i * DIV, DIV)
    for i, p in enumerate((79, 81, 83, 84)):           # G5 A5 B5 C6
        t1.note(0, p, 96, 2 * BAR + i * DIV, DIV)

    # 左手：低音谱号区域，与右手同时发声（迫使 splitVoices 分出第二声部）
    t1.note(0, 36, 64, 0, DIV * 2)                     # C2
    t1.note(0, 43, 64, DIV * 2, DIV * 2)               # G2
    t1.note(0, 48, 72, 2 * BAR, DIV * 2)               # C3
    t1.note(0, 55, 72, 2 * BAR + DIV * 2, DIV * 2)     # G3

    # 力度档位：p -> ff，验证大谱表下 direction 也带 <staff> 归属
    t1.cc(0, 11, 30, 0)
    t1.cc(0, 11, 100, 2 * BAR)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_xml_structure() -> bytes:
    """结构性守卫压力样本（v1.69.0）。

    把最容易让「逐小节逐声部时值求和」失真的几种机制叠进同一份文件：
    三连音（160 tick x 3 占一拍）、同拍和弦（<chord/> 成员不占时长）、
    同拍不同时值（splitVoices 拆 <voice> + <backup>）、跨小节长音（<tie>
    切段）、休止间隙、4/4 -> 6/8 变拍号。守卫按 <backup> 切时间线、跳过
    <chord/> 成员、跨小节继承拍号，每条时间线都应恰好等于该小节长度。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x06, b"Bridge"))                   # 全局段落名
    t0.add(3 * BAR, meta(0x58, bytes([6, 3, 24, 8])))  # 第 4 小节起 6/8

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)

    # 小节 1：三连音一拍 + 同值和弦一拍 + 长音/短音同起点（拆两声部）
    for i, p in enumerate((60, 62, 64)):
        t1.note(0, p, 88, i * 160, 160)
    for p in (67, 71, 74):
        t1.note(0, p, 84, 480, 480)
    t1.note(0, 48, 70, 960, 960)
    t1.note(0, 72, 92, 960, 480)

    # 小节 2：整小节长音与四个四分同时进入（再拆一次声部）
    t1.note(0, 36, 66, BAR, BAR)
    for i, p in enumerate((72, 74, 76, 77)):
        t1.note(0, p, 90, BAR + i * DIV, DIV)

    # 小节 3：先留一拍休止，再三个四分，末尾一个跨小节长音
    for i, p in enumerate((69, 71, 72)):
        t1.note(0, p, 90, 2 * BAR + DIV + i * DIV, DIV)
    t1.note(0, 55, 72, 2 * BAR + 3 * DIV, 2 * DIV)   # 5280 -> 6240，跨到 6/8

    # 小节 4（6/8 = 1440 tick）：长音延续段 480 + 四个八分 240 x 4
    for i, p in enumerate((79, 81, 83, 84)):
        t1.note(0, p, 92, 3 * BAR + DIV + i * (DIV // 2), DIV // 2)

    # 力度换档与段落名，让 <direction> 也进审计范围
    t1.cc(0, 11, 40, 0)
    t1.cc(0, 11, 110, BAR)
    t1.cc(0, 11, 60, 2 * BAR)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_staff_switch() -> bytes:
    """大谱表同一声部跨小节换谱表（v1.68.0）。

    单条钢琴轨（program 0）音域 C2(36) ~ C6(84)，跨度 48 半音且跨中音 C，
    触发 <staves>2</staves>；四个小节都是单声部：
      第 1 小节全在上行谱(1)，第 2 小节全在下行谱(2)，
      第 3 小节回到上行谱，第 4 小节前半上行 / 后半下行。
    高音 10 个、低音 6 个，全曲多数侧是上行谱。旧实现给 <direction> 的
    <staff> 用这个全曲多数侧，于是第 2 小节的 ff 与渐弱楔形被印到空白的
    上行谱，真正发声的下行谱看不到力度。修复后每条记号按自己的拍位找
    最近音头：第 4 小节第 3 拍换档那条带 <offset> 的 direction 也该落在
    后半小节的低音谱表。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)                                # Acoustic Grand Piano

    bars = (
        (72, 74, 76, 77),   # 第 1 小节：全在上行谱
        (36, 40, 43, 48),   # 第 2 小节：全在下行谱
        (79, 81, 83, 84),   # 第 3 小节：又回到上行谱
        (72, 74, 43, 48),   # 第 4 小节：前半上行、后半下行
    )
    for b, pitches in enumerate(bars):
        for i, p in enumerate(pitches):
            t1.note(0, p, 90, b * BAR + i * DIV, DIV)

    # 每小节开头换档；第 2 小节叠一段 CC11 渐弱斜坡（楔形）；第 4 小节
    # 第 3 拍再换一次档，逼出带 <offset> 的 direction。
    t1.cc(0, 11, 30, 0)                     # p
    t1.cc(0, 11, 100, BAR)                  # ff
    for i, v in enumerate((100, 80, 60, 40, 20)):
        t1.cc(0, 11, v, BAR + i * (BAR // 8))
    t1.cc(0, 11, 70, 2 * BAR)               # mf
    t1.cc(0, 11, 110, 3 * BAR + 2 * DIV)    # 第 4 小节中途 -> <offset>

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_grand_staff_wedge() -> bytes:
    """大谱表跨谱表渐强楔形（v1.70.0）。

    单条钢琴轨（program 0）音域 F2(34) ~ F5(77)，跨度 43 半音且跨中音 C，
    触发 <staves>2</staves>。一条 CC11 斜坡把整段写成一个 crescendo：
      第 1 小节全在上行谱(1)：72 74 76 77，CC11 30->55->100 起点在 tick 0；
      第 2 小节全在下行谱(2)：34 38 43 48，楔形终点落在 tick 960。
    旧实现每条 <direction> 的 <staff> 按各自拍位取：crescendo 起在 <staff>1</staff>、
    stop 落在 <staff>2</staff>，一条 hairpin 被拆到两个谱表，MuseScore 渲染成
    断裂的楔形。修复后同一条 wedge 以起点谱表为锚，stop 复用同一 <staff>。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(0, meta(0x59, bytes([0, 0])))               # C major

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)                                # Acoustic Grand Piano

    # 第 1 小节：上行谱
    for i, p in enumerate((72, 74, 76, 77)):
        t1.note(0, p, 80, i * DIV, DIV)
    # 第 2 小节：下行谱
    for i, p in enumerate((34, 38, 43, 48)):
        t1.note(0, p, 80, BAR + i * DIV, DIV)

    # CC11 斜坡 30->55->100：>=3 点、总变化 >=24 -> 一个 crescendo（起点 0、终点 960）
    t1.cc(0, 11, 30, 0)
    t1.cc(0, 11, 55, DIV // 2)
    t1.cc(0, 11, 100, BAR)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_midtempo() -> bytes:
    # 小节中途变速（v1.48.0）。
    # 旧实现把速度记号按小节归并成 map<bar, mark>：同一小节的多个阶跃只留
    # 最后一个，且一律写在小节开头——第 1 小节 120→90(第2拍)→60(第4拍) 会被
    # 吞成「整小节 60」，MuseScore 从第一拍就按 60 播放。修复后每个阶跃各写
    # 一条 <direction>，落点用 <offset> 标回小节内位置；正好落在小节线上的
    # 第 3 小节 120 不写 <offset>，保持旧排版。
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(DIV, meta(0x51, bytes([0x0A, 0x2C, 0x2B])))  # 666667us -> 90BPM（正第 2 拍）
    t0.add(1440, meta(0x51, bytes([0x0F, 0x42, 0x40])))  # 1000000us -> 60BPM（第 4 拍）
    t0.add(2 * BAR, meta(0x51, bytes([0x07, 0xA1, 0x20])))  # 回到 120BPM（第 3 小节线）

    t1 = Track()
    t1.add(0, meta(0x03, b"Melody"))
    t1.program(0, 73, 0)                               # Flute
    for i in range(12):
        t1.note(0, 72 + (i % 4), 96, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_dyn_step() -> bytes:
    # 小节中途换力度档（v1.49.0）。
    # 旧实现只在每小节开头取 dynamicAt(barStart)：第 1 小节第 2 拍 ff→p、
    # 第 4 拍 p→f 都被推到下一小节才印出来，整条力度标记晚了三拍，MuseScore
    # 播放与谱面对不上。修复后中途换档按真实 tick 写 <offset>。
    # 四个档位刻意两两反向（100→20→80→60），不会被渐变检测误判成楔形，
    # 因此每处都必须是独立的力度记号。
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 500000us -> 120BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4

    t1 = Track()
    t1.add(0, meta(0x03, b"Piano"))
    t1.program(0, 0, 0)                                # Acoustic Grand
    t1.cc(0, 11, 100, 0)                               # ff（第 1 小节头）
    t1.cc(0, 11, 20, DIV)                              # p（第 1 小节第 2 拍）
    t1.cc(0, 11, 80, 3 * DIV)                          # f（第 1 小节第 4 拍）
    t1.cc(0, 11, 50, 2 * BAR)                          # mp（第 3 小节线）
    for i in range(12):
        t1.note(0, 60 + (i % 4), 96, i * DIV, DIV)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_beam_levels() -> bytes:
    """v1.66.0：连梁压力样本，让 CI 在真实素材上覆盖二级梁与复拍子。

    DIV=480，前四小节 4/4（每小节 1920 tick），后两小节变拍号到 6/8
    （每小节 1440 tick）：

        小节 1  16 个十六分（120 tick）         -> 一级 + 二级梁各 4 组
        小节 2  [十六 十六 八分] x2 + [八分 十六 十六] x2
                                              -> 二级梁只盖相邻两个十六分
        小节 3  [十六 八分 十六] x2 + 八分 八分 四分
                                              -> 二级梁参与者不连续，不画
        小节 4  附点十六分 x4 + 附点八分 x2 + 四分
                                              -> 0.375 / 0.75 拍附点档
        小节 5-6  6/8 各 6 个八分             -> 两组三音连梁（begin/continue/end）

    一级梁的 continue 状态、二级梁（number="2"）、以及 6/8 的附点拍分组，
    在新增这份素材之前都只有单测覆盖；check_xml_beams.py 的覆盖硬约束会
    盯着这几个维度，缺一个就报错。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(7680, meta(0x58, bytes([6, 3, 24, 8])))     # 小节 5 起 6/8
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)

    def fill(offset, pattern, length):
        cur = offset
        for i, dur in enumerate(pattern):
            t1.note(0, 60 + (i % 7), 96, cur, dur)
            cur += dur
        assert cur - offset == length, (offset, cur - offset)

    fill(0,     [120] * 16, BAR)
    fill(BAR,   [120, 120, 240] * 2 + [240, 120, 120] * 2, BAR)
    fill(2*BAR, [120, 240, 120] * 2 + [240, 240, 480], BAR)
    fill(3*BAR, [180, 180, 180, 180, 360, 360, 480], BAR)
    for b in range(2):
        fill(4*BAR + b * 1440, [240] * 6, 1440)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def build_beam_hooks() -> bytes:
    """v1.67.0：钩形梁压力样本，让 CI 在真实素材上审到 forward/backward hook。

    DIV=480，前两小节 4/4（每小节 1920 tick），后两小节变拍号到 6/8
    （每小节 1440 tick，梁组边长 720 tick = 附点四分）：

        小节 1 拍 1  [十六 八分 十六]       -> 二级梁两颗孤立项：
                                              组首 forward hook、组尾 backward hook
              拍 2  [十六 十六 八分]       -> 二级梁正常 begin/end（对照组）
              拍 3  [八分 八分]            -> 一级梁成对
              拍 4  [四分]                 -> 长音截断梁组
        小节 2 拍 1  [32 32 十六 八分]     -> 三级梁连续段（无需 hook）
              拍 2  [32 十六 32 八分]     -> 三级梁两颗孤立项（level 3 hook）
              拍 3/4 同上小节
        小节 3-4  6/8 各 [十六 八分 十六 八分] x2
                                          -> 每颗十六分都是孤立项，
                                             组首 forward、组内 backward hook

    在它之前，混合时值下的孤立短音符只由单测和临时探针覆盖；少了 hook，
    MusicXML 里十六分音符会被显示成八分，而 HTML 端仍按自身时值画两条减时线。
    """
    t0 = Track()
    t0.add(0, meta(0x03, b"Conductor"))
    t0.add(0, meta(0x51, bytes([0x07, 0xA1, 0x20])))   # 120 BPM
    t0.add(0, meta(0x58, bytes([4, 2, 24, 8])))        # 4/4
    t0.add(2 * BAR, meta(0x58, bytes([6, 3, 24, 8])))  # 小节 3 起 6/8
    t0.add(0, meta(0x59, bytes([0, 0])))

    t1 = Track()
    t1.add(0, meta(0x03, b"Vocal"))
    t1.program(0, 0, 0)

    def fill(offset, pattern, length):
        cur = offset
        for i, dur in enumerate(pattern):
            t1.note(0, 60 + (i % 7), 96, cur, dur)
            cur += dur
        assert cur - offset == length, (offset, cur - offset)

    # 16th=120, eighth=240, quarter=480, 32nd=60
    fill(0, [120, 240, 120, 120, 120, 240, 240, 240, 480], BAR)
    fill(BAR, [60, 60, 120, 240, 60, 120, 60, 240, 240, 240, 480], BAR)
    for b in range(2):
        fill(2 * BAR + b * 1440, [120, 240, 120, 240] * 2, 1440)

    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV)
    return bytes(header + t0.render() + t1.render())


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    mid = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "sample.mid")
    with open(mid, "wb") as f:
        data = build()
        f.write(data)
    lrc = os.path.splitext(mid)[0] + ".lrc"
    with open(lrc, "w", encoding="utf-8") as f:
        f.write(LRC)
    print(f"wrote {mid} ({len(data)} bytes)")
    print(f"wrote {lrc}")

    # 量化用的素材与 sample.mid 同目录，文件名固定
    hmid = os.path.join(here, "humanized.mid")
    with open(hmid, "wb") as f:
        hdata = build_humanized()
        f.write(hdata)
    print(f"wrote {hmid} ({len(hdata)} bytes)")

    # 带打击乐轨的素材
    dmid = os.path.join(here, "drums.mid")
    with open(dmid, "wb") as f:
        ddata = build_drums()
        f.write(ddata)
    print(f"wrote {dmid} ({len(ddata)} bytes)")

    # 一字多音（歌词跨小节分配）的素材
    mmid = os.path.join(here, "melisma.mid")
    with open(mmid, "wb") as f:
        mdata = build_melisma()
        f.write(mdata)
    mlrc = os.path.join(here, "melisma.lrc")
    with open(mlrc, "w", encoding="utf-8") as f:
        f.write(MELISMA_LRC)
    print(f"wrote {mmid} ({len(mdata)} bytes)")
    print(f"wrote {mlrc}")

    # 小节内换和弦 + 中途变速的素材
    hmid2 = os.path.join(here, "harmony.mid")
    with open(hmid2, "wb") as f:
        hdata2 = build_harmony()
        f.write(hdata2)
    print(f"wrote {hmid2} ({len(hdata2)} bytes)")

    # 弱起（第一小节不满）的素材
    pmid = os.path.join(here, "pickup.mid")
    with open(pmid, "wb") as f:
        pdata = build_pickup()
        f.write(pdata)
    print(f"wrote {pmid} ({len(pdata)} bytes)")

    # 带 MIDI 内置歌词（0x05）的卡拉 OK 素材
    kmid = os.path.join(here, "karaoke.mid")
    with open(kmid, "wb") as f:
        kdata = build_karaoke()
        f.write(kdata)
    print(f"wrote {kmid} ({len(kdata)} bytes)")

    # 中途变拍号 + 变速 + 鼓轨 + 内置歌词的混合压力素材
    metmid = os.path.join(here, "meter.mid")
    with open(metmid, "wb") as f:
        metdata = build_meter()
        f.write(metdata)
    print(f"wrote {metmid} ({len(metdata)} bytes)")

    # 中途转调 1=C → 1=D → 1=F 的混合压力素材
    modmid = os.path.join(here, "modulation.mid")
    with open(modmid, "wb") as f:
        moddata = build_modulation()
        f.write(moddata)
    print(f"wrote {modmid} ({len(moddata)} bytes)")

    # 中国五声调式（C 宫 / G 徵），故意不写 0x59 的自动猜调素材
    pentmid = os.path.join(here, "pentatonic.mid")
    with open(pentmid, "wb") as f:
        pentdata = build_pentatonic()
        f.write(pentdata)
    print(f"wrote {pentmid} ({len(pentdata)} bytes)")

    # LRC 时间轴 × tempo map：一份 .mid 配三份 .lrc
    tlmid = os.path.join(here, "tempolrc.mid")
    with open(tlmid, "wb") as f:
        tldata = build_tempolrc()
        f.write(tldata)
    print(f"wrote {tlmid} ({len(tldata)} bytes)")
    for name, content in (
        ("tempolrc.lrc", build_tempolrc_lrc()),
        ("tempolrc_offset.lrc", build_tempolrc_offset_lrc()),
        ("tempolrc_multi.lrc", build_tempolrc_multi_lrc()),
    ):
        path = os.path.join(here, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"wrote {path}")

    # 力度 / 渐强渐弱 / 断奏 / 重音 / 滑音 / 颤音 / 段落标记混合压力素材
    exmid = os.path.join(here, "expression.mid")
    with open(exmid, "wb") as f:
        exdata = build_expression()
        f.write(exdata)
    print(f"wrote {exmid} ({len(exdata)} bytes)")

    # 声部 / 乐器标注 + 倚音（装饰音）的混合素材
    pmid2 = os.path.join(here, "parts.mid")
    with open(pmid2, "wb") as f:
        pdata2 = build_parts()
        f.write(pdata2)
    print(f"wrote {pmid2} ({len(pdata2)} bytes)")

    # 和弦转位 / 七和弦扩充素材
    cmid = os.path.join(here, "chords.mid")
    with open(cmid, "wb") as f:
        cdata = build_chords()
        f.write(cdata)
    print(f"wrote {cmid} ({len(cdata)} bytes)")

    # 渐快 / 渐慢（连续 set_tempo 折叠）素材
    rmid = os.path.join(here, "tempo_ramp.mid")
    with open(rmid, "wb") as f:
        rdata = build_tempo_ramp()
        f.write(rdata)
    print(f"wrote {rmid} ({len(rdata)} bytes)")

    # 长音跨小节（延音线）素材
    tlmid = os.path.join(here, "long_tie.mid")
    with open(tlmid, "wb") as f:
        tldata = build_long_tie()
        f.write(tldata)
    print(f"wrote {tlmid} ({len(tldata)} bytes)")

    # 同轨复调（双声部）素材
    tpmid = os.path.join(here, "polyphony.mid")
    with open(tpmid, "wb") as f:
        tpdata = build_polyphony()
        f.write(tpdata)
    print(f"wrote {tpmid} ({len(tpdata)} bytes)")

    # SMPTE 时间基准素材
    smid = os.path.join(here, "smpte.mid")
    with open(smid, "wb") as f:
        sdata = build_smpte()
        f.write(sdata)
    print(f"wrote {smid} ({len(sdata)} bytes)")

    # format 2（多个独立序列）素材
    f2mid = os.path.join(here, "format2.mid")
    with open(f2mid, "wb") as f:
        f2data = build_format2()
        f.write(f2data)
    print(f"wrote {f2mid} ({len(f2data)} bytes)")

    # 复音触后（0xA0）素材
    atmid = os.path.join(here, "aftertouch.mid")
    with open(atmid, "wb") as f:
        atdata = build_aftertouch()
        f.write(atdata)
    print(f"wrote {atmid} ({len(atdata)} bytes)")

    # 同音重触发素材
    rtmid = os.path.join(here, "retrigger.mid")
    with open(rtmid, "wb") as f:
        rtdata = build_retrigger()
        f.write(rtdata)
    print(f"wrote {rtmid} ({len(rtdata)} bytes)")

    # 轨尾悬挂音素材
    dgmid = os.path.join(here, "dangling.mid")
    with open(dgmid, "wb") as f:
        dgdata = build_dangling()
        f.write(dgdata)
    print(f"wrote {dgmid} ({len(dgdata)} bytes)")

    # 双歌词轨素材
    tlmid = os.path.join(here, "twolyrics.mid")
    with open(tlmid, "wb") as f:
        tldata = build_twolyrics()
        f.write(tldata)
    print(f"wrote {tlmid} ({len(tldata)} bytes)")

    # RPN 弯音范围素材（按通道隔离 / 中途改范围 / 跨轨设定）
    for name, builder in (
        ("bendrange_channels", build_bendrange_channels),
        ("bendrange_timeline", build_bendrange_timeline),
        ("bendrange_crosstrack", build_bendrange_crosstrack),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 控制器语义素材（CC64 / CC123 / CC7）
    for name, builder in (
        ("pedal", build_pedal),
        ("allnotesoff", build_allnotesoff),
        ("cc7volume", build_cc7volume),
        ("pedal_channels", build_pedal_channels),
        ("pedal_hysteresis", build_pedal_hysteresis),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # running status 生命周期素材（meta / SysEx / 实时消息 / 规范基线）
    for name, builder in (
        ("running_meta", build_running_meta),
        ("running_sysex", build_running_sysex),
        ("running_realtime", build_running_realtime),
        ("running_standard", build_running_standard),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # MIDI Port / Channel Prefix 素材（端口隔离 / 弯音范围 / 前缀 / 两份基线）
    for name, builder in (
        ("port_pedal", build_port_pedal),
        ("port_bend", build_port_bend),
        ("port_prefix", build_port_prefix),
        ("port_prefix_base", build_port_prefix_base),
        ("port_standard", build_port_standard),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # SysEx 鼓模式素材（GS/XG 指定鼓通道 / 三种 Reset / 端口隔离 / 零回归）
    for name, builder in (
        ("sysex_gs_drum", build_sysex_gs_drum),
        ("sysex_xg_drum", build_sysex_xg_drum),
        ("sysex_gs_normal", build_sysex_gs_normal),
        ("sysex_gs_reset", build_sysex_gs_reset),
        ("sysex_gm_reset", build_sysex_gm_reset),
        ("sysex_xg_reset", build_sysex_xg_reset),
        ("sysex_conductor", build_sysex_conductor),
        ("sysex_ch9_normal", build_sysex_ch9_normal),
        ("sysex_ch9_drum", build_sysex_ch9_drum),
        ("sysex_unknown", build_sysex_unknown),
        ("sysex_port_drum", build_sysex_port_drum),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # Bank Select / Program Change 素材（XG 鼓组 / 时间轴 / 端口 / 零回归对照）
    for name, builder in (
        ("bank_xg_drum", build_bank_xg_drum),
        ("bank_normal", build_bank_normal),
        ("bank_switch_back", build_bank_switch_back),
        ("bank_conductor", build_bank_conductor),
        ("bank_port", build_bank_port),
        ("bank_reset", build_bank_reset),
        ("bank_ch9_plain", build_bank_ch9_plain),
        ("bank_no_program", build_bank_no_program),
        ("program_timeline", build_program_timeline),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # --list-tracks 诊断一致性素材（中途换音色 + XG 鼓组同曲）
    for name, builder in (
        ("diag_consistency", build_diag_consistency),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 截断 / 损坏文件：解析必须停在 buffer 末尾，不产生幻影音符
    for name, builder in (
        ("truncated", build_truncated),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # CC66 持音踏板：踩下瞬间正按着的键被保持，之后新弹的音不受影响
    for name, builder in (
        ("sostenuto", build_sostenuto),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # CC11 渐强渐弱按通道归属：伴奏的 cresc. 不能印到旋律谱上
    for name, builder in (
        ("dyn_channels", build_dyn_channels),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # CC67 软踏板：只改 velocity，不改时值，也不动 CC11 / CC7 力度表
    for name, builder in (
        ("soft_pedal", build_soft_pedal),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # SMF chunk 边界：私有扩展块 + 长度字段失准，不再整份报废
    for name, builder in (
        ("chunk_resync", build_chunk_resync),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 通道级 wedge：两条独立轨各写自己的渐强渐弱，全局段落名只写一次
    for name, builder in (
        ("wedge_tracks", build_wedge_tracks),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 长度字段越过文件末尾、后续轨仍完整：按锚点重同步，不再误报截断
    for name, builder in (
        ("oversized_len", build_oversized_len),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 同轨三声部：MusicXML 按 <voice> 各写自己的力度与渐变
    for name, builder in (
        ("voice_trio", build_voice_trio),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # --track / 网页段落选择素材（演奏事件隔离 + 全局设定保留）
    for name, builder in (
        ("track_pedal", build_track_pedal),
        ("track_rpn_setup", build_track_rpn_setup),
        ("track_conductor", build_track_conductor),
        ("track_leak", build_track_leak),
        ("track_leak_base", build_track_leak_base),
        ("track_empty", build_track_empty),
        ("track_fmt2", build_track_fmt2),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")
    # 键盘大谱表：宽音域钢琴轨排成上下两行谱（高音 + 低音谱号）
    for name, builder in (
        ("grand_staff", build_grand_staff),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 结构守卫压力样本：三连音 / 和弦 / backup / 变拍号 / 跨小节长音
    for name, builder in (
        ("xml_structure", build_xml_structure),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 大谱表换谱表：方向记号的 <staff> 按小节 / 拍位跟随实际音符
    for name, builder in (
        ("staff_switch", build_staff_switch),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 大谱表跨谱表渐强楔形：同一条 wedge 起止必须落同一谱表
    for name, builder in (
        ("grand_staff_wedge", build_grand_staff_wedge),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 小节中途变速：多个阶跃各带 <offset>，不再被吸附到小节开头
    for name, builder in (
        ("midtempo", build_midtempo),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 小节中途换力度档：每处换档各带 <offset>，不再被推迟到下一小节
    for name, builder in (
        ("dyn_step", build_dyn_step),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 移调素材：降号调 (1=Bb) 起步、中途转到升号调 (1=D)，锁住 --transpose
    # 把 MIDI 调号一起平移（只移音不移调号会让唱名与调号分叉）
    for name, builder in (
        ("transpose_key", build_transpose_key),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 门限因子按声部估：左手连奏链不再把右手的门限锁死在 1.0
    for name, builder in (
        ("poly_gate", build_poly_gate),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 复调轨按声部补空档：左手全音符不再挡住右手补整拍
    for name, builder in (
        ("poly_gap", build_poly_gap),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 门限衰减的空档并进前一个音：4/4 一小节不再只剩 3 拍
    for name, builder in (
        ("gate_gap", build_gate_gap),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 时值折算不许把小节撑过拍号（3.75 拍记成 4 拍），倚音也不许吞掉
    # 它前面的休止
    for name, builder in (
        ("overfull", build_overfull),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")

    # 不规则休止间隙：MusicXML 拆成标准时值之和、文本谱拍数守恒
    for name, builder in (
        ("rest_split", build_rest_split),
        ("rest_midbar", build_rest_midbar),
        ("html_dots", build_html_dots),
        ("html_rests", build_html_rests),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")


    # 连梁压力样本：十六分音符的二级梁 / continue / 6/8 三音连梁
    for name, builder in (
        ("beam_levels", build_beam_levels),
        ("beam_hooks", build_beam_hooks),
    ):
        path = os.path.join(here, name + ".mid")
        with open(path, "wb") as f:
            blob = builder()
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()

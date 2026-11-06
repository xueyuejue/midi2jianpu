#!/usr/bin/env python3
"""生成 midi2jianpu 的回归测试素材（标准 MIDI 文件，format 1）。

不依赖任何第三方库，只输出很小的 .mid 和 .lrc，便于在 CI / 新机器上
随时重建素材、复现问题。共生成 10 份素材：sample / humanized / drums /
melisma / harmony / pickup / karaoke / meter / modulation / pentatonic。
meter 是把变拍号、变速、鼓轨、内置歌词、伴奏和弦混在一起的「压力样本」，
modulation 是中途转调（1=C → 1=D → 1=F），pentatonic 则是中国五声调式
（C 宫、G 徵调式，故意不写 0x59 逼自动猜调暴露宫音判错）。

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


if __name__ == "__main__":
    main()

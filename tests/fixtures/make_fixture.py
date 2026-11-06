#!/usr/bin/env python3
"""生成 midi2jianpu 的回归测试素材（标准 MIDI 文件，format 1）。

不依赖任何第三方库，只输出一个很小的 .mid 和一个 .lrc，便于在 CI / 新机器上
随时重建素材、复现问题。

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


if __name__ == "__main__":
    main()

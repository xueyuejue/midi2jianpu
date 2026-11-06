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


if __name__ == "__main__":
    main()

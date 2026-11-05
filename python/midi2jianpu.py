#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
midi2jianpu - 第一版原型

把一个 .mid 文件转换成文本形式的中文简谱（数字谱）。

用法:
    pip install mido
    python midi2jianpu.py your_song.mid

设计目标（原型阶段）:
    1. 解析标准 MIDI 文件，提取音符序列（音高、起始时间、时值）
    2. 简单识别主音（调性）：统计各音高出现时长，取最长者作为主音
    3. 按首调唱名把音高换算成简谱数字 1-7
    4. 输出可读的文本简谱（八度用 . 表示，附点时值用 - 表示）

注意：这是验证核心逻辑的原型，排版、多轨、连音等在后续版本完善。
"""

import sys
import argparse
from collections import defaultdict

try:
    import mido
except ImportError:
    sys.stderr.write(
        "需要 mido 库。请先运行:  pip install mido  (以及 python-rtmidi 仅在需要实时IO时)\n"
    )
    raise


# ---- 音名工具 ---------------------------------------------------------------

# MIDI 音号到音名（12 平均律，C4=60）
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# 自然大调里各音级相对于主音的半音距离
MAJOR_SCALE = [0, 2, 4, 5, 7, 9, 11]
# 自然小调
MINOR_SCALE = [0, 3, 5, 7, 8, 10]


def midi_to_name(note: int) -> str:
    """60 -> 'C4'"""
    name = NOTE_NAMES[note % 12]
    octave = note // 12 - 1
    return f"{name}{octave}"


# ---- MIDI 解析 --------------------------------------------------------------

def extract_notes(mid_path: str):
    """
    从 MIDI 文件提取音符事件列表。
    返回: [ {start_seconds, duration_seconds, pitch, channel, track} ]
    只合并单轨/所有轨的 Note On/Off，原型阶段不做复杂多轨分离。
    """
    mid = mido.MidiFile(mid_path)

    # 先把每个 track 的事件按 ticks 展开成绝对时间
    notes = []  # 暂存 (start_tick, pitch, track, channel)
    active = {}  # (track, channel, pitch) -> start_tick
    tick_to_seconds_all = []

    absolute_ticks = 0
    tempo = 500000  # 默认 120 BPM (microseconds per beat)

    for track_idx, track in enumerate(mid.tracks):
        absolute_ticks = 0
        for msg in track:
            absolute_ticks += msg.time
            if msg.is_meta and msg.type == "set_tempo":
                tempo = msg.tempo
            if msg.type == "note_on" and msg.velocity > 0:
                key = (track_idx, msg.channel, msg.note)
                active[key] = absolute_ticks
            elif msg.type == "note_off" or (
                msg.type == "note_on" and msg.velocity == 0
            ):
                key = (track_idx, msg.channel, msg.note)
                if key in active:
                    start_tick = active.pop(key)
                    # 用默认 tempo 做近似换算（原型阶段）
                    start_sec = mido.tick2second(start_tick, mid.ticks_per_beat, tempo)
                    end_sec = mido.tick2second(absolute_ticks, mid.ticks_per_beat, tempo)
                    notes.append({
                        "start_seconds": start_sec,
                        "duration_seconds": max(end_sec - start_sec, 0.02),
                        "pitch": msg.note,
                        "channel": msg.channel,
                        "track": track_idx,
                    })

    notes.sort(key=lambda n: n["start_seconds"])
    return notes, mid.ticks_per_beat


# ---- 调性识别 ---------------------------------------------------------------

def estimate_tonic(notes):
    """
    简单估计主音：统计每个 pitch class (0-11) 的总发声时长，取最高的。
    返回 (pitch_class 0-11, is_major)
    """
    weight = defaultdict(float)
    for n in notes:
        weight[n["pitch"] % 12] += n["duration_seconds"]
    # 主音 pitch class
    pc = max(weight, key=weight.get)

    # 判断大小调：主音上方大三度(4)还是小三度(3)更重
    major_third = weight.get((pc + 4) % 12, 0)
    minor_third = weight.get((pc + 3) % 12, 0)
    is_major = major_third >= minor_third
    return pc, is_major


# ---- 简谱转换 ---------------------------------------------------------------

def pitch_to_jianpu(pitch: int, tonic_pc: int, is_major: str):
    """
    把 MIDI 音号转成简谱字符。
    返回: (数字字符串, 八度偏移, 升降号)
        八度偏移: 0 = 中音(以中央C附近为参考), 正数 = 高N个八度, 负数 = 低N个八度
    """
    scale = MAJOR_SCALE if is_major else MINOR_SCALE
    # 只取当前八度内的半音位置（0-11），避免把绝对音高当八度
    semi = (pitch - tonic_pc) % 12
    # 八度以 C4=60 为简谱中音(1)参考
    octave_offset = (pitch - 60) // 12

    # 在自然音阶里找最近的音级
    degree = None
    accidental = ""
    for i, s in enumerate(scale):
        if s == semi:
            degree = i
            break
    else:
        # 不在自然音阶 = 升降半音。找上一个自然音级
        for i in range(len(scale) - 1, -1, -1):
            if scale[i] < semi:
                degree = i
                accidental = "#" if semi == scale[i] + 1 else "b"
                break
        if degree is None:
            degree = 6
            accidental = "#" if semi == 11 else ""

    num = str(degree + 1)  # 简谱数字 1-7
    return num, octave_offset, accidental


def format_jianpu(notes, tonic_pc, is_major):
    """把音符列表格式化成文本简谱。"""
    scale_name = "大调" if is_major else "小调"
    lines = [
        f"主音(1=): {NOTE_NAMES[tonic_pc]} {scale_name}",
        "-" * 40,
    ]
    for n in notes:
        num, oct_off, acc = pitch_to_jianpu(n["pitch"], tonic_pc, is_major)
        oct_mark = ""
        if oct_off > 0:
            oct_mark = "'" * oct_off      # 高八度在数字后加 '
        elif oct_off < 0:
            oct_mark = "," * (-oct_off)    # 低八度在数字后加 ,
        # 时长近似：按秒数粗估成 四分音符为一拍的比例（原型）
        dur = n["duration_seconds"]
        beat = int(round(dur / 0.5))     # 假设一拍 0.5 秒(120BPM)
        beat_mark = "-" * max(beat - 1, 0)  # 附点/延音线用 -
        lines.append(f"{acc}{num}{oct_mark}{beat_mark}  ({midi_to_name(n['pitch'])}, {dur:.2f}s)")
    return "\n".join(lines)


# ---- 主流程 -----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="MIDI -> 中文简谱 原型")
    ap.add_argument("mid", help="输入 .mid 文件路径")
    ap.add_argument("--track", type=int, default=None, help="只看某个 track")
    args = ap.parse_args()

    notes, tpq = extract_notes(args.mid)
    if args.track is not None:
        notes = [n for n in notes if n["track"] == args.track]

    if not notes:
        print("未提取到音符，请检查 MIDI 文件。")
        return

    tonic_pc, is_major = estimate_tonic(notes)
    print(f"\n共提取 {len(notes)} 个音符（ticks/beat={tpq}）\n")
    print(format_jianpu(notes, tonic_pc, is_major))


if __name__ == "__main__":
    main()

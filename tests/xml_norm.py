#!/usr/bin/env python3
"""跨版本零回归用的 MusicXML 归一化助手（v1.68.0）。

各 check_* 脚本对「除本轮修的维度外逐字节一致」各有自己的剥离函数：
v1.64.0 剥 <beam>、v1.61.0 剥纯休止列、v1.65.0 摘 bgrp 外壳。
v1.68.0 改的是 <direction> 里 <staff> 的取值，比对前也要把它摘掉，
但**只能摘 direction 块内的**：<note> 里的 <staff> 是音符归属，
属于大谱表游标语义，任何版本都不许变。
"""
import re

PRINT_B = re.compile(rb"@media print\{.*?\n\}", re.S)
PRINT_S = re.compile(r"@media print\{.*?\n\}", re.S)

DIR_B = re.compile(rb"<direction\b.*?</direction>", re.S)
DIR_S = re.compile(r"<direction\b.*?</direction>", re.S)
STAFF_LINE_B = re.compile(rb"[ \t]*<staff>\d+</staff>\r?\n")
STAFF_LINE_S = re.compile(r"[ \t]*<staff>\d+</staff>\r?\n")


def strip_direction_staff(xml):
    """删掉 <direction> 块里的 <staff> 整行，同时支持 str / bytes。"""
    if isinstance(xml, bytes):
        return DIR_B.sub(lambda m: STAFF_LINE_B.sub(b"", m.group(0)), xml)
    return DIR_S.sub(lambda m: STAFF_LINE_S.sub("", m.group(0)), xml)


def strip_print_block(html):
    """摘掉 HTML 的 @media print 块（v1.78.0）。

    打印媒体样式只决定「纸面上长什么样」，不参与谱面时值 / 结构语义，所以
    跨版本逐字节比对时它是噪音：v1.78.0 把 .barno 从 #ddd 压到 #777、给
    .footer 补了 #888，那是有意的可读性修复，不是结构漂移。打印配色由
    smoke.py 第 [91] 组单独守（量的是亮度，不是色号）。
    """
    if isinstance(html, bytes):
        return PRINT_B.sub(b"@media print{}", html)
    return PRINT_S.sub("@media print{}", html)

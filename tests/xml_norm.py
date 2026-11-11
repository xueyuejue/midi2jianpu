#!/usr/bin/env python3
"""跨版本零回归用的 MusicXML 归一化助手（v1.68.0）。

各 check_* 脚本对「除本轮修的维度外逐字节一致」各有自己的剥离函数：
v1.64.0 剥 <beam>、v1.61.0 剥纯休止列、v1.65.0 摘 bgrp 外壳。
v1.68.0 改的是 <direction> 里 <staff> 的取值，比对前也要把它摘掉，
但**只能摘 direction 块内的**：<note> 里的 <staff> 是音符归属，
属于大谱表游标语义，任何版本都不许变。
"""
import re

DIR_B = re.compile(rb"<direction\b.*?</direction>", re.S)
DIR_S = re.compile(r"<direction\b.*?</direction>", re.S)
STAFF_LINE_B = re.compile(rb"[ \t]*<staff>\d+</staff>\r?\n")
STAFF_LINE_S = re.compile(r"[ \t]*<staff>\d+</staff>\r?\n")


def strip_direction_staff(xml):
    """删掉 <direction> 块里的 <staff> 整行，同时支持 str / bytes。"""
    if isinstance(xml, bytes):
        return DIR_B.sub(lambda m: STAFF_LINE_B.sub(b"", m.group(0)), xml)
    return DIR_S.sub(lambda m: STAFF_LINE_S.sub("", m.group(0)), xml)

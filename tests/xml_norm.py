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

# 页眉「已丢弃 N 个越界事件 / 已截断 N 个越界音符」里的 N 是诊断计数，
# 不是谱面结构。v1.87 把解析期消费的事件也计入之后，stray_events.mid 的
# 页眉由 10 修正为 12（v1.86 漏数 2 个）；结构零回归不该被这个数字打断。
# 数字本身由 .probe/regress_v187.py 精确断言，其余字节仍逐字节比对。
NOTICE_B = re.compile("(已丢弃|已截断)\\s*\\d+\\s*(个越界(?:事件|音符))".encode("utf-8"))
NOTICE_S = re.compile(r"(已丢弃|已截断)\s*\d+\s*(个越界(?:事件|音符))")

DIR_B = re.compile(rb"<direction\b.*?</direction>", re.S)
DIR_S = re.compile(r"<direction\b.*?</direction>", re.S)
STAFF_LINE_B = re.compile(rb"[ \t]*<staff>\d+</staff>\r?\n")
STAFF_LINE_S = re.compile(r"[ \t]*<staff>\d+</staff>\r?\n")


def strip_direction_staff(xml):
    """删掉 <direction> 块里的 <staff> 整行，同时支持 str / bytes。"""
    if isinstance(xml, bytes):
        return DIR_B.sub(lambda m: STAFF_LINE_B.sub(b"", m.group(0)), xml)
    return DIR_S.sub(lambda m: STAFF_LINE_S.sub("", m.group(0)), xml)


def normalize_timeline_notice(html):
    """把页眉时间轴诊断的数字归一成 N，支持 str / bytes（v1.87.0）。

    只动「已丢弃 / 已截断」后面那个计数。诊断数字随解析口径变准是好事，
    但结构零回归比的是谱面，不该被页眉一行 unrelated 的数字打断。
    """
    if isinstance(html, bytes):
        return NOTICE_B.sub(lambda m: m.group(1) + b" N " + m.group(2), html)
    return NOTICE_S.sub(lambda m: m.group(1) + " N " + m.group(2), html)


# v1.151.0：.bgrp::after 的 top 从写死 12px 改为 var(--oct-h, 12px)，
# 并改写了紧邻的 CSS 注释。这是全局样式声明，对**每份素材**的 HTML 都生效，
# 属本轮有意修改；跨版本结构比对前把它归一回旧写法，谱面结构仍逐字节比。
# 真实行为（bgrp 带 --oct-h、CSS 用 var）由单测 / 冒烟 [145] / 扫掠独立守护。
BEAM_OLD_S = ("\r\n   .bgrp 里取消逐音符的短线，改用一条跨全组的绝对定位线；十六分/三十二分\r\n"
              "   再向下每 4px 叠一条平行线。line 的纵向位置 = 八度点区(12px) + 数字行高，\r\n"
              "   与 .num 的 border-bottom 原本所在的位置对齐。 */")
BEAM_NEW_S = ("\r\n   .bgrp 里取消逐音符的短线，改用一条跨全组的绝对定位线；十六分/三十二分\r\n"
              "   再向下每 4px 叠一条平行线。line 的纵向位置 = 八度点区 + 数字行高，\r\n"
              "   与 .num 的 border-bottom 原本所在的位置对齐。八度点区默认 12px；三个点\r\n"
              "   起 .oct 被撑高到 19/26/33px，flushBeam 会把实际高度写进 --oct-h，减时线\r\n"
              "   随之下移，否则会浮到数字上方、画进八度点区。 */")
BEAM_OLD_B = BEAM_OLD_S.encode("utf-8")
BEAM_NEW_B = BEAM_NEW_S.encode("utf-8")
TOP_OLD_S = "top:calc(12px + 1.15em - 1px)"
TOP_NEW_S = "top:calc(var(--oct-h, 12px) + 1.15em - 1px)"
TOP_OLD_B = TOP_OLD_S.encode("ascii")
TOP_NEW_B = TOP_NEW_S.encode("ascii")


def normalize_beam_oct_css(html):
    """把 v1.151.0 的减时线 CSS 归一回旧写法，支持 str / bytes。

    只动注释块与 .bgrp::after 的 top 声明两处；bgrp 元素上的 --oct-h 属性
    只会出现在本轮新素材（已在新素材跳过表），既有素材不受影响。
    """
    if isinstance(html, bytes):
        return html.replace(BEAM_NEW_B, BEAM_OLD_B).replace(TOP_NEW_B, TOP_OLD_B)
    return html.replace(BEAM_NEW_S, BEAM_OLD_S).replace(TOP_NEW_S, TOP_OLD_S)


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

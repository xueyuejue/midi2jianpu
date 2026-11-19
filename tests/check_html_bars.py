#!/usr/bin/env python3
"""HTML 逐小节列连续性校验。

简谱的每一列都「接在上一列后面」：休止列把游标推到下一列该在的位置，音符列
按自己的时值推进。所以只要某一列被漏掉，它后面所有列的起点就会整体前移——
这一小节读出来的拍数就少了，小节线也跟着提前。

HTML 曾经在两条路径上漏列：
  * v1.61.0 休止符按最近档近似（已修）；
  * v1.144.0 倚音分支直接 `continue`，把小节开启与「倚音前面那段空拍」一起跳过，
    parts.mid 的 2 拍弱起只画 1 拍、overfull.mid 第 3 小节少 1 拍。

本脚本用「MusicXML 的小节起点」当标尺：HTML 每小节从该小节起点开始累计列时值，
每个带 data-t 的音符列都必须落在累计位置上。判据只看「列起点」，不看小节末尾，
所以文本谱 / HTML 都不补的尾部休止（MusicXML 必须补）不会误报。

用法：
  python tests/check_html_bars.py                # 全量审计
  python tests/check_html_bars.py --self-test    # 纯逻辑自测
  python tests/check_html_bars.py --exe PATH     # 指定可执行文件
  python tests/check_html_bars.py --regress OLD  # 拿旧版跑，本轮应报出修复前的问题
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 与 src/html_writer.cpp 的列样式同源（单位：拍）
BASE = {"thirtysecond": 0.125, "sixteenth": 0.25, "eighth": 0.5,
        "quarter": 1.0, "half": 2.0, "whole": 4.0}
EXT = {"half": {1: 2.0, 2: 3.0}, "whole": {3: 4.0, 5: 6.0}}
NOTE_OPEN = "<span class='note "
DIV_RE = re.compile(r"<divisions>(\d+)</divisions>")


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def take_span(s, start):
    """从 start（指向 '<span'）起返回配对的整段 span。"""
    i = start
    depth = 0
    while True:
        nxt_open = s.find("<span", i)
        nxt_close = s.find("</span>", i)
        if nxt_close < 0:
            return s[start:]
        if 0 <= nxt_open < nxt_close:
            depth += 1
            i = nxt_open + 5
        else:
            depth -= 1
            i = nxt_close + 7
            if depth == 0:
                return s[start:i]


def columns(frag):
    """一段小节 HTML -> [(data-t 或 None, 拍数)]，按出现顺序。"""
    out = []
    i = 0
    while True:
        j = frag.find(NOTE_OPEN, i)
        if j < 0:
            break
        col = take_span(frag, j)
        m = re.match(r"<span class='note ([^']*)'([^>]*)>", col)
        cls, attrs = m.group(1), m.group(2)
        mt = re.search(r"data-t='(\d+)'", attrs)
        tick = int(mt.group(1)) if mt else None
        out.append((tick, col_beats(cls, attrs, col)))
        i = j + len(col)
    return out


def col_beats(cls, attrs, frag=""):
    """一列画出来的拍数；认不出时基样式返回 None。"""
    parts = cls.split()
    base = next((p for p in parts if p in BASE), None)
    if base is None:
        return None
    m = re.search(r"--n:(\d+)", attrs)
    n = int(m.group(1)) if m else 0
    if n:
        return EXT.get(base, {}).get(n)
    # 和弦列的附点挂在内部 .tone 上（整列只有最下面那个音画点），
    # 不在列 class 里，得看整段。
    dotted = "dotted" in parts or ("dotted" in frag and "chordcol" in parts)
    return BASE[base] * (1.5 if dotted else 1.0)


def html_tracks(html):
    """[(该段每小节的 [(tick, beats), ...], 是否含三连音), ...]。"""
    out = []
    for block in html.split("<div class='track'")[1:]:
        bars = []
        for m in block.split("<span class='measure'")[1:]:
            bars.append((columns(m), "class='trip'" in m))
        out.append(bars)
    return out


def xml_parts(xml):
    """[(该段每小节的 (起点 tick, division, 是否含小于门限的碎休止)), ...]。"""
    out = []
    for pm in re.findall(r'<part id="[^"]*"[^>]*>(.*?)</part>', xml, re.S):
        divm = DIV_RE.search(pm)
        div = int(divm.group(1)) if divm else 480
        gate = max(1, div // 2)             # 与 restGateTicks 同源
        bars = []
        t = 0
        for mb in re.findall(r'<measure number="[^"]*"[^>]*>(.*?)</measure>', pm, re.S):
            tot = 0
            tiny = False
            for nb in re.findall(r"<note>(.*?)</note>", mb, re.S):
                if "<chord/>" in nb:
                    continue
                du = re.search(r"<duration>(\d+)</duration>", nb)
                d = int(du.group(1)) if du else 0
                if "<rest" in nb and 0 < d < gate:
                    tiny = True
                tot += d
            bars.append((t, div, tiny))
            t += tot
        out.append(bars)
    return out


def bar_errors(cols, bar_start, div, gate):
    """一小节内列起点与累计位置的偏差；返回问题描述列表。"""
    errs = []
    t = bar_start
    for tick, beats in cols:
        if beats is None:
            return errs                     # 认不出这一列，整小节放过
        if tick is not None and abs(tick - t) >= gate:
            errs.append("列起点 %d != 累计 %d" % (tick, t))
            return errs
        t += int(round(beats * div))
    return errs


def audit(exe, verbose=True):
    errors = []
    skipped = 0
    tripped = 0
    files = sorted(glob.glob(os.path.join(FIX, "*.mid")))
    with tempfile.TemporaryDirectory() as td:
        for mid in files:
            name = os.path.basename(mid)
            h = os.path.join(td, "a.html")
            x = os.path.join(td, "a.xml")
            subprocess.run([exe, mid, "--html", h, "--musicxml", x, "--player"],
                           capture_output=True)
            try:
                hs = open(h, encoding="utf-8").read()
                xs = open(x, encoding="utf-8").read()
            except OSError:
                errors.append("%s: 渲染失败" % name)
                continue
            ht, xp = html_tracks(hs), xml_parts(xs)
            if len(ht) != len(xp):
                # HTML 把复调轨按声部拆成多行，MusicXML 仍是一个 part：
                # 两边小节没法一一对齐，这里放过（由歌词 / 声部守卫覆盖）。
                skipped += 1
                continue
            for pi, (hb, xb) in enumerate(zip(ht, xp)):
                if len(hb) != len(xb):
                    errors.append("%s: 第 %d 段小节数 HTML %d / XML %d"
                                  % (name, pi, len(hb), len(xb)))
                    continue
                for bi, ((cols, trip), (start, div, tiny)) in enumerate(zip(hb, xb)):
                    if trip or tiny:
                        # 三连音列按 1/3 拍，样式档位还原不了；小于休止门限的
                        # 碎休止文本谱与 HTML 都按设计省略（MusicXML 必须写），
                        # 这两类小节不适用本判据。
                        tripped += 1
                        continue
                    gate = max(1, div // 2)     # 与 restGateTicks 同源
                    for msg in bar_errors(cols, start, div, gate):
                        errors.append("%s: 第 %d 段第 %d 小节 %s"
                                      % (name, pi, bi, msg))
    if verbose:
        print("fixtures %d / 跳过复调 %d / 跳过三连音与碎休止小节 %d"
              % (len(files), skipped, tripped))
        for e in errors[:20]:
            print("  [FAIL] " + e)
        print("HTML 列连续性: %s（%d 项问题）"
              % ("通过" if not errors else "失败", len(errors)))
    return errors


def self_test():
    fails = 0
    total = 0

    def ck(cond, msg):
        nonlocal fails, total
        total += 1
        if not cond:
            fails += 1
            print("  [FAIL] " + msg)

    # 1. 正常小节：休止 + 音符，累计对得上
    html = ("<span class='measure'><span class='notes'>"
            "<span class='note quarter'><span class='num'>0</span></span>"
            "<span class='note quarter' data-t='480'><span class='num'>1</span></span>"
            "</span></span>")
    cols = columns(html)
    ck(len(cols) == 2, "两列都解析出来（%d）" % len(cols))
    ck(bar_errors(cols, 0, 480, 240) == [], "休止 1 拍后音符落在 480")

    # 2. 漏掉开头的休止列：首列 480 出现在小节起点 0
    html2 = ("<span class='measure'><span class='notes'>"
             "<span class='note quarter' data-t='480'><span class='num'>1</span></span>"
             "</span></span>")
    errs = bar_errors(columns(html2), 0, 480, 240)
    ck(len(errs) == 1, "漏掉首列休止会被抓到（%d 项）" % len(errs))

    # 3. 漏掉中间的休止列：0.5 拍列后紧跟 1 拍后的音
    html3 = ("<span class='notes'>"
             "<span class='note eighth'><span class='num'>0</span></span>"
             "<span class='note quarter' data-t='480'><span class='num'>1</span></span>"
             "</span>")
    ck(len(bar_errors(columns(html3), 0, 480, 240)) == 1,
       "中间漏 0.5 拍休止会被抓到")

    # 4. 小于门限的空拍（0.25 拍）按设计不印，不报错
    html4 = ("<span class='notes'>"
             "<span class='note sixteenth' data-t='120'><span class='num'>1</span></span>"
             "</span>")
    ck(bar_errors(columns(html4), 0, 480, 240) == [],
       "小于休止门限的空拍不报错")

    # 5. 附点与延长线：half --n:1 = 2 拍、--n:2 = 3 拍
    ck(col_beats("half", " style='--n:1'") == 2.0, "--n:1 的二分音符算 2 拍")
    ck(col_beats("half", " style='--n:2'") == 3.0, "--n:2 的二分音符算 3 拍")
    ck(col_beats("quarter dotted", "") == 1.5, "附点四分音符算 1.5 拍")

    # 6. 和弦列的附点挂在 .tone 上，也要算进去
    frag = ("<span class='note chordcol eighth'><span class='core'>"
            "<span class='stack'><span class='tone dotted'><span class='num'>1</span>"
            "</span></span></span></span>")
    cols6 = columns(frag)
    ck(len(cols6) == 1 and abs(cols6[0][1] - 0.75) < 1e-9,
       "和弦列的附点被算进时值（%.3f）" % (cols6[0][1] if cols6 else -1))

    # 7. 小节切分：两个 measure 各自成段
    two = html + html
    ck(len(html_tracks("<div class='track'><h3>x</h3>" + two)[0]) == 2,
       "两个小节切成两段")

    print("自测 %d 项，失败 %d 项" % (total, fails))
    return fails


def main():
    if "--self-test" in sys.argv:
        return 1 if self_test() else 0
    exe = default_exe()
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    if "--regress" in sys.argv:
        exe = sys.argv[sys.argv.index("--regress") + 1]
        print("[回归] 旧版可执行文件：%s（本轮修复的问题应当在这里报出来）" % exe)
    return 1 if audit(exe) else 0


if __name__ == "__main__":
    sys.exit(main())

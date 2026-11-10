#!/usr/bin/env python3
"""三端结构一致性校验（v1.62.0）。

v1.58.0 ~ v1.61.0 把「休止符时值 / 三端拍数」逐端对齐：文本谱靠 exactRest、
MusicXML 靠 splitRestDuration()、HTML 靠 splitRestBeats()，并用
`check_html_rests.py` 让 HTML 与 MusicXML 的休止段逐段对表。但「三端一致」
仍未在**结构上**被一次性钉死：文本谱的小节由 `|` 显式分隔、MusicXML 由
`<measure>` 分隔、HTML 由 `<span class='measure'>` 分隔，三者若是同一条
核心逻辑算出来的，节数量与拍号就必须逐轨、逐小节对得上。

本脚本钉死三件事（都不依赖脆弱的「逐音符时值求和」——和弦并发、多谱表
`<backup>`、三连音编码都会让时值累加失真，所以这里只比**结构**）：

  1. 轨数 + 逐轨小节数：文本谱与 HTML 共用同一套「按声部拆轨」逻辑，因此二者
     的轨数与每条轨的小节数必须逐条相等。这是最强、最直接的跨端不变量。
  2. 全局初始拍号（名义拍号，不含弱起缩短）：文本谱页眉 `拍号=X/Y` /
     MusicXML 第一个 `<time>` / HTML `<div class="meta">` 的 `X/Y` 三端相等。
  3. 逐小节拍号（名义拍号）：文本谱与 MusicXML 按对齐的下标逐轨对表（变拍号），
     弱起小节（首小节）与末小节豁免——这两小节两侧表达方式本就不同，不参与比对。

为什么不直接比「文本谱 vs MusicXML 的轨数」：MusicXML 把多声部收进同一个
`<part>` 的 `<voice>` 里，而文本谱 / HTML 会按声部拆成多条轨，这是合理的格式
表达差异，不是缺陷，所以 XML 只在校验「名义拍号」与「按对齐下标的逐轨小节数 /
逐小节拍号」时参与，轨数不和文本谱强行对齐。

本脚本同时是 v1.62.0 一个真实 HTML 渲染缺陷的回归测试：旧版 HTML writer 在
「音符跨小节 + 同拍多音（和弦）」时，会把分段按「音符」而非「小节」交错，使
渲染循环的小节号回退、把已画过的小节重画一遍，钢琴轨凭空多出十几二十个小节
（如 parts.mid 钢琴轨渲染出 21 个小节，而文本谱只有 5 个）。修复后三端结构
逐轨对齐，本脚本必须稳定输出「0 处不一致」。

用法：
  python tests/check_three_way.py                  # 校验全部 fixture
  python tests/check_three_way.py --self-test      # 纯逻辑自测
  python tests/check_three_way.py --regress OLD    # 对旧版做结构性零回归
  python tests/check_three_way.py --exe PATH       # 指定可执行文件
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 复用文本谱解析里已经验证过的拍号解析（与 check_bar_beats.py 同源），
# 避免重写漂移。
sys.path.insert(0, os.path.join(ROOT, "tests"))
import check_bar_beats as cb  # noqa: E402


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


# ---------------------------------------------------------------------------
# 文本谱：逐轨、逐小节的**名义**拍号序列（不含弱起缩短；弱起由 check_bar_beats
# 单独管）。返回 [(轨名, [每小节名义拍号, ...]), ...] 以及全局名义拍号。
# ---------------------------------------------------------------------------
def text_tracks(lines):
    """返回 [(轨名, [每小节名义拍号, ...]), ...]；拍号单位与 check_bar_beats 一致
    （4/4 -> 4.0，3/4 -> 3.0，6/8 -> 3.0）。弱起小节也按名义拍号计，不与 XML 冲突。

    变拍号 `changes` 的键是「第 at 小节起」（1-based，与 MusicXML 的 measure
    编号对齐：弱起小节即第 1 小节），所以第 i 个（0-based）小节对应 at = i+1。
    """
    base, changes, _pickup, _has = cb.parse_meters(lines)
    tracks = []
    cur = None
    for l in lines:
        if re.search(r"^\[[^\]]*\] \(\d+ 音符", l):
            if cur is not None:
                tracks.append(cur)
            cur = {"name": l.strip(), "meters": []}
            continue
        if cur is not None and "|" in l:
            for _s in l.split("|")[:-1]:
                cur["meters"].append(None)  # 占位，下面按序号套用名义拍号
    if cur is not None:
        tracks.append(cur)
    for tr in tracks:
        for i in range(len(tr["meters"])):
            meter = base
            for at in sorted(changes):
                if (i + 1) >= at:
                    meter = changes[at]
            tr["meters"][i] = meter
    return tracks


def text_nominal(lines):
    """全局名义拍号（文本谱页眉 `拍号=X/Y`）。"""
    base, _c, _p, _h = cb.parse_meters(lines)
    return base


# ---------------------------------------------------------------------------
# MusicXML：逐 part、逐 measure 的拍号序列（名义拍号）。
# ---------------------------------------------------------------------------
def _time_block(seg):
    """从一段 measure 文本里取 `<time>` 的拍号；没有 `<time>` 返回 None。"""
    tm = re.search(r"<time>.*?</time>", seg, re.S)
    if not tm:
        return None
    b = re.search(r"<beats>(\d+)</beats>", tm.group(0))
    d = re.search(r"<beat-type>(\d+)</beat-type>", tm.group(0))
    if not b or not d:
        return None
    return int(b.group(1)) * 4.0 / int(d.group(1))


def xml_parts(xml):
    """返回 [[每小节拍号, ...], ...]，每个 part 一条；`<time>` 只在变更处出现，
    其余小节沿用上一个小节的拍号。"""
    parts = []
    for part in re.split(r"(?=<part[ >])", xml):
        if "<measure" not in part:
            continue
        meters = []
        cur = None
        for m in part.split("<measure")[1:]:
            e = m.find("</measure>")
            seg = m[:e] if e >= 0 else m
            tb = _time_block(seg)
            if tb is not None:
                cur = tb
            meters.append(cur)
        parts.append(meters)
    return parts


def xml_nominal(xml):
    """从整个文档里取第一个 `<time>` 作为全局名义拍号；没有返回 None。"""
    tm = re.search(r"<time>.*?</time>", xml, re.S)
    if not tm:
        return None
    b = re.search(r"<beats>(\d+)</beats>", tm.group(0))
    d = re.search(r"<beat-type>(\d+)</beat-type>", tm.group(0))
    if not b or not d:
        return None
    return int(b.group(1)) * 4.0 / int(d.group(1))


# ---------------------------------------------------------------------------
# HTML：逐 track 的小节数 + 全局初始拍号（用真正的 HTML 解析，避免嵌套 div /
# 跨轨重叠计数）。
# ---------------------------------------------------------------------------
class _HtmlTracks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tracks = []        # 每条轨的小节数
        self._cur = None
        self._depth = 0         # 当前 track div 的嵌套深度（进入 track 后为 >0）

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = a.get("class", "")
        if tag == "div" and cls.startswith("track"):
            if self._depth == 0 and self._cur is not None:
                self.tracks.append(self._cur)
            self._depth += 1
            if self._depth == 1:
                self._cur = 0
        elif tag == "div" and self._depth > 0:
            self._depth += 1
        elif tag == "span" and "measure" in cls.split() and self._depth > 0:
            self._cur += 1

    def handle_endtag(self, tag):
        if tag == "div" and self._depth > 0:
            self._depth -= 1
            if self._depth == 0 and self._cur is not None:
                self.tracks.append(self._cur)
                self._cur = None


def html_tracks(htm):
    """返回每个 `<div class='track'>` 里 `<span class='measure'>` 的个数。"""
    p = _HtmlTracks()
    p.feed(htm)
    return p.tracks


def html_global_meter(htm):
    """从 `<div class="meta">` 里抓全局初始拍号 `X/Y`，没有返回 None。"""
    m = re.search(r"<div class=\"meta\">.*?(\d+)/(\d+)", htm, re.S)
    if not m:
        return None
    return int(m.group(1)) * 4.0 / int(m.group(2))


# ---------------------------------------------------------------------------
# 比对
# ---------------------------------------------------------------------------
def compare_fixture(exe, mid):
    """返回该 fixture 的违规列表 [(描述, 细节), ...]。"""
    with tempfile.TemporaryDirectory() as td:
        t = os.path.join(td, "t.txt")
        h = os.path.join(td, "h.html")
        x = os.path.join(td, "x.xml")
        subprocess.run([exe, mid, "--output", t], capture_output=True,
                       timeout=120, check=True)
        subprocess.run([exe, mid, "--html", h], capture_output=True,
                       timeout=120, check=True)
        subprocess.run([exe, mid, "--musicxml", x], capture_output=True,
                       timeout=120, check=True)
        tl = open(t, encoding="utf-8", errors="replace").read().splitlines()
        hh = open(h, encoding="utf-8", errors="replace").read()
        xx = open(x, encoding="utf-8", errors="replace").read()

    tt = text_tracks(tl)
    xp = xml_parts(xx)
    ht = html_tracks(hh)
    tnm = text_nominal(tl)
    xnm = xml_nominal(xx)
    hnm = html_global_meter(hh)
    name = os.path.basename(mid)
    bad = []

    # 1) 文本谱 vs HTML：轨数
    if len(tt) != len(ht):
        bad.append(("文本谱 vs HTML 轨数不一致",
                     "%s 文本=%d / HTML=%d" % (name, len(tt), len(ht))))

    # 2) 文本谱 vs HTML：逐轨小节数
    n_th = min(len(tt), len(ht))
    for i in range(n_th):
        ct, ch = len(tt[i]["meters"]), ht[i]
        if ct != ch:
            bad.append(("第%d轨文本谱 vs HTML 小节数不一致" % (i + 1),
                        "%s 文本=%d / HTML=%d" % (name, ct, ch)))

    # 3) 全局名义拍号：三端相等
    for label, val in (("文本谱", tnm), ("MusicXML", xnm), ("HTML", hnm)):
        if val is None:
            bad.append(("全局拍号缺失", "%s %s" % (name, label)))
    vals = [v for v in (tnm, xnm, hnm) if v is not None]
    if len(set(round(v, 6) for v in vals)) > 1:
        bad.append(("全局名义拍号不一致",
                    "%s 文本=%s / XML=%s / HTML=%s" % (name, tnm, xnm, hnm)))

    # 4) 文本谱 vs MusicXML：按对齐下标逐轨小节数 + 逐小节名义拍号
    n_tx = min(len(tt), len(xp))
    for i in range(n_tx):
        ct, cx = len(tt[i]["meters"]), len(xp[i])
        if ct != cx:
            bad.append(("第%d轨文本谱 vs XML 小节数不一致" % (i + 1),
                        "%s 文本=%d / XML=%d" % (name, ct, cx)))
        atm = tt[i]["meters"]
        axm = xp[i]
        m = min(len(atm), len(axm))
        for j in range(m):
            if j == 0 or j == m - 1:
                continue  # 弱起（首小节）/ 末小节豁免
            if atm[j] is None or axm[j] is None:
                continue
            if abs(atm[j] - axm[j]) > 1e-6:
                bad.append(("第%d轨第%d小节名义拍号不一致" % (i + 1, j + 1),
                            "%s 文本=%s / XML=%s" % (name, atm[j], axm[j])))
    return bad


def audit_fixtures(exe):
    bad = []
    for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
        bad.extend(compare_fixture(exe, p))
    return bad


def self_test():
    n = 0
    failed = []

    def ck(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    # parse_meters 已在 check_bar_beats 自测；这里只验本脚本的封装。
    cb.parse_meters(["拍号=3/4"])
    # 文本谱逐轨名义拍号：3/4 -> 全部 3.0（弱起不改变名义拍号）
    lines = ["拍号=3/4", "[V] (4 音符)", "1 2 3  |", "1 2 3  |"]
    tt = text_tracks(lines)
    ck(len(tt) == 1, "单轨")
    ck(tt[0]["meters"] == [3.0, 3.0], "名义拍号序列: %r" % tt[0]["meters"])

    # 弱起：名义拍号仍为 4/4（不缩短）
    lines2 = ["弱起 2 拍", "拍号=4/4", "[V] (4 音符)", "1 2  |", "1 2 3 4  |"]
    tt2 = text_tracks(lines2)
    ck(tt2[0]["meters"] == [4.0, 4.0], "弱起名义拍号序列: %r" % tt2[0]["meters"])
    ck(abs(text_nominal(lines2) - 4.0) < 1e-6, "弱起全局名义拍号")

    # 变拍号：第 3 小节起 3/4（第 3、4 小节都按 3/4）
    lines3 = ["4/4 -> 3/4(第3小节)", "[V] (4 音符)",
              "1 2 3 4  |", "1 2 3 4  |", "1 2 3  |", "1 2 3 4  |"]
    tt3 = text_tracks(lines3)
    ck(tt3[0]["meters"] == [4.0, 4.0, 3.0, 3.0], "变拍号序列: %r" % tt3[0]["meters"])

    # MusicXML 拍号：第一小节 4/4，第三小节变 3/4
    xml = ("<part id='P1'><measure number='1'><time><beats>4</beats>"
           "<beat-type>4</beat-type></time></measure>"
           "<measure number='2'></measure>"
           "<measure number='3'><time><beats>3</beats>"
           "<beat-type>4</beat-type></time></measure>"
           "<measure number='4'></measure></part>")
    xp = xml_parts(xml)
    ck(len(xp) == 1 and xp[0] == [4.0, 4.0, 3.0, 3.0], "XML 变拍号序列: %r" % xp)
    ck(abs(xml_nominal(xml) - 4.0) < 1e-6, "XML 全局名义拍号")

    # HTML 小节数 / 全局拍号（用真正的解析器）
    htm = ("<div class=\"meta\">1=C 大调 ｜ 3/4 ｜</div>"
           "<div class='track'><div class='staff'><div class='row'>"
           "<span class='measure'></span>"
           "<span class='measure'></span></div></div></div>")
    ck(html_tracks(htm) == [2], "HTML 小节数: %r" % html_tracks(htm))
    ck(abs(html_global_meter(htm) - 3.0) < 1e-6, "HTML 全局拍号")

    htm2 = ("<div class=\"meta\">1=C 大调 ｜ 6/8 ｜</div>"
            "<div class='track'><div class='staff'><div class='row'>"
            "<span class='measure'></span></div></div></div>")
    ck(abs(html_global_meter(htm2) - 3.0) < 1e-6, "6/8 -> 3.0: %r" % html_global_meter(htm2))

    # HTML 解析器对嵌套/多轨不串档
    htm3 = ("<div class='track'><div class='staff'><div class='row'>"
            "<span class='measure'></span></div></div></div>"
            "<div class='track'><div class='staff'><div class='row'>"
            "<span class='measure'></span><span class='measure'></span>"
            "</div></div></div>")
    ck(html_tracks(htm3) == [1, 2], "HTML 多轨计数: %r" % html_tracks(htm3))

    print("check_three_way self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 0 if not failed else 1


def _render_all(exe, mid, td, tag):
    t = os.path.join(td, tag + ".txt")
    h = os.path.join(td, tag + ".html")
    x = os.path.join(td, tag + ".xml")
    subprocess.run([exe, mid, "--output", t], capture_output=True, timeout=120, check=True)
    subprocess.run([exe, mid, "--html", h], capture_output=True, timeout=120, check=True)
    subprocess.run([exe, mid, "--musicxml", x], capture_output=True, timeout=120, check=True)
    return t, h, x


def structural_regress(old_exe, new_exe):
    errors = []
    with tempfile.TemporaryDirectory() as td:
        for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
            name = os.path.basename(p)
            ot, oh, ox = _render_all(old_exe, p, td, "old")
            nt, nh, nx = _render_all(new_exe, p, td, "new")
            # 新版自身结构必须合法
            new_bad = compare_fixture(new_exe, p)
            if new_bad:
                errors.append("%s: 新版自身结构违规 %r" % (name, new_bad))
            # 跨版本：文本谱逐轨结构、HTML 逐轨小节数、XML 逐轨小节数必须不变
            otl = open(ot, encoding="utf-8", errors="replace").read().splitlines()
            ntl = open(nt, encoding="utf-8", errors="replace").read().splitlines()
            if text_tracks(otl) != text_tracks(ntl):
                errors.append("%s: 文本谱逐轨结构随版本变化" % name)
            ohh = open(oh, encoding="utf-8", errors="replace").read()
            nhh = open(nh, encoding="utf-8", errors="replace").read()
            if html_tracks(ohh) != html_tracks(nhh):
                errors.append("%s: HTML 逐轨小节数随版本变化" % name)
            oxx = open(ox, encoding="utf-8", errors="replace").read()
            nxx = open(nx, encoding="utf-8", errors="replace").read()
            if xml_parts(oxx) != xml_parts(nxx):
                errors.append("%s: MusicXML 逐轨小节数随版本变化" % name)
    return errors


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if "--regress" in args:
        old_exe = args[args.index("--regress") + 1]
        errors = structural_regress(old_exe, exe)
        print("结构性零回归：%d 份素材" % len(glob.glob(os.path.join(FIX, "*.mid"))))
        for e in errors:
            print("  [FAIL]", e)
        return 0 if not errors else 1
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad = audit_fixtures(exe)
    if bad:
        print("三端结构不一致：%d 处" % len(bad))
        for d, detail in bad[:40]:
            print("  %s: %s" % (d, detail))
        return 1
    print("三端结构一致：全部 fixture 0 处不一致（文本谱↔HTML 轨数/小节数；三端名义拍号）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

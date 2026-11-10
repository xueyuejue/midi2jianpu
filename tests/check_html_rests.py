#!/usr/bin/env python3
"""HTML 休止符时值精确性校验（v1.61.0）。

文本谱 v1.59.0 的 exactRest 与 MusicXML v1.58.0 的 splitRestDuration()
都已把不规则间隙拆成标准时值之和，HTML 却仍用 durStyleOf(restBeats)
取「最近档」画一个 0 列 —— 1.75 拍画成 1.5 拍、1.25 拍画成 1 拍，
同一份谱三端读出的拍数不一致。

v1.61.0 让 HTML 也按精确档位之和拆列。本脚本守护三件事：
  1. 拆分逻辑（与 src/html_writer.cpp::kExactRestSteps 同源）自测；
  2. 全部 fixture 的 HTML 休止段与 MusicXML 真实段逐段核对；
  3. 跨版本结构性零回归：差异只能是插入合法的 0 列，txt/xml 不许变。

判据说明：HTML 把跨小节长间隙按小节切开、MusicXML 却合并成一段，这类
既有分段口径差异会让 HTML 段值小于 XML 段值很多（>=1 拍）；而「按最近档
吸附」的特征是少 0~0.5 拍。只对后者报错，既不误伤既有差异，也不会漏掉
真正的近似写法。

用法：
  python tests/check_html_rests.py                     # 校验全部 fixture
  python tests/check_html_rests.py --self-test        # 纯逻辑自测
  python tests/check_html_rests.py --regress OLD      # 对旧版做结构性零回归
  python tests/check_html_rests.py --exe PATH         # 指定可执行文件
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 与 src/html_writer.cpp::kExactRestSteps 同源：units = 拍数 * 16。
# 只包含 durStyleOf() 能精确还原的档位，否则拆出来的列又会被取最近档。
EXACT_STEPS = (
    (96, 6.0), (64, 4.0), (48, 3.0), (32, 2.0), (24, 1.5),
    (16, 1.0), (12, 0.75), (8, 0.5), (6, 0.375), (4, 0.25), (2, 0.125),
)
BASE = {"thirtysecond": 0.125, "sixteenth": 0.25, "eighth": 0.5,
        "quarter": 1.0, "half": 2.0, "whole": 4.0}
EXT = {"half": {1: 2.0, 2: 3.0}, "whole": {3: 4.0, 5: 6.0}}

COL_RE = re.compile(r"<span class='note ([^']*)'([^>]*)>(.*?)</span></span>", re.S)
# 不带和弦 / 力度 / 断音 / 倚音标记的完整 0 列：restColumns 拆出来的续列
# 都是这种固定模板，用它做跨版本的结构性比对，避免字符级 difflib 把
# 两个完全相同的列串对齐到列中间。
REST_COL_RE = re.compile(
    r"<span class='note ([^']*)'([^>]*)><span class='oct'></span>"
    r"<span class='core'><span class='num'>0</span>"
    r"(?:<span class='exts'>.*?</span>)?</span>"
    r"<span class='oct low'></span><span class='lyric'></span></span>", re.S)
NOTE_RE = re.compile(r"<note>(.*?)</note>", re.S)
DUR_RE = re.compile(r"<duration>(\d+)</duration>")
DIV_RE = re.compile(r"<divisions>(\d+)</divisions>")
# HTML 页脚内嵌 M2J_VERSION，跨版本比对必须先归一化（与 regress.py 同口径）。
VER_RE = re.compile(r"midi2jianpu v\d+\.\d+\.\d+")
EPS = 1e-6
GAP_TOL = 0.5          # 「最近档吸附」最多少 0.5 拍


def norm_html(html):
    return VER_RE.sub("midi2jianpu vX.Y.Z", html)


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def split_rest_beats(gap_ticks, division):
    """C++ splitRestBeats 的等价实现。返回精确档拍数列表；[] 表示拆不干净。"""
    if gap_ticks <= 0 or division <= 0:
        return []
    scaled = gap_ticks * 16
    if scaled % division != 0:
        return []                      # 不是 1/16 拍的整数倍，旧兜底
    rem = scaled // division
    out = []
    while rem > 0:
        picked = False
        for units, beats in EXACT_STEPS:
            if units <= rem:
                out.append(beats)
                rem -= units
                picked = True
                break
        if not picked:
            return []                  # 余量不足 1/8 拍，旧兜底
    return out


def col_rest_beats(cls, attrs, body):
    """一个 .note 列若为休止符，返回它画出来的拍数；否则 None。"""
    if "class='stack'" in body or "<span class='num'>0" not in body:
        return None
    parts = cls.split()
    base = next((p for p in parts if p in BASE), None)
    if base is None:
        return None
    m = re.search(r"--n:(\d+)", attrs)
    n = int(m.group(1)) if m else 0
    if n:
        return EXT.get(base, {}).get(n)
    return BASE[base] * (1.5 if "dotted" in parts else 1.0)


def html_rest_segments(html):
    """把 HTML 里连续的 0 列合并成一段，返回各段总拍数。"""
    segs, cur = [], 0.0
    for cls, attrs, body in COL_RE.findall(html):
        b = col_rest_beats(cls, attrs, body)
        if b is None:
            if cur:
                segs.append(cur)
                cur = 0.0
        else:
            cur += b
    if cur:
        segs.append(cur)
    return segs


def xml_rest_segments(xml):
    """把 MusicXML 里连续的 <note><rest/> 合并成一段，返回各段真实拍数。"""
    m = DIV_RE.search(xml)
    div = int(m.group(1)) if m else 480
    segs, cur = [], 0
    for nb in NOTE_RE.findall(xml):
        if "<rest" in nb:
            du = DUR_RE.search(nb)
            if du:
                cur += int(du.group(1))
        else:
            if cur:
                segs.append(cur / div)
                cur = 0
    if cur:
        segs.append(cur / div)
    return segs


def find_approx(html_segs, xml_segs):
    """返回「按最近档吸附」的问题描述列表。

    HTML 段值若与某个 XML 段相等 -> 精确，通过。
    若比某 XML 段少 0~0.5 拍 -> 最近档吸附，报错。
    差得更多 -> HTML/XML 分段口径差异（跨小节长间隙），豁免。
    """
    out = []
    for g in html_segs:
        if any(abs(g - s) < EPS for s in xml_segs):
            continue
        near = [s for s in xml_segs if 0 < s - g <= GAP_TOL + EPS]
        if near:
            s = min(near)
            out.append("HTML 休止段 %.3f 拍比 MusicXML 真实段 %.3f 拍少 %.3f 拍"
                       "（按最近档近似）" % (g, s, s - g))
    return out


def render(exe, mid, td, tag):
    out = {}
    for flag, ext in (("--output", "txt"), ("--html", "html"),
                      ("--musicxml", "xml")):
        p = os.path.join(td, tag + "." + ext)
        subprocess.run([exe, mid, flag, p], capture_output=True)
        with open(p, "rb") as f:
            out[ext] = f.read()
    return out


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def audit(exe, verbose=True):
    errors = []
    total_segs = 0
    total_beats = 0.0
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            stem = os.path.splitext(os.path.basename(mid))[0]
            r = render(exe, mid, td, "a")
            hs = html_rest_segments(r["html"].decode("utf-8", "replace"))
            xs = xml_rest_segments(r["xml"].decode("utf-8", "replace"))
            total_segs += len(hs)
            total_beats += sum(hs)
            for msg in find_approx(hs, xs):
                errors.append("%s: %s" % (stem, msg))
    if verbose:
        print("fixtures %d / HTML 休止段 %d / 总拍数 %.3f"
              % (len(fixtures()), total_segs, total_beats))
        for e in errors:
            print("  [FAIL] " + e)
        print("全量审计: %s (%d 项问题)" % ("通过" if not errors else "失败",
                                          len(errors)))
    return errors


def pure_rest_columns(html):
    """返回 [(完整列 HTML, 该列拍数)]：不带标记的完整 0 列。"""
    # 这个正则本身已经锁定 num=0，body 传一个等价字面量给 col_rest_beats 即可。
    return [(m.group(0), col_rest_beats(m.group(1), m.group(2),
                                        "<span class='num'>0</span>"))
            for m in REST_COL_RE.finditer(html)]


def strip_pure_rest_columns(html):
    """抹掉所有不带标记的完整 0 列；剩下的字节跨版本必须逐字节一致。"""
    return REST_COL_RE.sub("", html)


def exact_beats(value):
    return value is not None and any(abs(value - b) < EPS
                                    for _, b in EXACT_STEPS)


def structural_regress(old_exe, new_exe):
    errors = []
    changed = []
    added_cols = 0
    added_beats = 0.0
    approx_before = 0
    approx_after = 0
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            stem = os.path.splitext(os.path.basename(mid))[0]
            a = render(old_exe, mid, td, "old")
            b = render(new_exe, mid, td, "new")
            for ext in ("txt", "xml"):
                if a[ext] != b[ext]:
                    errors.append("%s: %s 发生变化（本轮只应改 HTML）" % (stem, ext))
            old_html = norm_html(a["html"].decode("utf-8", "replace"))
            new_html = norm_html(b["html"].decode("utf-8", "replace"))
            xs = xml_rest_segments(b["xml"].decode("utf-8", "replace"))
            approx_before += len(find_approx(html_rest_segments(old_html), xs))
            approx_after += len(find_approx(html_rest_segments(new_html), xs))
            if old_html == new_html:
                continue
            changed.append(stem)
            old_pure = pure_rest_columns(old_html)
            new_pure = pure_rest_columns(new_html)
            if strip_pure_rest_columns(old_html) != strip_pure_rest_columns(new_html):
                errors.append("%s: HTML 差异超出纯 0 列区域" % stem)
            added = len(new_pure) - len(old_pure)
            if added <= 0:
                errors.append("%s: 没有新增 0 列" % stem)
            added_cols += max(added, 0)
            delta = (sum(v or 0 for _, v in new_pure)
                     - sum(v or 0 for _, v in old_pure))
            added_beats += delta
            if delta <= 0:
                errors.append("%s: 补回拍数不为正（%+.3f）" % (stem, delta))
            for _, v in new_pure:
                if not exact_beats(v):
                    errors.append("%s: 新增列不是精确档 %r" % (stem, v))
            # 新版必须比旧版更精确（近似问题严格减少）
    print("变化素材 %d 份 / 插入 0 列 %d 个 / 补回 %.3f 拍: %s"
          % (len(changed), added_cols, added_beats, ", ".join(changed)))
    print("近似问题: 旧版 %d 项 -> 新版 %d 项" % (approx_before, approx_after))
    if approx_after > approx_before:
        errors.append("新版近似问题反而变多（%d -> %d）"
                      % (approx_before, approx_after))
    if approx_after >= approx_before and changed:
        errors.append("有素材变化但近似问题没有减少")
    return errors


def self_test():
    n = 0

    def ck(cond, label):
        nonlocal n
        n += 1
        if not cond:
            raise AssertionError("self-test #%d: %s" % (n, label))

    exact = [
        (120, 0.25), (240, 0.5), (360, 0.75), (480, 1.0),
        (720, 1.5), (960, 2.0), (1440, 3.0), (1920, 4.0), (2880, 6.0),
    ]
    for ticks, beats in exact:
        parts = split_rest_beats(ticks, 480)
        ck(len(parts) == 1 and abs(parts[0] - beats) < EPS,
           "精确档 %g 拍不拆列" % beats)

    cases = [
        (840, [1.5, 0.25]),
        (600, [1.0, 0.25]),
        (1800, [3.0, 0.75]),
        (1200, [2.0, 0.5]),
        (1080, [2.0, 0.25]),
        (2400, [4.0, 1.0]),
        (1320, [2.0, 0.75]),
    ]
    for ticks, want in cases:
        parts = split_rest_beats(ticks, 480)
        ck(len(parts) == len(want) and all(abs(a - b) < EPS for a, b in zip(parts, want)),
           "%d tick 拆成 %s" % (ticks, want))
        ck(abs(sum(parts) - ticks / 480.0) < EPS,
           "%d tick 拆分后总拍数守恒" % ticks)

    ck(split_rest_beats(460, 480) == [], "0.958 拍（非 1/16 拍整数倍）走旧兜底")
    ck(split_rest_beats(0, 480) == [], "零间隙不拆")
    ck(split_rest_beats(480, 0) == [], "division 非法时走旧兜底")
    ck(split_rest_beats(1, 480) == [], "余量不足 1/8 拍走旧兜底")

    # 所有可拆档位两两组合都必须守恒
    for u1, b1 in EXACT_STEPS[:6]:
        for u2, b2 in EXACT_STEPS[-4:]:
            ticks = (u1 + u2) * 30          # units * division/16 = units * 30
            parts = split_rest_beats(ticks, 480)
            ck(abs(sum(parts) - (b1 + b2)) < EPS,
               "%g + %g 拍组合守恒" % (b1, b2))

    # HTML 列解析
    col = lambda cls, style="": ("<span class='note %s'%s><span class='oct'></span>"
                                 "<span class='core'><span class='num'>0</span></span>"
                                 "<span class='oct low'></span>"
                                 "<span class='lyric'></span></span>" % (cls, style))
    ck(col_rest_beats("quarter dotted", "", col("quarter dotted")) == 1.5,
       "quarter dotted = 1.5 拍")
    ck(col_rest_beats("sixteenth", "", col("sixteenth")) == 0.25,
       "sixteenth = 0.25 拍")
    ck(col_rest_beats("half", " style='--n:1'", col("half")) == 2.0,
       "half --n:1 = 2 拍")
    ck(col_rest_beats("whole", " style='--n:3'", col("whole")) == 4.0,
       "whole --n:3 = 4 拍")
    ck(col_rest_beats("quarter", "", col("quarter").replace(">0<", ">5<")) is None,
       "音符列不算休止")
    ck(html_rest_segments(col("quarter dotted") + col("sixteenth")) == [1.75],
       "连续两个 0 列合并成 1.75 拍一段")
    note_col = col("quarter").replace("<span class='num'>0</span>",
                                      "<span class='num'>5</span>")
    ck(html_rest_segments(col("quarter") + note_col + col("quarter")) == [1.0, 1.0],
       "音符列把休止段切断")

    # XML 段解析
    x = ("<divisions>480</divisions>"
         "<note><rest/><duration>720</duration></note>"
         "<note><rest/><duration>120</duration></note>"
         "<note><pitch><step>C</step></pitch><duration>480</duration></note>"
         "<note><rest/><duration>240</duration></note>")
    ck(xml_rest_segments(x) == [1.75, 0.5], "XML 连续 rest note 合并成段")

    # 审计判据
    ck(find_approx([1.5, 1.0], [1.75, 1.25]), "1.5 / 1.0 拍近似写法必须报错")
    ck(not find_approx([1.75, 1.25], [1.75, 1.25]), "精确拆列通过")
    ck(not find_approx([2.0], [5.0, 1.0]), "跨小节分段差异（差 3 拍）豁免")
    ck(not find_approx([4.0, 4.0], [8.0]), "整小节切开（差 4 拍）豁免")
    ck(find_approx([2.0], [2.5]), "2.5 拍画成 2 拍（差 0.5）必须报错")
    ck(not find_approx([], [1.75]), "HTML 没有休止列时不报错")

    ck(norm_html("midi2jianpu v1.60.0") == "midi2jianpu vX.Y.Z",
       "页脚版本号归一化")
    ck(norm_html("midi2jianpu v1.61.0") == norm_html("midi2jianpu v1.60.0"),
       "两个版本的 HTML 归一化后一致")

    # 结构性零回归的列级判据
    old_h = "ABC" + col("quarter") + "DEF"
    new_h = "ABC" + col("quarter") + col("sixteenth") + "DEF"
    ck(strip_pure_rest_columns(old_h) == strip_pure_rest_columns(new_h),
       "插入 0 列不改变其余字节")
    ck(len(pure_rest_columns(new_h)) == len(pure_rest_columns(old_h)) + 1,
       "新版多一个纯 0 列")
    ck(exact_beats(0.25) and not exact_beats(0.3), "精确档白名单")
    repl = "ABC" + col("eighth") + "DEF"
    ck(len(pure_rest_columns(repl)) == len(pure_rest_columns(old_h)),
       "替换旧列不会新增 0 列（结构判据据此报错）")
    ck(sum(v or 0 for _, v in pure_rest_columns(repl))
       < sum(v or 0 for _, v in pure_rest_columns(old_h)),
       "替换成更短时值会让补回拍数变负")
    print("自测通过：%d 项" % n)
    return n


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        self_test()
        return 0
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if "--regress" in args:
        old = args[args.index("--regress") + 1]
        errs = structural_regress(old, exe)
        for e in errs:
            print("  [FAIL] " + e)
        print("结构性零回归: %s (%d 项问题)" % ("通过" if not errs else "失败",
                                            len(errs)))
        return 1 if errs else 0
    errs = audit(exe)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())

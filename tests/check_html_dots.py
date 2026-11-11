#!/usr/bin/env python3
"""HTML 附点一致性校验（v1.60.0）。

文本谱用 `_.` / `.` / `=.` 写附点，MusicXML 用 `<dot/>`，HTML 却把
DurStyle::dotted 算出来后忘了拼进 class —— CSS 的 `.dotted .num::after`
一直在，却没有任何元素命中它。于是 0.75 拍画成八分、1.5 拍画成四分，
读者数出来的拍数凭空少 1/3。

约定（与文本谱 DUR_TABLE 对齐）：
  * 需要画点的档位只有 0.375 / 0.75 / 1.5 拍；
  * 3 拍 / 6 拍靠延音线条数表达（5 - - / 5 - - - - -），不画点；
  * 三连音走单独档位，不画点；
  * 和弦列的附点挂在最低音那个 <span class='tone'> 上，只画一次。

用法：
  python tests/check_html_dots.py                  # 校验全部 fixture
  python tests/check_html_dots.py --self-test     # 纯逻辑自测
  python tests/check_html_dots.py --regress OLD   # 对旧版做结构性零回归
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")
HTML_CPP = os.path.join(ROOT, "src", "html_writer.cpp")

# 与 src/html_writer.cpp::durStyleOf 同源；dotted 语义是「要画一个点」。
DUR_TABLE = (
    (0.125, "thirtysecond", False, 0, 3),
    (0.25, "sixteenth", False, 0, 2),
    (0.375, "sixteenth", True, 0, 2),
    (0.5, "eighth", False, 0, 1),
    (0.75, "eighth", True, 0, 1),
    (1.0, "quarter", False, 0, 0),
    (1.5, "quarter", True, 0, 0),
    (2.0, "half", False, 1, 0),
    (3.0, "half", False, 2, 0),
    (4.0, "whole", False, 3, 0),
    (6.0, "whole", False, 5, 0),
)
DOT_BEATS = (0.375, 0.75, 1.5)
DOT_CLASSES = ("sixteenth", "eighth", "quarter")

NOTE_CLASS_RE = re.compile(r"<span class='note ([^']*)'")
TONE_CLASS_RE = re.compile(r"<span class='tone([^']*)'")
NOTE_RE = re.compile(r"<note>(.*?)</note>", re.S)
# HTML 页脚内嵌 M2J_VERSION，跨版本比对必须先归一化，否则 89 份素材
# 都会因为版本号字符串变化而误报（与 regress.py 同一口径）。
VER_RE = re.compile(r"midi2jianpu v\d+\.\d+\.\d+")


def norm_html(html):
    return VER_RE.sub("midi2jianpu vX.Y.Z", html)


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "midi2jianpu")


def dur_style(beats):
    """返回 (cls, dotted, extLines, beams)，取最近档，与 C++ 一致。"""
    best = min(DUR_TABLE, key=lambda t: abs(beats - t[0]))
    return best[1], best[2], best[3], best[4]


def triplet_style(beats):
    """三连音用独立档位，永不画附点。"""
    r = beats * 3.0
    if r < 0.34:
        return "thirtysecond", False, 0, 3
    if r < 0.75:
        return "sixteenth", False, 0, 2
    return "eighth", False, 0, 1


def note_class(beats, extra=""):
    cls, dotted, _, _ = dur_style(beats)
    return "note " + cls + (" dotted" if dotted else "") + extra


def chord_tone_classes(count, beats):
    """和弦列里只有最低音（最后一个 tone）带 dotted。"""
    _, dotted, _, _ = dur_style(beats)
    return ["tone" + (" dotted" if dotted and i == count - 1 else "")
            for i in range(count)]


def dotted_columns(html):
    """带附点的音符/休止符列 class 列表。"""
    return [c for c in NOTE_CLASS_RE.findall(html) if "dotted" in c.split()]


def dotted_tones(html):
    """和弦列里带附点的 tone 数。"""
    return sum(1 for c in TONE_CLASS_RE.findall(html) if "dotted" in c.split())


def illegal_dotted(html):
    """不该画点的位置：half/whole 档（靠延音线表达）或未知 class。"""
    bad = []
    for c in dotted_columns(html):
        parts = c.split()
        base = parts[1] if len(parts) > 1 and parts[0] == "chordcol" else parts[0]
        if base not in DOT_CLASSES:
            bad.append(c)
    return bad


def strip_dotted(html):
    """移除所有 ' dotted'，用于结构性零回归（改动必须只落在 class 上）。"""
    return html.replace(" dotted", "")


def short_dot_events(xml, include_rests=False):
    """MusicXML 里 <=1.5 拍且带 <dot/> 的音符数（grace 无 duration，跳过）。

    默认不计休止符：HTML 对不规则休止间隙按最近档近似成一列（v1.59.0
    既有约定，视觉排版而非可数拍数），而 MusicXML 会把它拆成标准时值之和，
    其中就含附点八分休止。两者不是一一对应，跨端要求「有 dot 就得有
    dotted 列」会误报。附点音符两端必须一致，才是本守护真正要守的东西。
    """
    m = re.search(r"<divisions>(\d+)</divisions>", xml)
    div = int(m.group(1)) if m else 480
    n = 0
    for body in NOTE_RE.findall(xml):
        if "<dot/>" not in body:
            continue
        if "<rest" in body and not include_rests:
            continue
        dm = re.search(r"<duration>(\d+)</duration>", body)
        if not dm:
            continue
        if int(dm.group(1)) <= div * 1.5 + 1:
            n += 1
    return n


def render(exe, mid, td, tag):
    stem = os.path.splitext(os.path.basename(mid))[0]
    out = {}
    for flag, ext in (("--output", "txt"), ("--html", "html"), ("--musicxml", "xml")):
        p = os.path.join(td, tag + "_" + stem + "." + ext)
        subprocess.run([exe, mid, flag, p], capture_output=True)
        out[ext] = open(p, "rb").read() if os.path.exists(p) else b""
    return out


def css_has_dotted_rule():
    src = open(HTML_CPP, "rb").read().decode("utf-8", "replace")
    return ".dotted .num::after" in src


def audit(exe, verbose=True):
    """全量审计：返回 error 列表与统计。"""
    errors = []
    stats = {"fixtures": 0, "dotted_cols": 0, "dotted_tones": 0,
             "xml_short_dots": 0, "fixtures_with_dots": 0}
    mids = sorted(glob.glob(os.path.join(FIX, "*.mid")))
    if not css_has_dotted_rule():
        errors.append("html_writer.cpp 里没有 .dotted .num::after 样式")
    with tempfile.TemporaryDirectory() as td:
        for mid in mids:
            stem = os.path.splitext(os.path.basename(mid))[0]
            o = render(exe, mid, td, "a")
            html = o["html"].decode("utf-8", "replace")
            xml = o["xml"].decode("utf-8", "replace")
            stats["fixtures"] += 1
            cols = dotted_columns(html)
            tones = dotted_tones(html)
            xd = short_dot_events(xml)
            stats["dotted_cols"] += len(cols)
            stats["dotted_tones"] += tones
            stats["xml_short_dots"] += xd
            if cols or tones:
                stats["fixtures_with_dots"] += 1
            for c in illegal_dotted(html):
                errors.append("%s: 不该画附点的列 class='note %s'" % (stem, c))
            # 有短附点事件却一个点都没画：整条通道又断了
            if xd > 0 and not cols and not tones:
                errors.append("%s: MusicXML 有 %d 个短附点音符，HTML 却没画点" % (stem, xd))
            # 专项素材：把各种档位钉死
            if stem == "html_dots":
                expect = {
                    "note eighth dotted": 3,
                    "note sixteenth dotted": 2,
                    "note quarter dotted": 2,
                }
                from collections import Counter
                got = Counter("note " + c for c in cols)
                for k, v in expect.items():
                    if got[k] != v:
                        errors.append("html_dots: %s 应有 %d 个，实际 %d"
                                      % (k, v, got[k]))
                if tones != 3:
                    errors.append("html_dots: 和弦列应只画 3 个点，实际 %d" % tones)
                if "class='note half dotted'" in html:
                    errors.append("html_dots: 3 拍长音不该画附点")
    if verbose:
        print("fixtures %d / HTML 附点列 %d / 和弦附点 %d / XML 短附点 %d / 含附点素材 %d"
              % (stats["fixtures"], stats["dotted_cols"], stats["dotted_tones"],
                 stats["xml_short_dots"], stats["fixtures_with_dots"]))
    return errors, stats


# v1.64.0 起 MusicXML 会写 <beam> 连梁；本脚本只管附点维度，
# XML 只要求「除连梁外逐字节一致」。
BEAM_RE = re.compile(rb'<beam number="\d">\w+</beam>')


def strip_beams(xml):
    return BEAM_RE.sub(b"", xml)


def structural_regress(old_exe, new_exe):
    """附点维度必须零变化；HTML 其他维度的变化降级为 [NOTE]。

    这个守护只负责「附点有没有画丢 / 画多」。v1.61.0 修 HTML 休止符拆列
    时会插入整个 0 列，HTML 逐字节必然变化，那是 check_html_rests.py 与
    regress.py 的职责；若这里继续要求「删掉 ' dotted' 后与旧版逐字节一致」，
    任何输出层改动都会被误判成附点回归。
    """
    errors = []
    notes = []
    changed = []
    added = 0
    mids = sorted(glob.glob(os.path.join(FIX, "*.mid")))
    with tempfile.TemporaryDirectory() as td:
        for mid in mids:
            stem = os.path.splitext(os.path.basename(mid))[0]
            a = render(old_exe, mid, td, "old")
            b = render(new_exe, mid, td, "new")
            for ext in ("txt", "xml"):
                if a[ext] != b[ext]:
                    if ext == "xml" and strip_beams(a[ext]) == strip_beams(b[ext]):
                        continue          # 只多了 <beam>，属 v1.64.0 的预期变化
                    errors.append("%s: %s 发生变化（本轮只应改 HTML）" % (stem, ext))
            old_html = norm_html(a["html"].decode("utf-8", "replace"))
            new_html = norm_html(b["html"].decode("utf-8", "replace"))
            if old_html == new_html:
                continue
            changed.append(stem)
            old_cols, new_cols = dotted_columns(old_html), dotted_columns(new_html)
            old_tones, new_tones = dotted_tones(old_html), dotted_tones(new_html)
            if old_cols != new_cols or old_tones != new_tones:
                errors.append("%s: 附点列/和弦附点发生变化 (%d/%d -> %d/%d)"
                              % (stem, len(old_cols), old_tones,
                                 len(new_cols), new_tones))
            if strip_dotted(new_html) != strip_dotted(old_html):
                notes.append("%s: HTML 还有附点之外的变化（由其他守护负责）" % stem)
            added += len(new_cols) + new_tones
            for c in illegal_dotted(new_html):
                errors.append("%s: 新增了不该画附点的列 %s" % (stem, c))
    print("变化素材 %d 份 / 新增附点 %d 个: %s"
          % (len(changed), added, ", ".join(changed)))
    for nt in notes:
        print("  [NOTE] " + nt)
    return errors


def self_test():
    """纯逻辑自测，不碰外部产物。"""
    cases = []

    def ck(cond, name):
        cases.append((bool(cond), name))

    ck(dur_style(0.125) == ("thirtysecond", False, 0, 3), "0.125 拍是三十二分")
    ck(dur_style(0.25) == ("sixteenth", False, 0, 2), "0.25 拍是十六分")
    ck(dur_style(0.375) == ("sixteenth", True, 0, 2), "0.375 拍是附点十六分")
    ck(dur_style(0.5) == ("eighth", False, 0, 1), "0.5 拍是八分")
    ck(dur_style(0.75) == ("eighth", True, 0, 1), "0.75 拍是附点八分")
    ck(dur_style(1.0) == ("quarter", False, 0, 0), "1.0 拍是四分")
    ck(dur_style(1.5) == ("quarter", True, 0, 0), "1.5 拍是附点四分")
    ck(dur_style(2.0) == ("half", False, 1, 0), "2.0 拍是二分加一条延音线")
    ck(dur_style(3.0) == ("half", False, 2, 0), "3.0 拍靠两条延音线，不画点")
    ck(dur_style(4.0) == ("whole", False, 3, 0), "4.0 拍靠三条延音线，不画点")
    ck(dur_style(6.0) == ("whole", False, 5, 0), "6.0 拍靠五条延音线，不画点")
    ck(sorted(b for b, _, d, _, _ in DUR_TABLE if d) == sorted(DOT_BEATS),
       "需要画点的档位恰好是 0.375 / 0.75 / 1.5")
    ck(dur_style(1.75)[1] is True, "最近档吸附：1.75 拍落在附点四分")
    ck(dur_style(1.2)[1] is False, "最近档吸附：1.2 拍落在四分，不画点")
    ck(dur_style(2.6)[1] is False and dur_style(2.6)[2] == 2,
       "最近档吸附：2.6 拍落在 3 拍档，靠延音线不画点")
    ck(triplet_style(1.0 / 3)[1] is False, "三连八分不画附点")
    ck(triplet_style(1.0 / 6)[1] is False, "三连十六分不画附点")
    ck("dotted" in note_class(1.5), "note_class 给附点四分拼上 dotted")
    ck("dotted" not in note_class(1.0), "note_class 不给四分拼 dotted")
    ck("dotted" not in note_class(3.0), "note_class 不给 3 拍拼 dotted")
    ck(note_class(1.5, " chordcol") == "note quarter dotted chordcol",
       "extraClass 仍拼在 dotted 之后，不影响既有解析")
    ck(chord_tone_classes(3, 0.75) == ["tone", "tone", "tone dotted"],
       "和弦附点只挂在最低音那个 tone 上")
    ck(chord_tone_classes(3, 1.0) == ["tone", "tone", "tone"],
       "非附点和弦一个 dotted 都没有")

    h = ("<span class='note eighth dotted'>x</span>"
         "<span class='note quarter'>y</span>"
         "<span class='note chordcol eighth'>"
         "<span class='tone'>a</span><span class='tone dotted'>b</span></span>")
    ck(len(dotted_columns(h)) == 1, "dotted_columns 只数列 class")
    ck(dotted_tones(h) == 1, "dotted_tones 数和弦里的附点 tone")
    ck(illegal_dotted(h) == [], "合法 HTML 没有越界附点")
    ck(illegal_dotted("<span class='note half dotted'>z</span>") ==
       ["half dotted"], "illegal_dotted 能抓到 3 拍档误画附点")
    ck(strip_dotted(h) == h.replace(" dotted", ""), "strip_dotted 只删 ' dotted'")
    ck(strip_dotted(h).count("dotted") == 0 and
       norm_html("midi2jianpu v1.60.0") == norm_html("midi2jianpu v1.59.0"),
       "strip_dotted 后无附点 class，且跨版本页脚已归一化")

    xml = ('<divisions>480</divisions>'
           '<note><duration>360</duration><type>eighth</type><dot/></note>'
           '<note><duration>1440</duration><type>half</type><dot/></note>'
           '<note><grace/><type>eighth</type><dot/></note>')
    ck(short_dot_events(xml) == 1, "short_dot_events 只数 <=1.5 拍的附点")
    ck(short_dot_events('<divisions>480</divisions>'
                        '<note><duration>720</duration><type>quarter</type><dot/></note>') == 1,
       "1.5 拍附点四分计入")
    ck(short_dot_events('<divisions>480</divisions>'
                        '<note><duration>1440</duration><type>half</type><dot/></note>') == 0,
       "3 拍附点二分不计入（延音线表达）")
    ck(short_dot_events('<divisions>480</divisions>'
                        '<note><rest/><duration>360</duration>'
                        '<type>eighth</type><dot/></note>') == 0,
       "不规则休止里的附点不计入跨端比对（HTML 按最近档近似）")
    ck(short_dot_events('<divisions>480</divisions>'
                        '<note><rest/><duration>360</duration>'
                        '<type>eighth</type><dot/></note>', include_rests=True) == 1,
       "include_rests=True 时休止附点照样能数")
    ck(css_has_dotted_rule(), "CSS 里 .dotted .num::after 规则存在")

    bad = [n for ok, n in cases if not ok]
    print("自测 %d 项，失败 %d 项" % (len(cases), len(bad)))
    for n in bad:
        print("  [FAIL]", n)
    return 1 if bad else 0


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--regress" in args:
        i = args.index("--regress")
        if i + 1 >= len(args):
            print("用法: check_html_dots.py --regress OLD_EXE")
            return 2
        errs = structural_regress(args[i + 1], exe)
        for e in errs:
            print("  [FAIL]", e)
        print("结构性零回归:", "通过" if not errs else "失败",
              "(%d 项问题)" % len(errs))
        return 1 if errs else 0
    errs, _ = audit(exe)
    for e in errs:
        print("  [FAIL]", e)
    print("全量审计:", "通过" if not errs else "失败", "(%d 项问题)" % len(errs))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())

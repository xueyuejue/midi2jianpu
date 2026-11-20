#!/usr/bin/env python3
"""MusicXML 时值一致性校验（v1.58.0）。

每个 <note> 的 <duration> 必须与 <type>/<dot/> 严格一致：MuseScore 按
<type> 画符号形状、按 <duration> 推进时间游标，两者不一致就会把休止符画错。
音符时值已经量化到标准档，休止符却曾按原始 tick 间隙原样输出，
1.75 拍被写成二分休止、3.75 拍被写成全休止。

例外：
  * <rest measure="yes"/> 全小节休止按惯例写 <type>whole</type>，
    duration 等于整小节长度（2/4 小节就是 2 拍），合法，不参与校验。
  * <grace/> 倚音没有 <duration>，跳过。
  * 三连音的实际时长是名义时值的 2/3，按 <time-modification> 折算。

用法：
  python tests/check_xml_durations.py                 # 校验全部 fixture
  python tests/check_xml_durations.py --self-test    # 纯逻辑自测
  python tests/check_xml_durations.py --regress OLD  # 对旧版做结构性零回归
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# v1.68.0 起 <direction> 的 <staff> 会按小节 / 拍位换谱表；该维度由
# check_xml_staff.py 单独守护，这里的跨版本逐字节比对先把它摘掉。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xml_norm import strip_direction_staff
import regress_skip

TYPE_BEATS = {
    "whole": 4.0, "half": 2.0, "quarter": 1.0, "eighth": 0.5,
    "16th": 0.25, "32nd": 0.125, "64th": 0.0625,
}

NOTE_RE = re.compile(r"<note>(.*?)</note>", re.S)
REST_LINE_RE = re.compile(r"^[ \t]*<note><rest[^>]*>[^\n]*</note>\r?\n", re.M)
# v1.64.0 起音符行会带 <beam> 连梁元素，比对「除休止符外逐字节一致」时
# 必须先剥掉，否则每份有八分音符的素材都会假报差异。
BEAM_RE = re.compile(r'<beam number="\d">[^<]+</beam>')


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "midi2jianpu")


def note_mismatches(xml):
    """返回 [(duration, 实际拍数, type, 是否附点, 名义拍数)]。"""
    m = re.search(r"<divisions>(\d+)</divisions>", xml)
    div = int(m.group(1)) if m else 480
    out = []
    for body in NOTE_RE.findall(xml):
        dm = re.search(r"<duration>(\d+)</duration>", body)
        if not dm:
            continue                       # grace note 不占时长
        if '<rest measure="yes"' in body:
            continue                       # 全小节休止是标准写法
        tm = re.search(r"<type>([^<]+)</type>", body)
        if not tm:
            continue
        dur = int(dm.group(1))
        beats = dur / div
        want = TYPE_BEATS.get(tm.group(1), 1.0)
        if "<dot/>" in body:
            want *= 1.5
        if "<time-modification>" in body:
            want *= 2.0 / 3.0
        if abs(beats - want) > 1e-6:
            out.append((dur, round(beats, 4), tm.group(1), "<dot/>" in body, round(want, 4)))
    return out


def strip_rest_lines(xml):
    """移除所有休止符 note 行与 <beam> 元素，用于比对「除休止符/连梁外逐字节一致」。"""
    return BEAM_RE.sub("", REST_LINE_RE.sub("", xml))


def rest_total(xml):
    """所有休止符 <duration> 之和（拆分前后必须守恒）。"""
    total = 0
    for body in NOTE_RE.findall(xml):
        if "<rest" not in body:
            continue
        dm = re.search(r"<duration>(\d+)</duration>", body)
        if dm:
            total += int(dm.group(1))
    return total


def render(exe, mid, td, tag):
    xml = os.path.join(td, tag + ".xml")
    subprocess.run([exe, mid, "--musicxml", xml], capture_output=True, timeout=120, check=True)
    return open(xml, encoding="utf-8", errors="replace").read()


def render_all(exe, mid, td, tag):
    txt = os.path.join(td, tag + ".txt")
    html = os.path.join(td, tag + ".html")
    xml = os.path.join(td, tag + ".xml")
    subprocess.run([exe, mid, "--output", txt, "--html", html, "--musicxml", xml],
                   capture_output=True, timeout=120, check=True)

    def rd(p):
        return open(p, encoding="utf-8", errors="replace").read()

    return (rd(txt),
            re.sub(r"midi2jianpu\s+v[\d.]+", "midi2jianpu vX", rd(html)),
            rd(xml))


def audit_fixtures(exe):
    bad = []
    with tempfile.TemporaryDirectory() as td:
        for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
            xml = render(exe, p, td, "check")
            for row in note_mismatches(xml):
                bad.append((os.path.basename(p), row))
    return bad


def structural_regress(old_exe):
    """与上一版逐字节比对；XML 允许只在休止符块内变化，且总时长守恒。

    只管 XML 这个维度：文本谱 / HTML 的变化由 check_bar_beats.py --regress
    与 build/regress.py 负责。跨多个版本比对时（例如 v1.57.0 -> v1.59.0）
    文本谱与 XML 可能分别在不同版本被修过，这类交叉差异只作 [NOTE] 提示，
    不算本脚本的错误；真正要拦的是「XML 差异越出休止符块」或时长不守恒。
    """
    new_exe = default_exe()
    changed = []
    errors = []
    notes = []
    with tempfile.TemporaryDirectory() as td:
        line = regress_skip.note()
        if line:
            print(line)
        for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
            name = os.path.basename(p)
            if regress_skip.skip(p):
                continue        # 本轮新增素材：旧版没有它，不算回归
            new_txt, new_html, new_xml = render_all(new_exe, p, td, "new")
            old_txt, old_html, old_xml = render_all(old_exe, p, td, "old")
            if new_xml == old_xml:
                continue
            changed.append(name)
            if new_txt != old_txt:
                notes.append(name + ": 文本谱同时变化（由 check_bar_beats.py 守护）")
            if new_html != old_html:
                notes.append(name + ": HTML 同时变化（由 regress.py 守护）")
            if (strip_direction_staff(strip_rest_lines(new_xml))
                    != strip_direction_staff(strip_rest_lines(old_xml))):
                errors.append(name + ": XML 差异不止休止符块")
            if rest_total(new_xml) != rest_total(old_xml):
                errors.append(name + ": 休止符总时长变化")
            if note_mismatches(new_xml):
                errors.append(name + ": 新版仍有 duration/type 不一致")
    return changed, errors, notes


def self_test():
    n = 0
    failed = []

    def ok(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    def doc(body, div=480):
        return "<score><divisions>%d</divisions><measure>%s</measure></score>" % (div, body)

    def note(dur, typ, dot=False, rest=False, extra=""):
        return ("<note><rest/>" if rest else "<note><pitch><step>C</step>"
                "<octave>4</octave></pitch>") + \
               "<duration>%d</duration><voice>1</voice><type>%s</type>" % (dur, typ) + \
               ("<dot/>" if dot else "") + extra + "</note>\n"

    ok(note_mismatches(doc(note(480, "quarter"))) == [],
       "四分音符精确匹配")
    ok(note_mismatches(doc(note(720, "quarter", dot=True))) == [],
       "附点四分音符精确匹配")
    ok(note_mismatches(doc(note(2880, "whole", dot=True))) == [],
       "附点全音符 6 拍精确匹配")
    ok(note_mismatches(doc(note(30, "64th"))) == [],
       "64 分音符精确匹配")
    ok(len(note_mismatches(doc(note(840, "half", rest=True)))) == 1,
       "1.75 拍写成二分休止应判不一致")
    ok(len(note_mismatches(doc(note(1800, "whole", rest=True)))) == 1,
       "3.75 拍写成全休止应判不一致")
    ok(len(note_mismatches(doc(note(600, "quarter", rest=True)))) == 1,
       "1.25 拍写成四分休止应判不一致")
    ok(note_mismatches(doc('<note><rest measure="yes"/><duration>960</duration>'
                           '<voice>1</voice><type>whole</type></note>\n')) == [],
       "全小节休止 measure=yes 合法")
    ok(note_mismatches(doc(note(160, "eighth", extra="<time-modification>"
                                 "<actual-notes>3</actual-notes>"
                                 "<normal-notes>2</normal-notes>"
                                 "</time-modification>"))) == [],
       "三连音八分按 2/3 折算")
    ok(len(note_mismatches(doc(note(240, "eighth", extra="<time-modification>"
                                    "<actual-notes>3</actual-notes>"
                                    "<normal-notes>2</normal-notes>"
                                    "</time-modification>")))) == 1,
       "三连音却按普通八分时长应判不一致")
    ok(note_mismatches(doc('<note><grace/><pitch><step>C</step><octave>4</octave>'
                           '</pitch><voice>1</voice><type>eighth</type></note>\n')) == [],
       "倚音没有 duration，跳过")
    ok(len(note_mismatches(doc(note(480, "quarter") + note(840, "half", rest=True)))) == 1,
       "混合音符里只报休止符那一处")

    a = doc(note(480, "quarter") + note(840, "half", rest=True))
    b = doc(note(480, "quarter") + note(720, "quarter", dot=True, rest=True)
            + note(120, "16th", rest=True))
    ok(strip_rest_lines(a) == strip_rest_lines(b),
       "移去除休止符行后两份 XML 结构一致")
    ok(rest_total(a) == rest_total(b) == 840,
       "休止符拆分前后总时长守恒")
    ok(note_mismatches(b) == [],
       "拆分后的休止符全部精确")
    ok(REST_LINE_RE.sub("", '<note><rest measure="yes"/><duration>960</duration>'
                        '<type>whole</type></note>\nX') == "X",
       "整小节休止行同样会被 strip")
    ok(strip_rest_lines('<note><type>eighth</type>'
                        '<beam number="1">begin</beam></note>\nX')
       == '<note><type>eighth</type></note>\nX',
       "<beam> 也会被剥掉，连梁不参与零回归比对")
    ok(note_mismatches(doc(note(240, "eighth"), div=480)) == [],
       "显式 divisions=480")
    print("check_xml_durations self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 1 if failed else 0


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    if "--regress" in args:
        idx = args.index("--regress")
        if idx + 1 >= len(args):
            print("用法: check_xml_durations.py --regress <旧版exe>")
            return 2
        changed, errors, notes = structural_regress(args[idx + 1])
        print("结构性零回归：%d 份素材的 XML 按预期变化（仅休止符拆分与连梁）" % len(changed))
        for n in notes:
            print("  [NOTE]", n)
        for e in errors:
            print("  [FAIL]", e)
        return 1 if errors else 0

    exe = default_exe()
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad = audit_fixtures(exe)
    if bad:
        print("MusicXML 时值不一致：%d 处" % len(bad))
        for name, row in bad[:20]:
            print("  %s duration=%d beats=%s type=%s dot=%s nominal=%s" %
                  (name, row[0], row[1], row[2], row[3], row[4]))
        return 1
    print("MusicXML 时值一致性：全部 fixture 0 处不一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())

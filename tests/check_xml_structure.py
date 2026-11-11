#!/usr/bin/env python3
r"""MusicXML / HTML 结构性总校验（v1.69.0）。

前面几个 check_* 各管一个维度：时值（单个 <note> 的 <duration> 与
<type>/<dot/> 一致）、连梁（<beam> 闭合与层级）、拍数（文本谱）、附点、
休止、三端轨数。但没人检查「一个小节里各声部的时长加起来是不是正好
等于拍号」——多声部 <backup>、和弦 <chord/>、三连音 <time-modification>、
跨小节 <tie>、变拍号任一处理错，都会让 MuseScore 拒绝打开或把后半小节
挤到下一小节，而上面那些逐音符 / 逐 token 的守卫全都抓不到。

本脚本补上这一维，并把「严格 XML 解析」「子元素顺序」「HTML 标签配对」
一起纳入——这三样以前只靠正则抽查，从没用真正的解析器验过：

  1. 严格 XML 解析（非法字符 / 未闭合标签直接 FAIL）；
  2. 逐小节、逐时间线（按 <backup> 切）时值求和 == 该小节拍号长度；
     <chord/> 成员不占时长、<grace/> 无 duration、拍号跨小节继承、
     弱起 implicit="yes" 豁免但仍要求时长 > 0 且小于整小节；
  3. <note> / <direction> / <attributes> 子元素顺序符合 MusicXML 4.0；
  4. HTML 标签配对（<span> 深度嵌套的 bgrp 尤其容易漏闭合）；
  5. 覆盖硬约束：素材库里必须有 backup / chord / 三连音 / 变拍号 /
     跨小节 tie / 大谱表样本，以及 xml_structure.mid 本身。

用法：
  python tests/check_xml_structure.py                 # 校验全部 fixture
  python tests/check_xml_structure.py --self-test    # 纯逻辑自测
  python tests/check_xml_structure.py --regress OLD  # 对旧版做零回归
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from html.parser import HTMLParser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# MusicXML 4.0 的 content model：同一元素可重复，但下标必须非递减。
NOTE_ORDER = ("chord", "grace", "cue", "pitch", "rest", "unpitched",
              "duration", "tie", "voice", "type", "dot", "accidental",
              "time-modification", "stem", "notehead", "staff", "beam",
              "notations", "lyric", "play")
DIR_ORDER = ("direction-type", "offset", "voice", "staff", "sound")
ATTR_ORDER = ("editorial", "divisions", "key", "time", "staves",
              "part-symbol", "instruments", "clef", "staff-details",
              "transpose", "directive", "measure-style")
ORDERS = {"note": NOTE_ORDER, "direction": DIR_ORDER,
          "attributes": ATTR_ORDER}
FOOTER_RE = re.compile(r"midi2jianpu\s+v[\d.]+")
FOOTER_RE_B = re.compile(rb"midi2jianpu\s+v[\d.]+")


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "midi2jianpu")


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def render(exe, mid, td, tag):
    """一次跑出三端，返回 (txt, html, xml)；页脚版本已归一化的另算。"""
    stem = os.path.basename(mid)
    out = {}
    for flag, ext in (("--output", "txt"), ("--html", "html"),
                      ("--musicxml", "xml")):
        p = os.path.join(td, tag + "_" + stem + "." + ext)
        if os.path.exists(p):
            os.remove(p)
        subprocess.run([exe, mid, flag, p], capture_output=True, timeout=120)
        out[ext] = open(p, "rb").read() if os.path.exists(p) else b""
    return out["txt"], out["html"], out["xml"]


def normalize_footer(raw):
    if isinstance(raw, bytes):
        return FOOTER_RE_B.sub(b"midi2jianpu vX", raw)
    return FOOTER_RE.sub("midi2jianpu vX", raw)


# ---------------------------------------------------------------------------
# XML 结构
# ---------------------------------------------------------------------------
def order_violations(elem, rank, label):
    out, prev, prev_name = [], -1, ""
    for ch in elem:
        t = ch.tag.split("}")[-1]
        if t not in rank:
            out.append("%s: 未知子元素 <%s>" % (label, t))
            continue
        r = rank.index(t)
        if r < prev:
            out.append("%s: <%s> 排在 <%s> 之后" % (label, t, prev_name))
        else:
            prev, prev_name = r, t
    return out


def measure_errors(meas, div, sig, where):
    """返回 (问题列表, 期望长度, 各时间线长度)。sig = (beats, beat-type)。"""
    out = []
    beats, bt = sig
    tm = meas.find("./attributes/time")
    if tm is None:
        tm = meas.find("time")
    if tm is not None and tm.find("beats") is not None:
        beats = int(tm.find("beats").text)
        bt = int(tm.find("beat-type").text)
    denom = bt if bt > 0 else 4
    want = div * 4 // denom * beats
    lines, acc = [], 0
    for node in list(meas):
        if node.tag == "backup":
            lines.append(acc)
            acc = 0
        elif node.tag == "note":
            if node.find("chord") is not None:
                continue                 # 和弦成员与主音共享时间线
            du = node.find("duration")
            if du is not None and du.text:
                acc += int(du.text)
    lines.append(acc)
    implicit = meas.get("implicit") == "yes"
    if implicit:
        if not lines or lines[0] <= 0 or lines[0] >= want:
            out.append("%s: 弱起小节时长 %s 不是 0 ~ %d 之间的正数"
                       % (where, lines, want))
    elif any(x != want for x in lines):
        out.append("%s: 逐声部时值 %s != 拍号长度 %d（%d/%d）"
                   % (where, lines, want, beats, bt))
    return out, want, lines


def structure_errors(xml):
    """严格解析 + 逐小节求和 + 子元素顺序；返回问题列表。"""
    if isinstance(xml, bytes):
        raw = xml
    else:
        raw = xml.encode("utf-8")
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as e:
        return ["MusicXML 严格解析失败: %s" % e]
    out = []
    de = root.find(".//divisions")
    div = int(de.text) if de is not None and de.text else 480
    for pi, part in enumerate(root.findall("part"), 1):
        sig = (4, 4)
        for mi, meas in enumerate(part.findall("measure"), 1):
            where = "P%d 第%d小节" % (pi, mi)
            errs, _, _ = measure_errors(meas, div, sig, where)
            out += errs
            tm = meas.find("./attributes/time")
            if tm is None:
                tm = meas.find("time")
            if tm is not None and tm.find("beats") is not None:
                sig = (int(tm.find("beats").text),
                       int(tm.find("beat-type").text))
            for tag, rank in ORDERS.items():
                for ei, e in enumerate(meas.findall(tag), 1):
                    out += order_violations(e, rank, "%s %s%d" % (where, tag, ei))
    return out


# ---------------------------------------------------------------------------
# HTML 标签配对
# ---------------------------------------------------------------------------
VOID = {"br", "hr", "img", "input", "meta", "link", "area", "base", "col",
        "embed", "param", "source", "track", "wbr"}


class _PairChecker(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.bad = []

    def handle_starttag(self, tag, attrs):
        if tag not in VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.bad.append("</%s> 与栈顶 <%s> 不配对"
                            % (tag, self.stack[-1] if self.stack else "空"))
        else:
            self.stack.pop()


def html_errors(html):
    text = html.decode("utf-8", "replace") if isinstance(html, bytes) else html
    c = _PairChecker()
    try:
        c.feed(text)
    except Exception as e:
        return ["HTML 解析异常: %s" % e]
    if c.stack:
        c.bad.append("未闭合标签: " + ",".join(c.stack[-5:]))
    return c.bad


# ---------------------------------------------------------------------------
# 覆盖硬约束
# ---------------------------------------------------------------------------
def coverage_stats(exe):
    stats = dict(fixtures=0, backup_files=0, chord_files=0, triplet_files=0,
                 meter_files=0, tie_files=0, grand_files=0, implicit_files=0)
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            _, _, xml = render(exe, mid, td, "cov")
            if not xml:
                continue
            stats["fixtures"] += 1
            text = xml.decode("utf-8", "replace")
            if "<backup>" in text:
                stats["backup_files"] += 1
            if "<chord/>" in text:
                stats["chord_files"] += 1
            if "<time-modification>" in text:
                stats["triplet_files"] += 1
            if "<tie " in text:
                stats["tie_files"] += 1
            if "<staves>2</staves>" in text:
                stats["grand_files"] += 1
            if 'implicit="yes"' in text:
                stats["implicit_files"] += 1
            times = set(re.findall(r"<time><beats>(\d+)</beats>"
                                   r"<beat-type>(\d+)</beat-type></time>", text))
            if len(times) >= 2:
                stats["meter_files"] += 1
    stats["has_fixture"] = os.path.exists(os.path.join(FIX, "xml_structure.mid"))
    return stats


def coverage_errors(stats):
    out = []
    if not stats.get("has_fixture"):
        out.append("xml_structure.mid 缺失：结构压力样本没进素材库")
    for key, word in (("backup_files", "<backup> 多声部"),
                      ("chord_files", "<chord/> 和弦"),
                      ("triplet_files", "三连音"),
                      ("meter_files", "变拍号"),
                      ("tie_files", "跨小节 <tie>"),
                      ("grand_files", "大谱表"),
                      ("implicit_files", "弱起小节")):
        if stats.get(key, 0) <= 0:
            out.append("没有%s素材：时值守恒的覆盖不完整" % word)
    return out


def audit_fixtures(exe):
    bad = []
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            name = os.path.basename(mid)
            _, html, xml = render(exe, mid, td, "chk")
            if not xml:
                bad.append(name + ": 未生成 MusicXML")
                continue
            for e in structure_errors(xml):
                bad.append(name + ": " + e)
            for e in html_errors(html):
                bad.append(name + " HTML: " + e)
    stats = coverage_stats(exe)
    for e in coverage_errors(stats):
        bad.append("[覆盖] " + e)
    return bad, stats


def structural_regress(old_exe, new_exe):
    """本轮不动功能代码：三端归一化页脚后应与旧版逐字节一致。"""
    errors, changed = [], []
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            name = os.path.basename(mid)
            o = render(old_exe, mid, td, "old")
            n = render(new_exe, mid, td, "new")
            for i, ext in enumerate(("txt", "html", "xml")):
                a, b = o[i], n[i]
                if ext != "xml":
                    a, b = normalize_footer(a), normalize_footer(b)
                if a != b:
                    changed.append(name + "." + ext)
                    errors.append("%s: %s 与旧版不一致" % (name, ext))
    return changed, errors


# ---------------------------------------------------------------------------
# 纯逻辑自测
# ---------------------------------------------------------------------------
def doc(body, attrs=None, number=1, implicit=False, extra_part=""):
    a = attrs if attrs is not None else (
        "<attributes><divisions>480</divisions>"
        "<time><beats>4</beats><beat-type>4</beat-type></time></attributes>")
    imp = ' implicit="yes"' if implicit else ""
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<score-partwise version="3.1"><part id="P1">'
            '<measure number="%d"%s>%s%s</measure>%s</part></score-partwise>'
            % (number, imp, a, body, extra_part))


def note(dur=480, chord=False, grace=False, extra=""):
    s = "<note>"
    if chord:
        s += "<chord/>"
    if grace:
        s += "<grace/>"
    else:
        s += ("<pitch><step>C</step><octave>4</octave></pitch>"
              "<duration>%d</duration>" % dur)
    return s + "<voice>1</voice><type>quarter</type>" + extra + "</note>"


def self_test():
    n, failed = 0, []

    def ok(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    four = note(480) * 4
    # 1~3 严格 XML 解析
    ok(structure_errors(doc(four)) == [], "4 个四分音符的 4/4 小节合法")
    ok(any("解析失败" in x for x in structure_errors(
        '<?xml version="1.0"?><score-partwise><part><measure>'
        '<note></part>')), "未闭合标签被严格解析抓到")
    ok(any("解析失败" in x for x in structure_errors(
        doc(four).replace("C</step>", "C\x01</step>"))),
       "XML 1.0 非法控制字符被抓到")

    # 4~8 逐声部时值守恒
    ok(any("逐声部时值" in x for x in structure_errors(doc(note(480) * 3))),
       "少一个四分音符被抓到")
    ok(structure_errors(doc(four + note(480, chord=True))) == [],
       "<chord/> 成员不重复占用时间线")
    ok(structure_errors(doc(four + "<backup><duration>1920</duration></backup>"
                            + four)) == [],
       "两条 backup 时间线各自守恒")
    ok(any("逐声部时值" in x for x in structure_errors(
        doc(four + "<backup><duration>1920</duration></backup>"
            + note(480) * 3))),
       "backup 后少一拍被抓到")
    ok(structure_errors(doc(four + note(0, grace=True))) == [],
       "<grace/> 无 duration 不参与求和")

    # 9~13 拍号继承与弱起
    sig68 = ("<attributes><divisions>480</divisions>"
             "<time><beats>6</beats><beat-type>8</beat-type></time></attributes>")
    second = '<measure number="2">' + note(240) * 6 + '</measure>'
    ok(structure_errors(doc(note(240) * 6, attrs=sig68,
                            extra_part=second)) == [],
       "第 2 小节无 <time> 时继承 6/8 的 1440 tick")
    ok(structure_errors(doc(note(240) * 6, attrs=sig68)) == [],
       "6/8 下 6 个八分恰好 1440 tick")
    ok(any("逐声部时值" in x for x in structure_errors(doc(note(240) * 6))),
       "同样 6 个八分在 4/4 下不守恒被抓到")
    ok(structure_errors(doc(note(480) * 2, implicit=True)) == [],
       "弱起 implicit=yes 且不完整时豁免")
    ok(any("弱起小节时长" in x for x in structure_errors(
        doc("", implicit=True))),
       "弱起小节时长为 0 被抓到")

    # 14~18 子元素顺序
    ok(structure_errors(doc(note(480) * 3 + note(
        480, extra="<staff>1</staff>"
        '<beam number="1">begin</beam>'))) == [],
       "note 里 staff -> beam 顺序合法")
    ok(any("排在" in x for x in structure_errors(doc(
        note(480) * 3 + note(480, extra='<beam number="1">begin</beam>'
                             '<staff>1</staff>')))),
       "note 里 beam 排在 staff 之前被抓到")
    good_dir = ('<direction placement="above"><direction-type>'
                '<dynamics><p/></dynamics></direction-type>'
                '<voice>1</voice><staff>1</staff><sound dynamics="45"/>'
                '</direction>')
    ok(structure_errors(doc(four + good_dir)) == [],
       "direction 子元素顺序合法")
    bad_dir = ('<direction placement="above"><direction-type>'
               '<dynamics><p/></dynamics></direction-type>'
               '<voice>1</voice><sound dynamics="45"/>'
               '<staff>1</staff></direction>')
    ok(any("排在" in x for x in structure_errors(doc(four + bad_dir))),
       "direction 里 staff 排在 sound 之后被抓到")
    ok(any("排在" in x for x in structure_errors(doc(
        four, attrs="<attributes><time><beats>4</beats>"
        "<beat-type>4</beat-type></time><divisions>480</divisions>"
        "</attributes>"))),
       "attributes 里 divisions 排在 time 之后被抓到")

    # 17~20 HTML 标签配对
    ok(html_errors("<div><span class='x'>a</span></div>") == [],
       "正常嵌套 HTML 合法")
    ok(html_errors("<div><span>a</div>") != [], "未闭合 <span> 被抓到")
    ok(html_errors("<div><span>a</span></span></div>") != [],
       "多余闭合标签被抓到")
    ok(html_errors("<div>a<br>b</div>") == [], "void 标签 <br> 不计入配对")

    # 21~28 覆盖硬约束
    good = dict(has_fixture=True, backup_files=3, chord_files=4,
                triplet_files=2, meter_files=2, tie_files=3,
                grand_files=2, implicit_files=1)
    ok(coverage_errors(good) == [], "覆盖齐全时不报错")
    for key in ("has_fixture", "backup_files", "chord_files",
                "triplet_files", "meter_files", "tie_files",
                "grand_files", "implicit_files"):
        missing = dict(good)
        missing[key] = False if key == "has_fixture" else 0
        ok(any("覆盖" in x or "缺失" in x for x in coverage_errors(missing)),
           "缺 %s 被覆盖硬约束抓到" % key)

    # 29~31 页脚归一化
    ok(normalize_footer(b"midi2jianpu v1.68.0") == b"midi2jianpu vX",
       "bytes 页脚版本归一化")
    ok(normalize_footer("midi2jianpu v1.69.0") == "midi2jianpu vX",
       "str 页脚版本归一化")
    ok(normalize_footer(b"other") == b"other", "无页脚时原样返回")

    print("check_xml_structure self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 1 if failed else 0


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if "--regress" in args:
        i = args.index("--regress")
        if i + 1 >= len(args):
            print("用法: check_xml_structure.py --regress <旧版exe>")
            return 2
        changed, errors = structural_regress(args[i + 1], exe)
        print("结构性零回归：%d 份素材，变化 %d 个产物"
              % (len(fixtures()), len(changed)))
        for e in errors[:40]:
            print("  [FAIL]", e)
        return 1 if errors else 0
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad, stats = audit_fixtures(exe)
    if bad:
        print("结构审计问题：%d 处" % len(bad))
        for x in bad[:40]:
            print("  [FAIL]", x)
        return 1
    print("结构审计通过：%d 份素材严格解析 / 逐声部时值守恒 / 元素顺序 / "
          "HTML 标签配对全部 0 问题" % stats["fixtures"])
    print("覆盖：backup %d / chord %d / 三连音 %d / 变拍号 %d / tie %d / "
          "大谱表 %d / 弱起 %d 份"
          % (stats["backup_files"], stats["chord_files"],
             stats["triplet_files"], stats["meter_files"],
             stats["tie_files"], stats["grand_files"],
             stats["implicit_files"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

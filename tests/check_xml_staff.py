#!/usr/bin/env python3
r"""MusicXML 大谱表方向记号的谱表归属校验（v1.68.0）。

大谱表（<staves>2</staves>）里，音符按每颗音的音高分流到上下两行谱，
而力度 / 渐变 / 速度这类 <direction> 必须标 <staff> 才会显示在正确那行。
旧实现给整个声部只算一次「全曲多数音高侧」：钢琴曲里同一声部整小节
移到另一行谱（左手独奏段、右手华彩段）时，力度记号会被印到完全没有
音符的空白谱行，真正发声的谱表反而看不到记号。

校验维度：

  1. 带 <staff> 的 <direction>，其 staff 必须属于「同小节、同 <voice>」
     的音符谱表集合；direction 不带 <voice> 时按该小节全部音符算。
  2. <direction> 子元素顺序：direction-type+ -> offset? -> voice?
     -> staff? -> sound?，顺序错了严格校验的软件会丢弃整条方向记号。
  3. 覆盖硬约束：素材库里必须有大谱表样本、跨小节换谱表样本、
     staff=2 的方向记号、带 <offset> 且带 <staff> 的方向记号，以及
     staff_switch.mid 本身。只防新增不防删除的 CI 拦不住素材被删。
  4. --regress：剥掉 <direction> 块里的 <staff> 行后，新旧产物逐字节
     一致。<note> 里的 <staff> 是大谱表游标语义，不在剥离范围内。

用法：
  python tests/check_xml_staff.py                 # 校验全部 fixture
  python tests/check_xml_staff.py --self-test    # 纯逻辑自测
  python tests/check_xml_staff.py --regress OLD  # 对旧版做结构性零回归
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xml_norm import strip_direction_staff  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

MEASURE_RE = re.compile(r"<measure\b[^>]*>(.*?)</measure>", re.S)
NOTE_RE = re.compile(r"<note>(.*?)</note>", re.S)
DIRECTION_RE = re.compile(r"<direction\b[^>]*>(.*?)</direction>", re.S)
STAFF_RE = re.compile(r"<staff>(\d+)</staff>")
VOICE_RE = re.compile(r"<voice>(\d+)</voice>")
OFFSET_RE = re.compile(r"<offset>(\d+)</offset>")
STAVES_RE = re.compile(r"<staves>(\d+)</staves>")

# MusicXML 4.0 <direction> 的固定子元素顺序。
DIRECTION_ORDER = ("direction-type", "offset", "voice", "staff", "sound")


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "midi2jianpu")


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def run_xml(exe, mid, td, tag):
    out = os.path.join(td, tag + "_" + os.path.basename(mid) + ".xml")
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([exe, mid, "--musicxml", out],
                   capture_output=True, timeout=120)
    if not os.path.exists(out):
        return None
    return open(out, encoding="utf-8", errors="replace").read()


def measure_stats(body):
    """返回 (按 voice 的谱表集合, 全小节谱表集合)。

    休止符也带 <staff>（大谱表空小节上下两行各一条全小节休止），
    必须一并计入，否则空小节的方向记号会被误判成「落在空白谱表」。
    """
    voices = {}
    allst = set()
    for n in NOTE_RE.findall(body):
        sm = STAFF_RE.search(n)
        if not sm:
            continue
        vm = VOICE_RE.search(n)
        v = vm.group(1) if vm else "1"
        voices.setdefault(v, set()).add(sm.group(1))
        allst.add(sm.group(1))
    return voices, allst


def direction_violations(xml):
    """逐小节检查方向记号的谱表归属与子元素顺序。"""
    out = []
    for mi, m in enumerate(MEASURE_RE.finditer(xml), 1):
        body = m.group(1)
        voices, allst = measure_stats(body)
        for d in DIRECTION_RE.finditer(body):
            inner = d.group(1)
            # 子元素顺序：direction-type 必须打头，其余按固定顺序递增。
            seq = [t for t in DIRECTION_ORDER if inner.find("<" + t) >= 0]
            if not seq or seq[0] != "direction-type":
                out.append("第 %d 小节 <direction> 缺 direction-type 或它不是首元素"
                           % mi)
            pos = [inner.find("<" + t) for t in seq]
            if any(pos[i] > pos[i + 1] for i in range(len(pos) - 1)):
                out.append("第 %d 小节 <direction> 子元素顺序错: %s"
                           % (mi, " -> ".join(seq)))
            sm = STAFF_RE.search(inner)
            if not sm:
                continue          # 速度 / 段落名等全局记号不带 <staff>
            if "<wedge" in inner:
                # 渐强渐弱楔形是一条跨小节（可能跨谱表）的 hairpin：起止必须
                # 落同一谱表（以起点谱表为锚），stop 所在小节的音符可能是另一
                # 行谱，逐小节归属检查在此不适用，改由 wedge_same_staff 保证
                # 「起止同 staff」。
                continue
            vm = VOICE_RE.search(inner)
            allowed = voices.get(vm.group(1), allst) if vm else allst
            if sm.group(1) not in allowed:
                detail = ",".join(sorted(allowed)) if allowed else "无音符"
                out.append("第 %d 小节 <direction> 标 staff=%s，但该声部该小节"
                           "只有 %s" % (mi, sm.group(1), detail))
    return out


PART_RE = re.compile(r"<part\b[^>]*>(.*?)</part>", re.S)


def wedge_pairs(xml):
    """逐 part 抽取楔形起止的 (start_staff, stop_staff, number) 列表。

    楔形是跨小节（可能跨谱表）的一条 hairpin：crescendo / diminuendo 是
    起点，type="stop" 是终点，二者用同一个 number 配对；配对口径与
    MusicXML 导出层一致（按 part 内的 number 匹配）。
    """
    out = []
    for part in PART_RE.finditer(xml):
        body = part.group(1)
        starts = {}            # number -> start staff
        for d in DIRECTION_RE.finditer(body):
            inner = d.group(1)
            wm = re.search(r"<wedge type=\"(\w+)\" number=\"(\d+)\"", inner)
            if not wm:
                continue
            num = wm.group(2)
            sm = STAFF_RE.search(inner)
            staff = sm.group(1) if sm else None
            if wm.group(1) == "stop":
                out.append((starts.get(num), staff, num))
            else:
                starts[num] = staff
    return out


def wedge_same_staff(xml):
    """一条楔形起止必须落同一谱表（v1.70.0 不变量）。"""
    out = []
    for start_staff, stop_staff, num in wedge_pairs(xml):
        if start_staff is None or stop_staff is None:
            continue
        if start_staff != stop_staff:
            out.append("楔形 number=%s 起 staff=%s、止 staff=%s："
                       "跨谱表断裂（起止必须落同一谱表）"
                       % (num, start_staff, stop_staff))
    return out


def staff_sets_by_measure(xml):
    """每个非空小节的音符谱表集合序列（用于识别换谱表素材）。"""
    sets = []
    for m in MEASURE_RE.finditer(xml):
        _, allst = measure_stats(m.group(1))
        if allst:
            sets.append(frozenset(allst))
    return sets


def has_switch(xml):
    """同一 part 的小节之间，音符所在谱表集合是否变过。"""
    return len(set(staff_sets_by_measure(xml))) >= 2


def coverage_stats(exe):
    stats = dict(grand_files=0, switch_files=0, directions=0,
                 staff2_dirs=0, offset_dirs=0, wedge_dirs=0)
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            xml = run_xml(exe, mid, td, "audit")
            if xml is None:
                continue
            if int((STAVES_RE.search(xml).group(1)
                    if STAVES_RE.search(xml) else "1")) >= 2:
                stats["grand_files"] += 1
            if has_switch(xml):
                stats["switch_files"] += 1
            for d in DIRECTION_RE.finditer(xml):
                inner = d.group(1)
                sm = STAFF_RE.search(inner)
                if not sm:
                    continue
                stats["directions"] += 1
                if sm.group(1) == "2":
                    stats["staff2_dirs"] += 1
                if OFFSET_RE.search(inner):
                    stats["offset_dirs"] += 1
                if "<wedge" in inner:
                    stats["wedge_dirs"] += 1
    stats["has_fixture"] = os.path.exists(os.path.join(FIX, "staff_switch.mid"))
    stats["has_wedge_fixture"] = os.path.exists(
        os.path.join(FIX, "grand_staff_wedge.mid"))
    return stats


def coverage_errors(stats):
    out = []
    if not stats.get("has_fixture"):
        out.append("staff_switch.mid 缺失：大谱表换谱表压力样本没进素材库")
    if not stats.get("has_wedge_fixture"):
        out.append("grand_staff_wedge.mid 缺失：跨谱表楔形压力样本没进素材库")
    if stats.get("grand_files", 0) <= 0:
        out.append("没有任何 <staves>2</staves> 素材：大谱表维度没被真实覆盖")
    if stats.get("switch_files", 0) <= 0:
        out.append("没有跨小节换谱表的素材：direction 用全曲多数侧的缺陷会被漏过")
    if stats.get("staff2_dirs", 0) <= 0:
        out.append("没有 staff=2 的 <direction>：下行谱方向记号覆盖丢失")
    if stats.get("offset_dirs", 0) <= 0:
        out.append("没有带 <offset> 且带 <staff> 的 <direction>："
                   "中途换档按拍位定位的覆盖丢失")
    if stats.get("wedge_dirs", 0) <= 0:
        out.append("没有任何 wedge 素材：渐强渐弱楔形覆盖丢失")
    return out


def audit_fixtures(exe):
    bad = []
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            name = os.path.basename(mid)
            xml = run_xml(exe, mid, td, "check")
            if xml is None:
                bad.append(name + ": 生成 MusicXML 失败")
                continue
            for v in direction_violations(xml):
                bad.append(name + ": " + v)
            for v in wedge_same_staff(xml):
                bad.append(name + ": " + v)
    stats = coverage_stats(exe)
    for e in coverage_errors(stats):
        bad.append("[覆盖] " + e)
    return bad, stats


def structural_regress(old_exe, new_exe):
    """跨版本零回归：剥掉 direction 的 <staff> 行后必须逐字节一致。"""
    errors, notes, changed = [], [], 0
    delta = 0
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            name = os.path.basename(mid)
            old_xml = run_xml(old_exe, mid, td, "old")
            new_xml = run_xml(new_exe, mid, td, "new")
            if old_xml is None or new_xml is None:
                errors.append(name + ": 生成 MusicXML 失败")
                continue
            if old_xml == new_xml:
                continue
            changed += 1
            if strip_direction_staff(old_xml) != strip_direction_staff(new_xml):
                errors.append(name + ": 除 <direction> 的 <staff> 之外还有别的差异")
            def staffs(x):
                return [m.group(1) for d in DIRECTION_RE.finditer(x)
                        for m in [STAFF_RE.search(d.group(1))] if m]
            o, n = staffs(old_xml), staffs(new_xml)
            delta += sum(1 for a, b in zip(o, n) if a != b)
    notes.append("方向记号 <staff> 取值变化 %d 处，涉及 %d 份素材" % (delta, changed))
    return changed, errors, notes


# ---------------------------------------------------------------------------
# 纯逻辑自测
# ---------------------------------------------------------------------------
def doc(*bodies):
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<score-partwise version="3.1"><part id="P1">'
            '<measure number="1"><staves>2</staves>' + "".join(bodies)
            + '</measure></part></score-partwise>')


def note(staff=None, voice="1", rest=False):
    s = "<note>"
    s += "<rest/>" if rest else "<pitch><step>C</step><octave>4</octave></pitch>"
    s += "<duration>480</duration><voice>%s</voice><type>quarter</type>" % voice
    if staff is not None:
        s += "<staff>%d</staff>" % staff
    return s + "</note>"


def direction(staff=None, voice=None, offset=None, order=None, typ="dyn"):
    parts = ["<direction-type>" +
             ("<dynamics><p/></dynamics>" if typ == "dyn" else "<words>Verse</words>")
             + "</direction-type>"]
    if offset is not None:
        parts.append("<offset>%d</offset>" % offset)
    if voice is not None:
        parts.append("<voice>%d</voice>" % voice)
    if staff is not None:
        parts.append("<staff>%d</staff>" % staff)
    parts.append('<sound dynamics="45"/>')
    if order is not None:
        parts = [p for key in order for p in parts if "<" + key in p]
    return '<direction placement="above">' + "".join(parts) + "</direction>"


def self_test():
    n, failed = 0, []

    def ok(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    # 1~3 单声部：方向记号必须跟音符在同一行谱
    ok(direction_violations(doc(direction(staff=1), note(staff=1))) == [],
       "单声部上一行谱的记号合法")
    ok(any("没有音符" in x or "只有 2" in x for x in
           direction_violations(doc(direction(staff=1), note(staff=2)))),
       "方向记号落在只有低音谱音符的小节被抓到")
    ok(direction_violations(doc(direction(staff=2), note(staff=2))) == [],
       "低音谱小节标 staff=2 合法")

    # 4~6 全局记号 / 多声部按 voice 归属
    ok(direction_violations(doc(direction(), note(staff=2))) == [],
       "不带 <staff> 的全局记号不参与归属检查")
    ok(direction_violations(doc(direction(staff=2, voice=2),
                                note(staff=1, voice=1),
                                note(staff=2, voice=2))) == [],
       "带 <voice>2</voice> 的记号按第 2 声部集合判定")
    ok(any("只有 2" in x for x in direction_violations(
           doc(direction(staff=1, voice=2),
               note(staff=1, voice=1), note(staff=2, voice=2)))),
       "带 <voice>2</voice> 却标上行谱被抓到")

    # 7~9 子元素顺序
    ok(any("顺序错" in x for x in direction_violations(
           doc(direction(order=["direction-type", "sound", "staff"], staff=1),
               note(staff=1)))),
       "<staff> 排在 <sound> 之后被抓到")
    ok(any("顺序错" in x for x in direction_violations(
           doc(direction(order=["direction-type", "voice", "offset", "sound"],
                         offset=240, voice=1), note(staff=1)))),
       "<offset> 排在 <voice> 之后被抓到")
    ok(any("缺 direction-type" in x for x in direction_violations(
           doc('<direction><sound dynamics="45"/></direction>', note(staff=1)))),
       "缺 direction-type 被抓到")

    # 10~12 休止符也占谱表；小节内跨两谱时任一谱表合法
    ok(direction_violations(doc(direction(staff=2), note(staff=2, rest=True))) == [],
       "全小节休止符也算该谱表有内容")
    ok(direction_violations(doc(direction(staff=2),
                                note(staff=1), note(staff=2))) == [],
       "同小节跨两谱时标 staff=2 合法")
    ok(any("无音符" in x for x in direction_violations(
           doc(direction(staff=1), note(staff=None)))),
       "小节完全不带 <staff> 时方向记号被抓到")

    # 13~15 strip_direction_staff 只删 direction 内的 <staff>
    raw = ('<direction placement="above">\n'
           '  <direction-type><dynamics><p/></dynamics></direction-type>\n'
           '  <staff>1</staff>\n'
           '  <sound dynamics="45"/>\n'
           '</direction>\n'
           '<note><type>quarter</type><staff>2</staff></note>\n')
    st = strip_direction_staff(raw)
    ok("<staff>" not in st.split("</direction>")[0],
       "str 版剥掉 direction 内的 <staff>")
    ok("<staff>2</staff>" in st,
       "str 版保留 <note> 内的 <staff>")
    by = strip_direction_staff(raw.encode("utf-8"))
    ok(b"<staff>" not in by.split(b"</direction>")[0]
       and b"<staff>2</staff>" in by,
       "bytes 版同样只剥 direction 内的 <staff>")

    # 16~18 has_switch 口径
    ok(has_switch('<measure number="1">' + note(staff=1) + '</measure>'
                  '<measure number="2">' + note(staff=2) + '</measure>'),
       "小节间换谱表被识别")
    ok(not has_switch('<measure number="1">' + note(staff=1) + note(staff=2)
                      + '</measure><measure number="2">' + note(staff=1)
                      + note(staff=2) + '</measure>'),
       "每小节都跨两谱不算换谱表")
    ok(not has_switch('<measure number="1">' + note(staff=None)
                      + '</measure><measure number="2">' + note(staff=None)
                      + '</measure>'),
       "没有 <staff> 的非大谱表不算换谱表")

    # 19~24 覆盖硬约束
    good = dict(has_fixture=True, has_wedge_fixture=True, grand_files=2,
                switch_files=1, directions=9, staff2_dirs=4, offset_dirs=1,
                wedge_dirs=2)
    ok(coverage_errors(good) == [], "覆盖齐全时不报错")
    for key, word in (("has_fixture", "staff_switch.mid"),
                      ("has_wedge_fixture", "grand_staff_wedge"),
                      ("grand_files", "staves"),
                      ("switch_files", "换谱表"),
                      ("staff2_dirs", "staff=2"),
                      ("offset_dirs", "offset"),
                      ("wedge_dirs", "wedge")):
        missing = dict(good)
        missing[key] = False if key == "has_fixture" else 0
        ok(any(word in x for x in coverage_errors(missing)),
           "缺 %s 被覆盖硬约束抓到" % key)
    ok(any("方向记号" in x or "只有" in x for x in
           direction_violations(doc(direction(staff=1), note(staff=2)))),
       "负向：旧实现把低音小节记号标到上行谱一定变红")

    # 25~26 楔形豁免逐小节归属：跨谱表的 stop 不被误抓，
    #         非楔形的方向记号仍被逐小节检查抓到
    wedge_stop = ('<direction placement="above"><direction-type>'
                  '<wedge type="stop" number="1"/></direction-type>'
                  '<staff>1</staff></direction>')
    ok(direction_violations(doc(direction(staff=1), note(staff=1), wedge_stop)) == [],
       "楔形 stop 不参与逐小节归属检查（豁免）")
    ok(any("只有 2" in x for x in direction_violations(
           doc(direction(staff=1), note(staff=2)))),
       "非楔形的方向记号仍按逐小节归属抓到")

    # 27~29 wedge_same_staff：起止同 staff 合法、跨谱表断裂被抓、缺 staff 不误报
    w_start = ('<direction placement="above"><direction-type>'
               '<wedge type="crescendo" number="1"/></direction-type>'
               '<staff>1</staff></direction>')
    def wdoc(stop_staff):
        ws = ('<direction placement="above"><direction-type>'
              '<wedge type="stop" number="1"/></direction-type>'
              '<staff>%s</staff></direction>' % stop_staff)
        return ('<?xml version="1.0" encoding="UTF-8"?>'
                '<score-partwise version="3.1"><part id="P1">'
                '<measure number="1"><staves>2</staves>' + note(staff=1)
                + w_start + '</measure>'
                '<measure number="2"><staves>2</staves>' + note(staff=2)
                + ws + '</measure></part></score-partwise>')
    ok(wedge_same_staff(wdoc("1")) == [], "跨谱表楔形起止同 staff 合法")
    ok(any("跨谱表断裂" in x for x in wedge_same_staff(wdoc("2"))),
       "起 staff=1 止 staff=2 的跨谱表楔形被抓到")
    ok(wedge_same_staff('<direction><direction-type><wedge type="crescendo" '
                        'number="1"/></direction-type></direction>') == [],
       "楔形缺 <staff> 不误报（start/stop 缺一不判）")

    print("check_xml_staff self-test: %d 项，%d 失败" % (n, len(failed)))
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
            print("用法: check_xml_staff.py --regress <旧版exe>")
            return 2
        changed, errors, notes = structural_regress(args[i + 1], exe)
        print("结构性零回归：%d 份素材" % len(fixtures()))
        for x in notes:
            print("  [NOTE]", x)
        for e in errors:
            print("  [FAIL]", e)
        return 1 if errors else 0
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad, stats = audit_fixtures(exe)
    if bad:
        print("方向记号谱表归属问题：%d 处" % len(bad))
        for x in bad[:40]:
            print("  [FAIL]", x)
        return 1
    print("方向记号谱表归属全部正确：%d 份素材 / %d 条带 <staff> 的 <direction>"
          "（staff=2 共 %d 条、带 <offset> %d 条、wedge %d 条、大谱表 %d 份、"
          "换谱表 %d 份）"
          % (len(fixtures()), stats["directions"], stats["staff2_dirs"],
             stats["offset_dirs"], stats["wedge_dirs"],
             stats["grand_files"], stats["switch_files"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

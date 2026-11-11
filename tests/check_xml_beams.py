#!/usr/bin/env python3
r"""MusicXML 连梁（<beam>）结构校验（v1.64.0，v1.67.0 补钩形梁）。

MusicXML 不写 <beam> 时，MuseScore 只能按自己的规则自动连梁；遇到混合时值、
跨拍连音、三连音就会连错。写了 <beam> 就必须守规矩，否则比不写更糟：

  1. 每组梁按 begin -> continue* -> end 闭合，不许悬挂 begin、
     不许孤立 continue/end（`continue` 少了配对的 begin，某些软件会整条丢）；
  2. 带 <beam> 的音符时值必须短于四分（quarter/half/whole 不该有梁）；
  3. 子元素顺序：<type>/<dot>/<time-modification> -> <staff> -> <beam>
     -> <notations>，顺序错了严格校验的软件会丢弃整条音符；
  4. 和弦成员（<chord/>）必须与主音带同一组梁，否则一颗音画得出梁、
     另一颗画不出；
  5. 参与连梁的短音符层级必须完整：16th 要有第 2 层、32nd 要有第 3 层……
     孤立短音符用 forward/backward hook 补齐，缺一层就会把十六分显示成
     八分（HTML 端却照常画两条减时线）。

梁的边界口径与 HTML 端减时线一致：同小节、同谱表、同声部、首尾相接、
同一拍（三连音例外）。hook 值含空格，剥梁正则必须用 `[^<]+`，
写成 `\w+` 会漏掉钩形梁，跨版本零回归会假报 DIFF。

用法：
  python tests/check_xml_beams.py                 # 校验全部 fixture
  python tests/check_xml_beams.py --self-test    # 纯逻辑自测
  python tests/check_xml_beams.py --regress OLD  # 对旧版做结构性零回归
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

NOTE_RE = re.compile(r"<note>(.*?)</note>", re.S)
MEASURE_RE = re.compile(r"<measure\b(.*?)</measure>", re.S)
BEAM_RE = re.compile(r'<beam number="(\d)">([^<]+)</beam>')
BEAM_ANY_RE = re.compile(r'<beam number="\d">[^<]+</beam>')
TYPE_RE = re.compile(r"<type>([^<]+)</type>")
STAFF_RE = re.compile(r"<staff>(\d+)</staff>")
VOICE_RE = re.compile(r"<voice>(\d+)</voice>")
DIV_RE = re.compile(r"<divisions>(\d+)</divisions>")

FLAT_TYPES = {"quarter", "half", "whole", "breve", "long"}
BEAMABLE = {"eighth", "16th", "32nd", "64th"}
HOOKS = {"forward hook", "backward hook"}
# 参与一级梁的短音符必须逐层补齐：16th 要 2 层、32nd 要 3 层、64th 要 4 层。
# 少一层，记谱软件就把十六分显示成八分（HTML 端却按自身时值画两条减时线）。
REQUIRED_LEVEL = {"eighth": 1, "16th": 2, "32nd": 3, "64th": 4}


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "midi2jianpu")


def strip_beams(xml):
    """去掉全部 <beam> 元素：新旧两版除梁以外必须逐字节一致。"""
    return BEAM_ANY_RE.sub("", xml)


def note_info(body):
    tm = TYPE_RE.search(body)
    sm = STAFF_RE.search(body)
    vm = VOICE_RE.search(body)
    return {
        "type": tm.group(1) if tm else "",
        "staff": sm.group(1) if sm else "1",
        "voice": vm.group(1) if vm else "1",
        "beams": {int(k): v for k, v in BEAM_RE.findall(body)},
        "chord": "<chord/>" in body,
        "grace": "<grace/>" in body,
        "rest": "<rest" in body,
    }


def beam_violations(xml):
    """返回违规描述列表（空表示全部合法）。"""
    out = []
    for mi, m in enumerate(MEASURE_RE.finditer(xml)):
        bar = mi + 1
        notes = NOTE_RE.findall(m.group(1))
        groups = {}
        prev_beams = None          # 上一颗非倚音音符的梁，用于和弦一致性
        for body in notes:
            info = note_info(body)
            if info["grace"]:
                continue           # 倚音不参与连梁
            # ---- 单音符自身：时值 + 子元素顺序 ----
            if info["beams"]:
                if info["type"] in FLAT_TYPES:
                    out.append("第 %d 小节 %s 音符带了 <beam>"
                               % (bar, info["type"]))
                elif info["type"] not in BEAMABLE:
                    out.append("第 %d 小节带 <beam> 的音符缺 <type>" % bar)
                i_beam = body.find("<beam ")
                i_type = body.find("<type>")
                if i_type < 0 or i_type > i_beam:
                    out.append("第 %d 小节 <type> 不在 <beam> 之前" % bar)
                i_staff = body.find("<staff>")
                if i_staff >= 0 and i_staff > i_beam:
                    out.append("第 %d 小节 <staff> 不在 <beam> 之前" % bar)
                i_not = body.find("<notations>")
                if i_not >= 0 and i_not < i_beam:
                    out.append("第 %d 小节 <beam> 不在 <notations> 之前" % bar)
                i_tm = body.find("<time-modification>")
                if i_tm >= 0 and i_tm > i_beam:
                    out.append("第 %d 小节 <beam> 不在 <time-modification> 之后" % bar)
                i_dot = body.find("<dot/>")
                if i_dot >= 0 and i_dot > i_beam:
                    out.append("第 %d 小节 <beam> 不在 <dot/> 之后" % bar)
            # ---- 层级完整性：参与连梁的短音符不能少画一层 ----
            # 混合时值里孤立的十六分音符（如 16th 8th 16th）既没有
            # 相邻的十六分可连，也不能整层丢掉；必须用钩形梁补齐。
            need = REQUIRED_LEVEL.get(info["type"], 0)
            if need >= 2 and 1 in info["beams"] and not info["rest"]:
                for lv in range(2, need + 1):
                    if lv not in info["beams"]:
                        out.append("第 %d 小节 %s 音符参与连梁却缺第 %d 层 <beam>"
                                   % (bar, info["type"], lv))
            # ---- 和弦成员必须与主音同梁 ----
            if info["chord"] and prev_beams is not None:
                if info["beams"] != prev_beams:
                    out.append("第 %d 小节和弦成员的 <beam> 与主音不一致" % bar)
            prev_beams = info["beams"]
            # 和弦成员与主音是同一个节奏事件，不能再走一遍状态机，
            # 否则「begin」会被当成第二个音头重复计数。
            if info["chord"]:
                continue
            groups.setdefault((info["staff"], info["voice"]), []).append(info)

        # ---- 每组(谱表,声部)逐层走状态机 ----
        for key, seq in sorted(groups.items()):
            for level in range(1, 5):
                state = None       # None: 期待 begin；begin/continue: 期待续接
                for info in seq:
                    word = info["beams"].get(level)
                    if word is None:
                        if state in ("begin", "continue"):
                            out.append("第 %d 小节 谱表%s 声部%s 第 %d 层梁悬挂未闭合"
                                       % (bar, key[0], key[1], level))
                        state = None
                        continue
                    if word == "begin":
                        if state in ("begin", "continue"):
                            out.append("第 %d 小节 谱表%s 声部%s 第 %d 层梁在未闭合时又 begin"
                                       % (bar, key[0], key[1], level))
                        state = "begin"
                    elif word == "continue":
                        if state not in ("begin", "continue"):
                            out.append("第 %d 小节 谱表%s 声部%s 第 %d 层梁出现孤立 continue"
                                       % (bar, key[0], key[1], level))
                        state = "continue"
                    elif word == "end":
                        if state not in ("begin", "continue"):
                            out.append("第 %d 小节 谱表%s 声部%s 第 %d 层梁出现孤立 end"
                                       % (bar, key[0], key[1], level))
                        state = None
                    elif word in HOOKS:
                        # 钩形梁是孤立的短梁，不参与 begin/continue/end 配对；
                        # 但它不能落在本层已开启的梁组中间，否则前面的
                        # begin 会悬挂，记谱软件会丢掉整条音符。
                        if state in ("begin", "continue"):
                            out.append("第 %d 小节 谱表%s 声部%s 第 %d 层钩形梁落在未闭合梁组中"
                                       % (bar, key[0], key[1], level))
                        state = None
                    else:
                        out.append("第 %d 小节第 %d 层梁状态非法: %s" % (bar, level, word))
                if state in ("begin", "continue"):
                    out.append("第 %d 小节 谱表%s 声部%s 第 %d 层梁到小节末仍未闭合"
                               % (bar, key[0], key[1], level))
    return out


def beam_stats(xml):
    """返回 (总梁元素数, 各层计数 dict)。"""
    total = 0
    per = {}
    for lv, _ in BEAM_RE.findall(xml):
        total += 1
        k = int(lv)
        per[k] = per.get(k, 0) + 1
    return total, per


def beam_states(xml):
    """返回各 beam 状态（begin / continue / end）的出现次数。"""
    out = {}
    for _, word in BEAM_RE.findall(xml):
        out[word] = out.get(word, 0) + 1
    return out


def hook_stats(xml):
    """返回 {(层号, 状态): 次数}，只统计钩形梁。"""
    out = {}
    for lv, word in BEAM_RE.findall(xml):
        if word in HOOKS:
            k = (int(lv), word)
            out[k] = out.get(k, 0) + 1
    return out


def coverage_errors(per, states, has_fixture=True, hooks=None,
                    has_hook_fixture=True):
    """真实素材必须覆盖到多级梁与三音以上连梁，否则返回问题描述。

    v1.66.0 之前 88 份素材里一个十六分音符都没有：二级梁（number="2"）
    与 continue 状态只由单测和临时探针覆盖，写错层级、漏掉串中间音这类
    回归不会被 CI 抓到。beam_levels.mid 补上这份覆盖，这里锁成硬约束——
    素材被删、生成器漏跑、或实现退化让十六分不再画二级梁，都会直接变红。
    """
    out = []
    if not has_fixture:
        # CI 只查「没有多出未跟踪素材」，不防有人把既有素材删掉。
        out.append("beam_levels.mid 缺失：连梁压力样本没进素材库（生成器漏跑或被删）")
    if sum(c for level, c in per.items() if level >= 2) <= 0:
        out.append("没有二级及以上 <beam>：十六分音符 / 次级梁的真实素材覆盖丢失")
    if states.get("continue", 0) <= 0:
        out.append("没有 continue 状态：三音以上连梁的真实素材覆盖丢失")
    if hooks is not None:
        # v1.67.0：钩形梁是混合时值下唯一能表达「孤立短音符」的方式，
        # 方向（组首 forward / 其余 backward）与三级梁都要在真实素材里
        # 被审到，只有单测覆盖等于没覆盖。
        def hc(pred):
            return sum(c for (lv, w), c in hooks.items()
                       if c > 0 and pred(lv, w))
        if not has_hook_fixture:
            out.append("beam_hooks.mid 缺失：钩形梁压力样本没进素材库（生成器漏跑或被删）")
        if hc(lambda lv, w: w == "forward hook") <= 0:
            out.append("没有 forward hook：组首孤立短音符的钩形梁覆盖丢失")
        if hc(lambda lv, w: w == "backward hook") <= 0:
            out.append("没有 backward hook：组尾/中间孤立短音符的钩形梁覆盖丢失")
        if hc(lambda lv, w: lv >= 3) <= 0:
            out.append("没有三级及以上钩形梁：三十二分音符的真实素材覆盖丢失")
    return out


def run_xml(exe, mid, outdir):
    out = os.path.join(outdir, "o.xml")
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([exe, mid, "--musicxml", out],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(out):
        return None
    with open(out, encoding="utf-8", errors="replace") as f:
        return f.read()


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def audit_fixtures(exe):
    """全量审计。返回 (违规列表, 有梁的文件数, 梁元素总数,
    层分布, 状态分布, 钩形梁分布)。"""
    bad = []
    with_beam = 0
    total = 0
    per = {}
    states = {}
    hooks = {}
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            xml = run_xml(exe, mid, td)
            if xml is None:
                bad.append((os.path.basename(mid), "没有产出 MusicXML"))
                continue
            name = os.path.basename(mid)
            for v in beam_violations(xml):
                bad.append((name, v))
            n, p = beam_stats(xml)
            if n:
                with_beam += 1
            total += n
            for k, c in p.items():
                per[k] = per.get(k, 0) + c
            for w, c in beam_states(xml).items():
                states[w] = states.get(w, 0) + c
            for k, c in hook_stats(xml).items():
                hooks[k] = hooks.get(k, 0) + c
    return bad, with_beam, total, per, states, hooks


def structural_regress(old_exe):
    """跨版本零回归：剥掉 <beam> 后新旧产物必须逐字节一致。"""
    new_exe = default_exe()
    errors = []
    notes = []
    added = 0
    changed = 0
    with tempfile.TemporaryDirectory() as td:
        for mid in fixtures():
            name = os.path.basename(mid)
            # 两次产物落在不同目录，避免互相覆盖
            o1 = os.path.join(td, "old")
            o2 = os.path.join(td, "new")
            os.makedirs(o1, exist_ok=True)
            os.makedirs(o2, exist_ok=True)
            old_xml = run_xml(old_exe, mid, o1)
            new_xml = run_xml(new_exe, mid, o2)
            if old_xml is None or new_xml is None:
                errors.append("%s: 生成 MusicXML 失败" % name)
                continue
            if (strip_direction_staff(strip_beams(old_xml))
                    != strip_direction_staff(strip_beams(new_xml))):
                errors.append("%s: 除 <beam> 之外还有别的差异" % name)
            no, _ = beam_stats(old_xml)
            nn, _ = beam_stats(new_xml)
            if nn != no:
                changed += 1
                added += nn - no
            bad = beam_violations(new_xml)
            if bad:
                errors.append("%s: 新版梁结构违规 %d 处（%s）"
                              % (name, len(bad), bad[0]))
    notes.append("新增 <beam> 元素 %d 个，涉及 %d 份素材" % (added, changed))
    return changed, added, errors, notes


def doc(*bodies):
    """把若干 <note> 体拼成最小可校验的 score-partwise。"""
    inner = "".join("<note>%s</note>" % b for b in bodies)
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<score-partwise version="3.1"><part id="P1">'
            '<measure number="1">%s</measure></part></score-partwise>' % inner)


def note(dur, tp, beams, staff=None, chord=False, grace=False, notch=False):
    s = "<chord/>" if chord else ""
    if grace:
        s += "<grace/>"
    s += "<pitch><step>C</step><octave>4</octave></pitch>"
    s += "<duration>%d</duration>" % dur
    s += "<voice>1</voice><type>%s</type>" % tp
    if staff is not None:
        s += "<staff>%d</staff>" % staff
    for lv, w in beams:
        s += '<beam number="%d">%s</beam>' % (lv, w)
    if notch:
        s += '<notations><tied type="start"/></notations>'
    return s


def self_test():
    n = 0
    failed = []

    def ok(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    # 1 合法成对
    ok(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                           note(240, "eighth", [(1, "end")]))) == [],
       "begin/end 成对合法")
    # 2 悬挂 begin
    ok(len(beam_violations(doc(note(240, "eighth", [(1, "begin")])))) == 1,
       "悬挂 begin 被抓到")
    # 3 孤立 end
    ok(len(beam_violations(doc(note(240, "eighth", [(1, "end")])))) == 1,
       "孤立 end 被抓到")
    # 4 continue 无 begin
    ok(len(beam_violations(doc(note(240, "eighth", [(1, "continue")]),
                               note(240, "eighth", [(1, "end")])))) >= 1,
       "孤立 continue 被抓到")
    # 5 begin/continue/end
    ok(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                           note(240, "eighth", [(1, "continue")]),
                           note(240, "eighth", [(1, "end")]))) == [],
       "begin/continue/end 合法")
    # 6 四分音符带梁
    ok(len(beam_violations(doc(note(480, "quarter", [(1, "begin")]),
                               note(480, "quarter", [(1, "end")])))) >= 1,
       "四分音符带梁被抓到")
    # 7 staff 在 beam 之后
    bad = ('<pitch><step>C</step><octave>4</octave></pitch><duration>240</duration>'
           '<voice>1</voice><type>eighth</type><beam number="1">begin</beam>'
           '<staff>1</staff>')
    ok(len(beam_violations(doc(bad, note(240, "eighth", [(1, "end")], staff=1)))) >= 1,
       "<staff> 排在 <beam> 之后被抓到")
    # 8 type 在 beam 之后
    bad2 = ('<pitch><step>C</step><octave>4</octave></pitch><duration>240</duration>'
            '<voice>1</voice><beam number="1">begin</beam><type>eighth</type>')
    ok(len(beam_violations(doc(bad2, note(240, "eighth", [(1, "end")])))) >= 1,
       "<type> 排在 <beam> 之后被抓到")
    # 9 beam 在 notations 之后
    bad3 = ('<pitch><step>C</step><octave>4</octave></pitch><duration>240</duration>'
            '<voice>1</voice><type>eighth</type>'
            '<notations><tied type="start"/></notations>'
            '<beam number="1">begin</beam>')
    ok(len(beam_violations(doc(bad3, note(240, "eighth", [(1, "end")])))) >= 1,
       "<beam> 排在 <notations> 之后被抓到")
    # 10 次级梁两条 run 各自闭合
    ok(beam_violations(doc(note(120, "16th", [(1, "begin"), (2, "begin")]),
                           note(120, "16th", [(1, "continue"), (2, "end")]),
                           note(240, "eighth", [(1, "end")]))) == [],
       "次级梁单 run（begin/end）合法")
    # 11 次级梁悬挂后遇非参与者
    ok(len(beam_violations(doc(note(120, "16th", [(1, "begin"), (2, "begin")]),
                               note(240, "eighth", [(1, "continue")]),
                               note(120, "16th", [(1, "end")])))) >= 1,
       "次级梁悬挂未闭合被抓到")
    # 12 两层同时闭合（收尾的是八分，二级梁已在此之前闭合）
    ok(beam_violations(doc(note(120, "16th", [(1, "begin"), (2, "begin")]),
                           note(120, "16th", [(1, "continue"), (2, "continue")]),
                           note(120, "16th", [(1, "continue"), (2, "end")]),
                           note(240, "eighth", [(1, "end")]))) == [],
       "两层梁同时闭合合法")
    # 13 strip_beams 只删梁
    raw = doc(note(240, "eighth", [(1, "begin")]))
    ok("<beam" not in strip_beams(raw) and "<type>eighth</type>" in strip_beams(raw),
       "strip_beams 只删 <beam> 元素")
    # 14 strip_beams 保留 notations
    ok("<notations>" in strip_beams(doc(note(240, "eighth", [(1, "begin")],
                                             notch=True))),
       "strip_beams 不动 <notations>")
    # 15 和弦成员梁不一致
    ok(len(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                               note(240, "eighth", [], chord=True),
                               note(240, "eighth", [(1, "end")])))) >= 1,
       "和弦成员缺梁被抓到")
    # 16 和弦成员梁一致
    ok(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                           note(240, "eighth", [(1, "begin")], chord=True),
                           note(240, "eighth", [(1, "end")]),
                           note(240, "eighth", [(1, "end")], chord=True))) == [],
       "和弦成员同梁合法")
    # 17 倚音不参与状态机
    ok(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                           note(0, "eighth", [], grace=True),
                           note(240, "eighth", [(1, "end")]))) == [],
       "倚音不打断连梁")
    # 18 梁到小节末未闭合
    ok(len(beam_violations(doc(note(240, "eighth", [(1, "begin")]),
                               note(240, "eighth", [(1, "continue")])))) == 1,
       "小节末未闭合被抓到")

    # 19 覆盖硬约束：缺二级梁
    ok(any("二级" in x for x in coverage_errors({1: 10}, {"begin": 5, "end": 5})),
       "二级梁覆盖丢失被抓到")
    # 20 覆盖硬约束：缺 continue
    ok(any("continue" in x for x in coverage_errors({1: 10, 2: 3},
                                                    {"begin": 2, "end": 2})),
       "continue 覆盖丢失被抓到")
    # 21 覆盖齐全时不报错，且状态统计口径正确
    ok(coverage_errors({1: 10, 2: 3},
                       {"begin": 2, "continue": 1, "end": 2}) == [],
       "二级梁与 continue 都在时不报错")
    # 22 压力样本被删也要报（CI 的「无未跟踪素材」只防新增，不防删除）
    ok(any("beam_levels" in x for x in coverage_errors(
           {1: 10, 2: 3}, {"begin": 2, "continue": 1, "end": 2}, False)),
       "压力样本被删被抓到")
    ok(beam_states('<beam number="1">begin</beam>'
                   '<beam number="2">continue</beam>'
                   '<beam number="1">end</beam>')
       == {"begin": 1, "continue": 1, "end": 1},
       "beam_states 按状态计数")

    # 24 正则必须能吃下含空格的 hook 值（\w+ 会漏，零回归会假 DIFF）
    hk = doc(note(120, "16th", [(1, "begin"), (2, "forward hook")]),
             note(240, "eighth", [(1, "continue")]),
             note(120, "16th", [(1, "end"), (2, "backward hook")]))
    ok(len(BEAM_RE.findall(hk)) == 5,
       "BEAM_RE 能解析 forward/backward hook")
    ok("<beam" not in strip_beams(hk),
       "strip_beams 能把钩形梁一起剥掉")
    # 25 合法钩形梁序列：组首 forward、组尾 backward，状态机不误报
    ok(beam_violations(hk) == [],
       "组首 forward hook + 组尾 backward hook 结构合法")
    # 26 hook 落在本层未闭合的梁组中间要报错
    ok(any("钩形梁落在未闭合梁组中" in x for x in beam_violations(
           doc(note(120, "16th", [(1, "begin"), (2, "begin")]),
               note(120, "16th", [(1, "end"), (2, "forward hook")])))),
       "钩形梁夹在未闭合梁组中间被抓到")
    # 27 层级完整性：16th 参与连梁却缺第二层（旧版缺陷的指纹）
    ok(any("缺第 2 层" in x for x in beam_violations(
           doc(note(120, "16th", [(1, "begin")]),
               note(240, "eighth", [(1, "continue")]),
               note(120, "16th", [(1, "end")])))),
       "16th 参与连梁却缺二级梁被抓到")
    # 28 用 hook 补齐层级后不再报「缺层」
    ok(not any("缺第" in x for x in beam_violations(hk)),
       "16th 用钩形梁补齐第二层后层级完整")
    # 29 32nd 缺第三层也要抓到
    ok(any("缺第 3 层" in x for x in beam_violations(
           doc(note(60, "32nd", [(1, "begin"), (2, "begin")]),
               note(60, "32nd", [(1, "end"), (2, "end")])))),
       "32nd 缺三级梁被抓到")
    # 30 完全孤立的 16th（不参与连梁）画的是符尾，不要求 <beam>
    ok(beam_violations(doc(note(120, "16th", []))) == [],
       "孤立 16th 不参与连梁时不要求层级")
    # 31 hook_stats 口径
    ok(hook_stats(hk) == {(2, "forward hook"): 1, (2, "backward hook"): 1},
       "hook_stats 按 (层号, 状态) 计数")
    # 32 beam_states 也统计 hook
    ok(beam_states(hk).get("forward hook", 0) == 1,
       "beam_states 统计 forward hook")
    # 33~36 钩形梁覆盖硬约束
    full_hooks = {(2, "forward hook"): 2, (2, "backward hook"): 3,
                  (3, "forward hook"): 1, (3, "backward hook"): 1}
    good_states = {"begin": 2, "continue": 1, "end": 2}
    ok(any("forward hook" in x for x in coverage_errors(
           {1: 10, 2: 3}, good_states,
           hooks={(2, "backward hook"): 2, (3, "backward hook"): 1})),
       "缺 forward hook 覆盖被抓到")
    ok(any("backward hook" in x for x in coverage_errors(
           {1: 10, 2: 3}, good_states,
           hooks={(2, "forward hook"): 2, (3, "forward hook"): 1})),
       "缺 backward hook 覆盖被抓到")
    ok(any("三级" in x for x in coverage_errors(
           {1: 10, 2: 3}, good_states,
           hooks={(2, "forward hook"): 2, (2, "backward hook"): 2})),
       "缺三级钩形梁覆盖被抓到")
    ok(any("beam_hooks" in x for x in coverage_errors(
           {1: 10, 2: 3}, good_states, hooks=full_hooks,
           has_hook_fixture=False)),
       "钩形梁压力样本被删被抓到")
    ok(coverage_errors({1: 10, 2: 3}, good_states,
                       hooks=full_hooks) == [],
       "钩形梁三维覆盖齐全时不报错")
    ok(coverage_errors({1: 10, 2: 3}, good_states, hooks=None) == [],
       "hooks=None 时跳过钩形梁覆盖检查（向后兼容）")

    print("check_xml_beams self-test: %d 项，%d 失败" % (n, len(failed)))
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
            print("用法: check_xml_beams.py --regress <旧版exe>")
            return 2
        changed, added, errors, notes = structural_regress(args[idx + 1])
        print("结构性零回归：%d 份素材的 MusicXML 只多了 <beam>" % changed)
        for x in notes:
            print("  [NOTE]", x)
        for e in errors:
            print("  [FAIL]", e)
        return 1 if errors else 0

    exe = default_exe()
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad, with_beam, total, per, states, hooks = audit_fixtures(exe)
    if bad:
        print("MusicXML 连梁结构问题：%d 处" % len(bad))
        for name, msg in bad[:20]:
            print("  %s: %s" % (name, msg))
        return 1
    cov = coverage_errors(per, states,
                          os.path.exists(os.path.join(FIX, "beam_levels.mid")),
                          hooks,
                          os.path.exists(os.path.join(FIX, "beam_hooks.mid")))
    if cov:
        print("MusicXML 连梁覆盖不足：")
        for c in cov:
            print("  [FAIL]", c)
        return 1
    dist = ", ".join("%d 级 %d 个" % (k, per[k]) for k in sorted(per))
    nh = sum(hooks.values())
    print("MusicXML 连梁：%d 份素材有梁，共 %d 个 <beam>（%s，其中钩形梁 %d 个），0 处结构问题"
          % (with_beam, total, dist or "无", nh))
    return 0


if __name__ == "__main__":
    sys.exit(main())

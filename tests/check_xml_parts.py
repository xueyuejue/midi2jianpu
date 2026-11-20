#!/usr/bin/env python3
"""MusicXML 各 part 必须等长（矩形谱），短轨的尾巴必须是整小节休止。

score-partwise 是一张矩形谱：各 <part> 的 <measure> 按「在自己 part 里的第几个」
对齐，所以每个 part 的小节数必须相同。渲染循环原本是「按片段推进」的——某条轨
在倒数第三小节就没音了，它的 part 就停在那儿：

  * sample.mid           Pad 3 小节，Vocal / Piano 4 小节
  * pedal_channels.mid   Piano 2 小节，Bass 3 小节
  * sysex_gs_normal.mid / sysex_unknown.mid
                         Drums 1 小节，Melody 2 小节

导入后那条谱表在末尾凭空少一节，与其它声部错位。前导空小节、中间空小节本来就
有补齐（源码注释里写的理由是「否则各声部的小节号与 MuseScore 的编号对不上」），
唯独尾节漏了——同一件事没做完，所以 v1.146.0 把尾节补上。

本脚本守三件事：

  1. 同一份 XML 里所有 part 的 <measure number> 序列完全相同；
  2. 每个 part 的尾节（最后一条有音的小节之后）必须是整小节休止：含
     `<rest measure="yes"/>`、且不含任何 <pitch>/<unpitched>；
  3. 小节号逐个 +1（弱起从 0 起算也一样）。

`--regress OLD` 做的是**定向证明**：把新版每个 part 多出来的尾节删掉，剩下的
必须与旧版逐字节相同。于是「本轮只补了尾节」这件事不靠人眼，靠 diff。

用法：
  python tests/check_xml_parts.py                # 全量审计
  python tests/check_xml_parts.py --self-test    # 纯逻辑自测
  python tests/check_xml_parts.py --exe PATH     # 指定可执行文件
  python tests/check_xml_parts.py --regress OLD  # 拿旧版跑，本轮修复的问题应当报出来
"""
import difflib
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# <part> 之间不嵌套；<measure> 之间也不嵌套，非贪婪匹配到自己的收尾即可。
PART_RE = re.compile(r'<part id="([^"]+)">(.*?)</part>', re.S)
MEAS_RE = re.compile(r'[ \t]*<measure number="([^"]+)"[^>]*>.*?</measure>\n?', re.S)


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def render(exe, mid, out):
    r = subprocess.run([exe, mid, "--musicxml", out],
                       capture_output=True, timeout=120)
    if r.returncode != 0:
        return None
    with open(out, encoding="utf-8", errors="replace") as f:
        return f.read()


def spans(xml):
    """[(part_id, [(小节号, 起点偏移, 终点偏移), ...]), ...]，按文档顺序。"""
    out = []
    for pm in PART_RE.finditer(xml):
        base = pm.start(2)
        ms = [(m.group(1), base + m.start(), base + m.end())
              for m in MEAS_RE.finditer(pm.group(2))]
        out.append((pm.group(1), ms))
    return out


def parts(xml):
    """[(part_id, [(小节号, 小节原文), ...]), ...]。"""
    return [(pid, [(num, xml[a:b]) for num, a, b in ms]) for pid, ms in spans(xml)]


def has_tone(body):
    """这一小节里有没有实音（<pitch> / <unpitched>）。"""
    return "<pitch>" in body or "<unpitched>" in body


def tail_len(measures):
    """尾部连续「无实音」小节的个数。"""
    n = 0
    for _, body in reversed(measures):
        if has_tone(body):
            break
        n += 1
    return n


def audit_xml(xml, label):
    """一份 XML 的矩形谱问题列表（空 = 合格）。"""
    errs = []
    ps = parts(xml)
    if not ps:
        return ["%s: 没有任何 <part>" % label]

    # 0) part-list 声明过的 part 必须在正文里真的存在（否则导入器会少一条谱表）
    listed = re.findall(r'<score-part id="([^"]+)"', xml)
    present = [pid for pid, _ in ps]
    for pid in listed:
        if pid not in present:
            errs.append("%s: part-list 声明了 %s，正文却没有对应的 <part>"
                        % (label, pid))

    # 1) 小节号序列必须完全一致
    seqs = {pid: [num for num, _ in ms] for pid, ms in ps}
    base_pid = ps[0][0]
    base = seqs[base_pid]
    for pid, _ in ps:
        if seqs[pid] != base:
            errs.append("%s: part %s 的小节序列与 %s 不同（%d 节 vs %d 节）"
                        % (label, pid, base_pid, len(seqs[pid]), len(base)))

    for pid, ms in ps:
        # 2) 尾节必须是整小节休止。tail_len 只数「无实音」的末尾小节，所以
        # 这里唯一要查的是它们有没有写成整小节休止——写成普通休止（没有
        # measure="yes"）会让导入器按拍号去凑，谱面多出一串休止符。
        k = tail_len(ms)
        for num, body in ms[len(ms) - k:]:
            if '<rest measure="yes"/>' not in body:
                errs.append("%s: part %s 尾节 %s 不是整小节休止（缺 "
                            '<rest measure="yes"/>）' % (label, pid, num))
        # 3) 小节号逐个 +1
        nums = seqs[pid]
        for a, b in zip(nums, nums[1:]):
            try:
                ia, ib = int(a), int(b)
            except ValueError:
                continue
            if ib != ia + 1:
                errs.append("%s: part %s 小节号跳变 %s -> %s"
                            % (label, pid, a, b))
    return errs


def strip_extra(new_xml, old_xml):
    """删掉新版里比旧版多出来的尾节，返回剩下的 XML。

    这是 `--regress` 的定向证明：如果本轮的改动真的只是「给短 part 补尾节」，
    删掉多出来的那几节之后，新版应当与旧版**逐字节相同**。
    """
    old_len = {pid: len(ms) for pid, ms in parts(old_xml)}
    drop = []
    for pid, ms in spans(new_xml):
        extra = len(ms) - old_len.get(pid, 0)
        if extra > 0:
            drop.extend(ms[len(ms) - extra:])
    out = new_xml
    for _, a, b in sorted(drop, key=lambda x: x[1], reverse=True):
        out = out[:a] + out[b:]
    return out


def audit(exe):
    errs = []
    for mid in fixtures():
        name = os.path.basename(mid)
        with tempfile.TemporaryDirectory() as td:
            xml = render(exe, mid, os.path.join(td, "o.xml"))
        if xml is None:
            errs.append("%s: 导出 MusicXML 失败" % name)
            continue
        errs.extend(audit_xml(xml, name))
    if errs:
        print("MusicXML 矩形谱审计：%d 处问题" % len(errs))
        for e in errs:
            print("  [FAIL]", e)
        return errs
    print("MusicXML 矩形谱审计通过：%d 份素材各 part 等长、尾节均为整小节休止、"
          "小节号连续" % len(fixtures()))
    return []


def regress(old_exe, new_exe):
    errs = []
    padded = []
    for mid in fixtures():
        name = os.path.basename(mid)
        with tempfile.TemporaryDirectory() as td:
            old = render(old_exe, mid, os.path.join(td, "o.xml"))
            new = render(new_exe, mid, os.path.join(td, "n.xml"))
        if old is None or new is None:
            errs.append("%s: 导出 MusicXML 失败" % name)
            continue
        if old == new:
            continue
        stripped = strip_extra(new, old)
        if stripped == old:
            padded.append(name)
            continue
        d = list(difflib.unified_diff(stripped.splitlines(), old.splitlines(),
                                      "new-去尾节", "old", lineterm="", n=0))
        errs.append("%s: 去掉尾节后仍与旧版不同\n%s"
                    % (name, "\n".join(d[:12])))
    if padded:
        print("[NOTE] 本轮只在尾部补了整小节休止、去掉尾节后逐字节相同的素材："
              "%d 份（%s）" % (len(padded), " / ".join(padded)))
    if errs:
        print("尾节补齐定向回归：%d 处不符" % len(errs))
        for e in errs:
            print("  [FAIL]", e)
        return errs
    print("尾节补齐定向回归通过：所有素材的差异都只是尾部补齐的整小节休止")
    return []


# ---------------------------------------------------------------------------
# 自测：纯逻辑，不调用可执行文件
# ---------------------------------------------------------------------------
def _part(pid, measures):
    return '<part id="%s">\n%s</part>\n' % (pid, "".join(measures))


def _meas(num, body, implicit=False):
    imp = ' implicit="yes"' if implicit else ""
    return '    <measure number="%s"%s>%s</measure>\n' % (num, imp, body)


def _tone(dur=480):
    return ('<note><pitch><step>C</step><octave>4</octave></pitch>'
            '<duration>%d</duration><voice>1</voice><type>quarter</type></note>'
            % dur)


def _whole_rest(dur=1920):
    return ('<note><rest measure="yes"/><duration>%d</duration>'
            '<voice>1</voice><type>whole</type></note>' % dur)


def self_test():
    n = 0
    failed = []

    def ck(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    rect = ('<score-partwise>\n'
            + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
            + _part("P2", [_meas("1", _tone()), _meas("2", _tone())])
            + '</score-partwise>\n')
    ck(audit_xml(rect, "rect") == [], "两 part 等长应通过：%r"
       % audit_xml(rect, "rect"))

    short = ('<score-partwise>\n'
             + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
             + _part("P2", [_meas("1", _tone())])
             + '</score-partwise>\n')
    ck(any("小节序列" in e for e in audit_xml(short, "short")),
       "短轨必须报「小节序列不同」")

    padded = ('<score-partwise>\n'
              + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
              + _part("P2", [_meas("1", _tone()), _meas("2", _whole_rest())])
              + '</score-partwise>\n')
    ck(audit_xml(padded, "padded") == [], "尾节是整小节休止应通过：%r"
       % audit_xml(padded, "padded"))

    # 尾节写了普通休止（没有 measure=yes）→ 不合格
    plain_rest = ('<score-partwise>\n'
                  + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
                  + _part("P2", [_meas("1", _tone()),
                                 _meas("2", '<note><rest/><duration>1920</duration>'
                                           '<voice>1</voice><type>whole</type></note>')])
                  + '</score-partwise>\n')
    ck(any('rest measure="yes"' in e for e in audit_xml(plain_rest, "pr")),
       "尾节缺 rest measure=\"yes\" 必须报错")

    # 小节号跳变
    jump = ('<score-partwise>\n'
            + _part("P1", [_meas("1", _tone()), _meas("3", _tone())])
            + '</score-partwise>\n')
    ck(any("小节号跳变" in e for e in audit_xml(jump, "jump")),
       "小节号跳变必须报错")

    # 弱起从 0 起算：0,1,2 连续即可
    pickup = ('<score-partwise>\n'
              + _part("P1", [_meas("0", _tone(960), True), _meas("1", _tone()),
                             _meas("2", _tone())])
              + '</score-partwise>\n')
    ck(audit_xml(pickup, "pickup") == [], "弱起 0 起算应通过：%r"
       % audit_xml(pickup, "pickup"))

    # part-list 有、正文没有：必须报错
    missing = ('<score-partwise><part-list>'
               '<score-part id="P1"/><score-part id="P2"/></part-list>\n'
               + _part("P1", [_meas("1", _tone())])
               + '</score-partwise>\n')
    ck(any("正文却没有" in e for e in audit_xml(missing, "missing")),
       "part-list 声明但正文缺失的 part 必须报错")

    # 单 part 不存在跨 part 约束
    solo = ('<score-partwise>\n'
            + _part("P1", [_meas("1", _tone()), _meas("2", _whole_rest())])
            + '</score-partwise>\n')
    ck(audit_xml(solo, "solo") == [], "单 part 尾节是休止应通过")

    # 定向回归：把尾节删掉之后要与旧版逐字节相同
    old = ('<score-partwise>\n'
           + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
           + _part("P2", [_meas("1", _tone())])
           + '</score-partwise>\n')
    new = ('<score-partwise>\n'
           + _part("P1", [_meas("1", _tone()), _meas("2", _tone())])
           + _part("P2", [_meas("1", _tone()), _meas("2", _whole_rest())])
           + '</score-partwise>\n')
    ck(strip_extra(new, old) == old, "删掉多出的尾节后应与旧版逐字节相同")
    ck(strip_extra(old, old) == old, "没有多出的尾节时应原样返回")
    # 多改了一个音符时不能靠「删尾节」蒙混过去
    tampered = new.replace("<octave>4</octave>", "<octave>5</octave>", 1)
    ck(strip_extra(tampered, old) != old, "改动音符后不能伪装成尾节补齐")

    print("check_xml_parts self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 0 if not failed else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    exe = default_exe()
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    if "--regress" in sys.argv:
        old = sys.argv[sys.argv.index("--regress") + 1]
        print("[回归] 旧版可执行文件：%s（本轮应只多出尾节）" % old)
        return 1 if regress(old, exe) else 0
    return 1 if audit(exe) else 0


if __name__ == "__main__":
    sys.exit(main())

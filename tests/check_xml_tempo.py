#!/usr/bin/env python3
"""MusicXML 速度记号不受「表情开关」影响的全量校验。

速度是回放速度，不是演奏记号。`--no-dynamics`（网页上是「力度与演奏法」
复选框）的文档范围只有「力度 / 断音 / 滑音 / 段落」，README 还承诺「没有
任何表情数据的文件输出不变」。

但在 v1.145.0 之前，速度记号整块长在 emitDirections 里，而那个 lambda 的
第一句是 `if (!expr.enabled()) return;`。于是关掉表情，速度跟着一起没了：

  * midtempo.mid   <sound tempo> 4 -> 1（120→90→60→120 只剩谱头 120）
  * tempo_ramp.mid <sound tempo> 6 -> 1，rit. / accel. 全丢

txt 与 HTML 一字节不变，只有 MusicXML 变——导进 MuseScore 一律按默认 120
播到底，与谱头印着的速度自相矛盾。

本脚本对每份素材各导出一次 MusicXML（开表情 / 关表情），按出现顺序抽出
所有「速度方向块」（含 `<metronome>` 或 `<sound tempo=` 的 `<direction>`），
要求两份序列**逐字相同**。只抽速度块，所以力度、楔形、段落名的消失不算
问题——那正是 `--no-dynamics` 该做的事。

用法：
  python tests/check_xml_tempo.py                # 全量审计
  python tests/check_xml_tempo.py --self-test    # 纯逻辑自测
  python tests/check_xml_tempo.py --exe PATH     # 指定可执行文件
  python tests/check_xml_tempo.py --regress OLD  # 拿旧版跑，本轮应报出修复前的问题
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# <direction> 之间不会互相嵌套，非贪婪匹配到本块的 </direction> 即可。
DIRECTION_RE = re.compile(r"<direction\b[^>]*>.*?</direction>", re.S)
# 速度块的两个特征：♩=N 的 <metronome>，以及真正影响回放的 <sound tempo>。
TEMPO_MARKERS = ("<metronome>", "<sound tempo=")


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def tempo_blocks(xml):
    """按出现顺序抽出该份 MusicXML 里所有速度方向块（原文，逐字比较用）。"""
    out = []
    for blk in DIRECTION_RE.findall(xml):
        if any(m in blk for m in TEMPO_MARKERS):
            out.append(blk)
    return out


def compare(on_xml, off_xml):
    """开 / 关表情两份输出的速度块序列差异；返回 [(类别, 明细)]。"""
    a, b = tempo_blocks(on_xml), tempo_blocks(off_xml)
    if a == b:
        return []
    if len(a) != len(b):
        return [("数量", "开表情 %d 条 / 关表情 %d 条" % (len(a), len(b)))]
    # 数量相同但内容不同：定位第一条不同的块
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return [("内容", "第 %d 条速度块不一致：%s / %s"
                     % (i + 1, shrink(x), shrink(y)))]
    return [("顺序", "条数与内容都对齐，但顺序不同")]


def shrink(block):
    """压成一行便于打印：去掉缩进与换行，只留标签。"""
    return " ".join(block.split())


def render(exe, mid, extra, td, slot):
    out = os.path.join(td, "%s.xml" % slot)
    subprocess.run([exe, mid, "--musicxml", out] + list(extra),
                   capture_output=True)
    try:
        return open(out, encoding="utf-8").read()
    except OSError:
        return None


def audit(exe, verbose=True):
    errors = []
    files = sorted(glob.glob(os.path.join(FIX, "*.mid")))
    blocks = 0
    with tempfile.TemporaryDirectory() as td:
        for mid in files:
            name = os.path.basename(mid)
            on = render(exe, mid, [], td, "on")
            off = render(exe, mid, ["--no-dynamics"], td, "off")
            if on is None or off is None:
                errors.append("%s: 渲染失败" % name)
                continue
            blocks += len(tempo_blocks(on))
            for kind, detail in compare(on, off):
                errors.append("%s: %s" % (name, detail))
    if verbose:
        print("fixtures %d / 速度方向块 %d 条" % (len(files), blocks))
        for e in errors[:20]:
            print("  [FAIL] " + e)
        print("速度记号不受表情开关影响: %s（%d 项问题）"
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

    step = ("      <direction placement=\"above\">\n"
            "        <direction-type><metronome><beat-unit>quarter</beat-unit>"
            "<per-minute>90</per-minute></metronome></direction-type>\n"
            "        <offset>480</offset>\n"
            "        <sound tempo=\"90\"/>\n"
            "      </direction>\n")
    header = ("      <direction placement=\"above\">\n"
              "        <direction-type><metronome><beat-unit>quarter</beat-unit>"
              "<per-minute>120</per-minute></metronome></direction-type>"
              "<sound tempo=\"120\"/>\n"
              "      </direction>\n")
    ramp = ("      <direction placement=\"above\">\n"
            "        <direction-type><words>rit.</words></direction-type>"
            "<sound tempo=\"140\"/>\n"
            "      </direction>\n")
    dyn = ("      <direction placement=\"above\">\n"
           "        <direction-type><dynamics><mf/></dynamics></direction-type>\n"
           "        <sound dynamics=\"75\"/>\n"
           "      </direction>\n")

    # 1. 完全一致 -> 无差异
    ck(compare(header + step, header + step) == [], "两份相同的速度块不报差异")

    # 2. 少一条 -> 数量不符（旧版 --no-dynamics 就是这个症状）
    errs = compare(header + step, header)
    ck(len(errs) == 1 and errs[0][0] == "数量", "少一条速度块按数量报出（%r）" % errs)

    # 3. 数量相同但数值不同 -> 内容不符
    other = step.replace("90", "60")
    errs = compare(header + step, header + other)
    ck(len(errs) == 1 and errs[0][0] == "内容", "同数量的速度值不同按内容报出")

    # 4. 顺序不同 -> 也要报（谱面播放顺序不能乱）
    errs = compare(header + step + ramp, header + ramp + step)
    ck(len(errs) == 1, "速度块顺序变了会被抓到（%r）" % errs)

    # 5. 只有力度块不同 -> 不报（那正是 --no-dynamics 该做的）
    ck(compare(dyn + header, header) == [], "力度块消失不算速度问题")

    # 6. <sound dynamics> 不能被当成速度块
    ck(tempo_blocks(dyn) == [], "只有 <sound dynamics> 的方向块不算速度块")

    # 7. 没有 <metronome>、只有 <sound tempo> 的 rit. 排版也算法速度块
    ck(len(tempo_blocks(ramp)) == 1, "rit. 起点排版（<words> + <sound tempo>）也算速度块")

    # 8. 相邻两块不许合并：抽出来的必须是两条独立块
    two = tempo_blocks(header + step)
    ck(len(two) == 2 and all(b.strip().startswith("<direction") for b in two),
       "相邻的速度块各自成块（%d）" % len(two))

    # 9. 空输入 -> 空表，不抛异常
    ck(tempo_blocks("") == [], "空输入返回空表")

    # 10. 段落名的 <words> 没有 <sound tempo> 时不算速度块
    sect = ("      <direction placement=\"above\">\n"
            "        <direction-type><words>副歌</words></direction-type>\n"
            "      </direction>\n")
    ck(tempo_blocks(sect) == [], "纯段落名的方向块不算速度块")

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

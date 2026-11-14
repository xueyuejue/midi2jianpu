#!/usr/bin/env python3
"""CLI 参数矩阵 × 全素材端到端不变量 + 移调正确性守护。

为什么单独立一道闸门：
    其余闸门都在**默认参数**下遍历素材：10 个全量守卫跑的是裸 `midi2jianpu F`，
    冒烟只对少数样例点开个别开关，fuzz 是字节级随机突变、从不组合 CLI 开关。
    于是「某个开关在某份素材上把 MusicXML 写坏 / 让 HTML 冒出裸 <script> /
    让移调把八度或调号算错」这类缺陷，默认参数路径跑不出来——v1.91.1 的
    MusicXML 控制字符缺陷正是这样溜过 fuzz 的（fuzz 只是碰巧突变到了轨名）。
    本闸门把「每个素材 × 一组开关」都跑一遍，锁死参数矩阵的端到端不变量：
        ① 退出码 ∈ {0,1,2,3,4}（合法退出码集合）；
        ② rc==0 且 --musicxml 时，MusicXML 必须能被 ElementTree 严格解析；
        ③ rc==0 且 --html 时，HTML 必须是合法 UTF-8；不开 --player 时整页无 script；
        ④ --track 0..N-1 与越界轨：rc ∈ {0,4}，rc==0 时 MusicXML 可解析；
        ⑤ 移调不变量：--transpose 0 与默认逐字节一致；--transpose 12 / 24 时
           每个音符的音名 / 升降不变、八度恰好 +1 / +2，且调号 <fifths> 一字不改。

用法：
    python tests/check_cli_matrix.py [--exe PATH]
        对全部 fixture 跑参数矩阵 / 轨扫描 / 移调不变量。
    python tests/check_cli_matrix.py --self-test
        纯逻辑自测，不调用可执行文件。

不依赖第三方库；失败返回非 0，可直接用于 CI。
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 合法退出码集合：0 成功 / 1 参数错 / 2 解析错 / 3 空谱 / 4 选定轨无音符。
VALID_RC = {0, 1, 2, 3, 4}

# 参数矩阵：覆盖解析期（--no-quantize / --no-dynamics / --no-key-changes /
# --no-pentatonic）与输出期（--bar-numbers / --transpose）各类开关。不追求
# 穷举，只要求「每一类开关都至少在全部素材上过一遍」——选 5 类最具代表性
# 的，既覆盖解析、输出、移调三条路径，又把 CI 步骤耗时压在合理范围。
FLAGSETS = [
    ["--no-quantize"],
    ["--no-dynamics"],
    ["--no-key-changes"],
    ["--bar-numbers"],
    ["--transpose", "2"],
]

# CLI 边界契约表（v1.97.0）：(附加参数, 期望 rc, 说明)。
# 起因：v1.96 只给 --transpose 补了边界契约，其余数值参数的边界散落在
# fuzz 的 HOSTILE_ARGS 里（且 --pickup / --soft-pedal-scale 根本没进矩阵），
# 而「--pickup N 与 --no-pickup」这类语义矛盾组合此前完全没有确定性守护。
# 这里把全部数值边界 + 矛盾组合收成一张表，逐条跑 CLI 锁死退出码。
# 期望 rc 可为 int 或集合（--track 0 在空轨素材上合法地返回 4）。
CLI_CONTRACTS = [
    # ---- 数值参数：语义越界即拒 rc=1 ----
    (["--bars", "0"], 1, "--bars 下界外"),
    (["--bars", "9"], 1, "--bars 上界外"),
    (["--pickup", "0"], 1, "--pickup 必须正数"),
    (["--pickup", "99"], 1, "--pickup 不短于小节"),
    (["--soft-pedal-scale", "-1"], 1, "--soft-pedal-scale 下界外"),
    (["--soft-pedal-scale", "5"], 1, "--soft-pedal-scale 上界外"),
    (["--track", "-1"], 1, "--track 负数"),
    (["--transpose", "999"], 1, "--transpose 上界外"),
    (["--transpose", "-999"], 1, "--transpose 下界外"),
    (["--transpose", "61"], 1, "--transpose 恰好越上界"),
    (["--transpose", "-61"], 1, "--transpose 恰好越下界"),
    (["--key", "ZZ"], 1, "--key 非法调名"),
    # ---- 语义矛盾组合：明确拒绝 rc=1（v1.97.0） ----
    (["--pickup", "1", "--no-pickup"], 1, "--pickup 与 --no-pickup 冲突"),
    (["--no-pickup", "--pickup", "99"], 1, "冲突组合不得绕过越界校验"),
    (["--key", "C", "--gong", "D"], 1, "--key 与 --gong 冲突"),
    # ---- 合法边界内：rc=0（--track 0 允许空轨的 4） ----
    (["--bars", "1"], 0, "--bars 下界内"),
    (["--bars", "8"], 0, "--bars 上界内"),
    (["--pickup", "1"], 0, "--pickup 合法"),
    (["--soft-pedal-scale", "0"], 0, "--soft-pedal-scale 下界内"),
    (["--soft-pedal-scale", "1"], 0, "--soft-pedal-scale 上界内"),
    (["--track", "0"], {0, 4}, "--track 首轨"),
    (["--transpose", "0"], 0, "--transpose 零移调"),
    (["--transpose", "24"], 0, "--transpose 两个八度"),
    (["--transpose", "60"], 0, "--transpose 恰好上界"),
    (["--transpose", "-60"], 0, "--transpose 恰好下界"),
    (["--key", "C"], 0, "--key 合法调名"),
    (["--no-pickup"], 0, "--no-pickup 单独使用"),
    # ---- 同一带值选项重复给：明确拒绝 rc=1（v1.99.0） ----
    # 旧版清一色 last-wins：--html a --html b 只写 b、--key C --key D 把
    # 调性改成 D，全程 rc=0、零提示。{OUT} / {OUT2} / {LRC} 在
    # check_cli_contracts() 里换成临时目录下的真实路径。
    (["--key", "C", "--key", "D"], 1, "--key 重复给（旧版静默改成 D）"),
    (["--gong", "C", "--gong", "D"], 1, "--gong 重复给"),
    (["--bars", "2", "--bars", "3"], 1, "--bars 重复给"),
    (["--track", "0", "--track", "1"], 1, "--track 重复给"),
    (["--transpose", "2", "--transpose", "-2"], 1, "--transpose 重复给"),
    (["--pickup", "1", "--pickup", "2"], 1, "--pickup 重复给"),
    (["--soft-pedal-scale", "0.5", "--soft-pedal-scale", "0.7"], 1,
     "--soft-pedal-scale 重复给"),
    (["--html", "{OUT}", "--html", "{OUT2}"], 1,
     "--html 重复给（旧版只写后一个文件）"),
    (["--output", "{OUT}", "--output", "{OUT2}"], 1, "--output 重复给"),
    (["--musicxml", "{OUT}", "--musicxml", "{OUT2}"], 1, "--musicxml 重复给"),
    (["--lyrics", "{LRC}", "--lyrics", "{LRC}"], 1, "--lyrics 重复给"),
    # ---- 输出不得覆盖非 MIDI 源文件（v1.99.0） ----
    (["--lyrics", "{LRC}", "--html", "{LRC}"], 1,
     "HTML 覆盖 --lyrics 源文件（旧版把歌词盖成 HTML）"),
    (["--lyrics", "{LRC}", "--output", "{LRC}"], 1,
     "文本谱覆盖 --lyrics 源文件"),
    (["--lyrics", "{LRC}", "--musicxml", "{LRC}"], 1,
     "MusicXML 覆盖 --lyrics 源文件"),
    # ---- 合法对照：放行路径不能误伤 ----
    (["--lyrics", "{LRC}", "--html", "{OUT}"], 0, "--lyrics 正常加载"),
    (["--player", "--player"], 0, "开关类重复幂等"),
    (["--bars", "2", "--no-dynamics"], 0, "不同带值选项与开关共存"),
]

TRACKS_RE = re.compile(r"共\s*(\d+)\s*轨")


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def rc_ok(rc):
    """合法退出码判据：只接受 {0,1,2,3,4}，其余一律视为异常（崩溃 / 断言）。"""
    return rc in VALID_RC


def parse_notes(root):
    """从已解析的 MusicXML 根取音符为 (step, alter, octave) 序列，跳过休止。"""
    out = []
    for note in root.iter("note"):
        if note.find("rest") is not None:
            continue
        p = note.find("pitch")
        if p is None:
            continue
        step = p.findtext("step")
        alter = p.findtext("alter") or "0"
        octv = int(p.findtext("octave"))
        out.append((step, alter, octv))
    return out


def notes_from_xml(path):
    return parse_notes(ET.parse(path).getroot())


def fifths_from_xml(path):
    return [int(f.text) for f in ET.parse(path).getroot().iter("fifths")]


def shift_octaves(notes, k):
    """把一串 (step, alter, octave) 整体升 k 个八度，用于和移调产物比对。"""
    return sorted((s, a, o + k) for (s, a, o) in notes)


def compare_octave(base_notes, moved_notes, k):
    """校验 moved 恰好是 base 整体升 k 个八度（音名/升降不变、八度 +k）。

    返回 None 表示通过；否则返回第一处不一致的说明。用多重集合比较，避免
    大谱表 / 多声部下音符排序不同造成的假差异。
    """
    if len(base_notes) != len(moved_notes):
        return "音符数不一致：base=%d moved=%d" % (len(base_notes), len(moved_notes))
    want = shift_octaves(base_notes, k)
    got = sorted(moved_notes)
    if want != got:
        for a, b in zip(want, got):
            if a != b:
                return "期望 %r 实得 %r" % (a, b)
        return "多重集合不一致"
    return None


def track_count(exe, mid):
    r = subprocess.run([exe, mid, "--list-tracks"], capture_output=True, timeout=120)
    text = (r.stdout + r.stderr).decode("utf-8", "replace")
    m = TRACKS_RE.search(text)
    return int(m.group(1)) if m else None


def run(exe, args, timeout=120):
    try:
        return subprocess.run([exe] + args, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def check_cli_contracts(exe):
    """逐条跑 CLI_CONTRACTS，锁死全部数值参数边界与矛盾组合的退出码。

    v1.96 只有 --transpose 一张边界契约；v1.97 推广成表驱动，把
    --bars / --pickup / --soft-pedal-scale / --track / --key 的边界，
    以及 --pickup+--no-pickup、--key+--gong 这类语义冲突一并守死；
    v1.99 再补「同一带值选项重复给」与「输出覆盖 --lyrics 源文件」两组。
    固定用 4/4 的 html_meta_inject.mid，避免拍号差异让 --pickup 用例飘。
    {OUT} / {OUT2} / {LRC} 换成临时目录下的真实路径；每条用例前都重写
    LRC，免得「覆盖源文件」用例把后面用例的歌词文件弄脏。
    """
    f = os.path.join(FIX, "html_meta_inject.mid")
    if not os.path.exists(f):
        mids = sorted(glob.glob(os.path.join(FIX, "*.mid")))
        if not mids:
            return ["CLI 边界契约：找不到任何 fixture"]
        f = mids[0]
    errs = []
    lrc_text = "[00:00.00]你好\n[00:01.00]世界\n"
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "o.out")
        out2 = os.path.join(td, "o2.out")
        lrc = os.path.join(td, "words.lrc")
        slots = {"{OUT}": out, "{OUT2}": out2, "{LRC}": lrc}
        for args, want, desc in CLI_CONTRACTS:
            with open(lrc, "w", encoding="utf-8") as fh:
                fh.write(lrc_text)
            real = [slots.get(x, x) for x in args]
            wants = {want} if isinstance(want, int) else set(want)
            r = run(exe, [f] + real)
            if r is None:
                errs.append("%s（%s）: 超时" % (desc, " ".join(real)))
                continue
            if r.returncode not in wants:
                errs.append("%s（%s）: 期望 rc∈%s 实得 rc=%d"
                            % (desc, " ".join(real), sorted(wants), r.returncode))
    return errs


def check_one(exe, mid, td):
    """对单个素材做参数矩阵 / 轨扫描 / 移调不变量，返回问题列表。"""
    name = os.path.basename(mid)
    errs = []

    xml_p = os.path.join(td, "o.xml")
    html_p = os.path.join(td, "o.html")
    txt_p = os.path.join(td, "o.txt")

    # ---- ① 参数矩阵：一次调用同时产出 txt+html+xml，逐项校验 ----
    for flags in FLAGSETS:
        tag = " ".join(flags)
        r = run(exe, [mid] + flags +
                ["--output", txt_p, "--html", html_p, "--musicxml", xml_p])
        if r is None:
            errs.append("%s [%s]: 超时" % (name, tag))
            continue
        if not rc_ok(r.returncode):
            errs.append("%s [%s]: 非法退出码 %d" % (name, tag, r.returncode))
            continue
        if r.returncode != 0:
            continue
        try:
            ET.parse(xml_p)
        except ET.ParseError as e:
            errs.append("%s [%s]: MusicXML 非良构 —— %s" % (name, tag, e))
        blob = open(html_p, "rb").read()
        try:
            html = blob.decode("utf-8")
        except UnicodeDecodeError as e:
            errs.append("%s [%s]: HTML 非 UTF-8 —— %s" % (name, tag, e))
            html = None
        if html is not None and "<script" in html:
            errs.append("%s [%s]: 未开 --player 却出现 <script>" % (name, tag))

    # ---- ④ 轨扫描：全部在轨 + 一个越界轨；多轨素材再加一个远越界 ----
    n = track_count(exe, mid)
    if n is not None:
        probes = list(range(n)) + [n]
        if n >= 2:
            probes.append(n + 7)
        for t in probes:
            r = run(exe, [mid, "--track", str(t), "--musicxml", xml_p])
            if r is None:
                errs.append("%s --track %d: 超时" % (name, t))
                continue
            if r.returncode not in (0, 4):
                errs.append("%s --track %d: 退出码 %d（应 0/4）"
                            % (name, t, r.returncode))
                continue
            if r.returncode == 0:
                try:
                    ET.parse(xml_p)
                except ET.ParseError as e:
                    errs.append("%s --track %d: XML 非良构 —— %s" % (name, t, e))

    # ---- ⑤ 移调不变量 ----
    base = os.path.join(td, "base.xml")
    r = run(exe, [mid, "--musicxml", base])
    if r is not None and r.returncode == 0:
        base_notes = notes_from_xml(base)
        base_fifths = fifths_from_xml(base)

        # --transpose 0 必须与默认逐字节一致（解析层 semitones==0 直接 return）
        z = os.path.join(td, "z.xml")
        rz = run(exe, [mid, "--transpose", "0", "--musicxml", z])
        if rz is not None and rz.returncode == 0:
            if open(base, "rb").read() != open(z, "rb").read():
                errs.append("%s --transpose 0: 与默认输出不逐字节一致" % name)

        # --transpose 12 / 24：八度 +1 / +2，音名/升降与调号不变
        for k in (1, 2):
            up = os.path.join(td, "up%d.xml" % k)
            ru = run(exe, [mid, "--transpose", str(12 * k), "--musicxml", up])
            if ru is None or ru.returncode != 0:
                errs.append("%s --transpose %d: 退出码异常" % (name, 12 * k))
                continue
            diff = compare_octave(base_notes, notes_from_xml(up), k)
            if diff:
                errs.append("%s --transpose %d: %s" % (name, 12 * k, diff))
            if fifths_from_xml(up) != base_fifths:
                errs.append("%s --transpose %d: 调号 <fifths> 被改动（%r -> %r）"
                            % (name, 12 * k, base_fifths, fifths_from_xml(up)))
    return errs


def audit_fixtures(exe):
    errs = []
    for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
        with tempfile.TemporaryDirectory() as td:
            errs.extend(check_one(exe, p, td))
    return errs


def self_test():
    n = 0
    failed = []

    def ck(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    # 合法退出码判据
    for ok in (0, 1, 2, 3, 4):
        ck(rc_ok(ok), "rc=%d 应被接受" % ok)
    for bad in (5, -1, 139, 255):
        ck(not rc_ok(bad), "rc=%d 应被拒绝" % bad)

    # note 解析：跳过休止，取 step/alter/octave
    mini = ("<s><note><rest/></note>"
            "<note><pitch><step>C</step><alter>1</alter><octave>4</octave>"
            "</pitch></note>"
            "<note><pitch><step>E</step><octave>5</octave></pitch></note></s>")
    notes = parse_notes(ET.fromstring(mini))
    ck(notes == [("C", "1", 4), ("E", "0", 5)],
       "note 解析（跳休止 / 缺省 alter=0）: %r" % (notes,))

    # 八度整体平移
    ck(shift_octaves([("C", "0", 4)], 1) == [("C", "0", 5)], "shift_octaves +1")
    ck(shift_octaves([("C", "0", 4)], 2) == [("C", "0", 6)], "shift_octaves +2")

    # compare_octave：正例通过、负例报差异、排序无关
    base = [("C", "0", 4), ("E", "0", 4), ("G", "0", 4)]
    ck(compare_octave(base, [("C", "0", 5), ("E", "0", 5), ("G", "0", 5)], 1) is None,
       "compare_octave 正例应通过")
    ck(compare_octave(base, [("G", "0", 5), ("C", "0", 5), ("E", "0", 5)], 1) is None,
       "compare_octave 应与顺序无关")
    ck(compare_octave(base, [("C", "0", 5), ("E", "0", 5), ("G", "0", 6)], 1) is not None,
       "compare_octave 八度错应报差异")
    ck(compare_octave(base, [("C", "0", 5), ("D", "0", 5), ("G", "0", 5)], 1) is not None,
       "compare_octave 音名错应报差异")
    ck(compare_octave(base, [("C", "0", 5), ("E", "0", 5)], 1) is not None,
       "compare_octave 音符数不同应报差异")

    # 调号提取
    fx = ("<s><fifths>0</fifths><fifths>2</fifths><fifths>-1</fifths></s>")
    ck(fifths_from_xml_str(fx) == [0, 2, -1], "fifths 提取: %r" % (fifths_from_xml_str(fx),))

    # 轨数解析
    ck(TRACKS_RE.search("轨道列表 (format 1, 共 2 轨):").group(1) == "2", "轨数解析 2")
    ck(TRACKS_RE.search("共 12 轨") is not None, "轨数解析 12")
    ck(TRACKS_RE.search("没有轨数") is None, "无轨数时应返回 None")

    # CLI 边界契约表结构（v1.97.0）
    def wants_of(w):
        return {w} if isinstance(w, int) else set(w)

    ck(bool(CLI_CONTRACTS), "CLI 边界契约表非空")
    ck(all(a and a[0].startswith("--") for a, w, d in CLI_CONTRACTS),
       "契约表每项参数都以 -- 开头")
    ck(all(wants_of(w) <= VALID_RC for a, w, d in CLI_CONTRACTS),
       "契约表期望退出码都在合法集合内")
    ck(any(wants_of(w) == {0} for a, w, d in CLI_CONTRACTS) and
       any(wants_of(w) == {1} for a, w, d in CLI_CONTRACTS),
       "契约表同时含放行与拒绝用例")
    covered = {x for a, w, d in CLI_CONTRACTS for x in a if x.startswith("--")}
    ck(all(o in covered for o in ("--bars", "--pickup", "--soft-pedal-scale",
                                  "--track", "--transpose", "--key",
                                  "--no-pickup", "--gong")),
       "契约表覆盖全部数值 / 弱起 / 调性参数")
    ck(any("--no-pickup" in a and "--pickup" in a
           for a, w, d in CLI_CONTRACTS),
       "契约表含 --pickup / --no-pickup 矛盾组合")
    ck(any(a.count("--key") == 2 for a, w, d in CLI_CONTRACTS),
       "契约表含同一带值选项重复给用例")
    ck(any("--lyrics" in a and "{LRC}" in a and wants_of(w) == {1}
           for a, w, d in CLI_CONTRACTS),
       "契约表含输出覆盖 --lyrics 源文件用例")
    ck(all(x in ("{OUT}", "{OUT2}", "{LRC}")
           for a, w, d in CLI_CONTRACTS for x in a if x.startswith("{")),
       "契约表占位符只用已定义的三种")
    ck(any(wants_of(w) == {0} and a.count("--player") == 2
           for a, w, d in CLI_CONTRACTS),
       "契约表含开关重复幂等放行用例")

    print("check_cli_matrix self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 0 if not failed else 1


def fifths_from_xml_str(s):
    return [int(f.text) for f in ET.fromstring(s).iter("fifths")]


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    errs = audit_fixtures(exe)
    terrs = check_cli_contracts(exe)
    errs.extend(terrs)
    if errs:
        print("CLI 参数矩阵校验失败：%d 处" % len(errs))
        for e in errs[:30]:
            print("  [FAIL]", e)
        return 1
    print("CLI 参数矩阵校验：全部 fixture × 参数矩阵 / 轨扫描 / 移调不变量均通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

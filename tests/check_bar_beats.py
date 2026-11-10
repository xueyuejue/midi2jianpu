#!/usr/bin/env python3
"""文本谱拍数守恒校验：一小节印出来的拍数必须等于拍号。

这是简谱最硬的约束：读者按 token 数拍子，休止符一旦按最近档吸附（1.75
拍记成 0. = 1.5 拍、1.25 拍记成 0 = 1 拍），后面的音头位置是定死的，
整小节就读少了拍，谱子没法照着唱。

用法：
    python tests/check_bar_beats.py [--exe PATH]
        对全部 fixture 跑文本谱，逐个非末小节核对拍数。
    python tests/check_bar_beats.py --self-test
        纯逻辑自测，不调用可执行文件。
    python tests/check_bar_beats.py --regress <旧版exe> [--exe <新版exe>]
        结构性零回归：修真实缺陷必然改动个别素材的文本谱，这里逐份证明
        「只插入了休止续写 token，没有删改任何旧 token」且「新版拍数守恒」。

不依赖第三方库；失败返回非 0，可直接用于 CI。
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

SUF = {"": 1.0, "_": 0.5, ".": 1.5, "_.": 0.75,
       "=": 0.25, "=.": 0.375, "%%": 0.125}
ART = "\u25bc\u00b7>\u2197\u2198tr,"
NOTE = re.compile(r"^([#b]?)([0-7]|-+)([',]*)(_\.|=\.|%%|_|=|\.)?(.*)$")


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def bar_beats(bar):
    """折算一小节印出来的拍数；认不出来的 token 返回 None。"""
    toks = re.sub(r"\[[^\]]*\]", " ", bar).split()
    tot = 0.0
    trip = False
    for t in toks:
        if t == ")":
            trip = False
            continue
        while t.startswith("3("):
            trip = True
            t = t[2:]
        t = re.sub(r"^\([0-7][',#b]*\)", "", t)      # 倚音前缀 (3)
        if not t:
            continue
        if "+" in t:                                  # 和弦整格只算一次
            t = t.split("+")[-1]
            if not re.match(r"^[#b]?[0-7][',#b]*", t):
                return None
        m = NOTE.match(t)
        if not m:
            return None
        core, suf, rest = m.group(2), m.group(4) or "", m.group(5)
        if rest and rest.strip(ART).strip("'"):
            return None
        if core.startswith("-"):
            # 「-<后缀>」是续写前缀：时值由后缀给出（-_. = 0.75 拍），
            # 没有后缀时一条横线才是一拍。
            tot += SUF[suf] if suf else len(core)
        elif trip:
            tot += 1.0 / 3
        else:
            if suf not in SUF:
                return None
            tot += SUF[suf]
    return tot


def parse_meters(lines):
    """从页眉摘要里抓初始拍号与转拍号表 {小节号: 拍数}。"""
    base = 4.0
    changes = {}
    for l in lines[:12]:
        m = re.search(r"拍号=(\d+)/(\d+)", l)
        if m:
            base = int(m.group(1)) * 4.0 / int(m.group(2))
        for num, den, at in re.findall(r"(\d+)/(\d+)\(第(\d+)小节\)", l):
            changes[int(at)] = int(num) * 4.0 / int(den)
    pickup = 0.0
    has_pickup = False
    for l in lines[:12]:
        if "弱起" in l:
            has_pickup = True
        m = re.search(r"弱起\s*([\d.]+)\s*拍", l)
        if m:
            pickup = float(m.group(1))
    return base, changes, pickup, has_pickup


def score_bars(lines):
    """返回每个小节的 {meter, text, beats, last}；一行可以有多个小节。"""
    base, changes, pickup, has_pickup = parse_meters(lines)
    track_bars = []
    for l in lines:
        if re.search(r"^\[[^\]]*\] \(\d+ 音符", l):
            track_bars.append([])
            continue
        if "|" not in l or not track_bars:
            continue
        for s in l.split("|")[:-1]:
            track_bars[-1].append(s)
    out = []
    for bars in track_bars:
        n = 0 if has_pickup else 1
        for i, s in enumerate(bars):
            meter = pickup if (n == 0 and pickup > 0) else base
            for at in sorted(changes):
                if n >= at:
                    meter = changes[at]
            out.append({"meter": meter, "text": s.strip(),
                        "beats": bar_beats(s), "last": i == len(bars) - 1,
                        "bar": n})
            n += 1
    return out


def violations(lines):
    """非末小节不守恒的项；最后一小节按惯例允许尾上休止不满。"""
    return [b for b in score_bars(lines)
            if not b["last"] and
            (b["beats"] is None or abs(b["beats"] - b["meter"]) > 0.01)]


def all_bar_beats(lines):
    return [(b["bar"], b["beats"], b["meter"]) for b in score_bars(lines)]


def bad_bars(lines):
    """含末小节在内，所有印出来与拍号不符的小节（末小节按惯例可不满）。"""
    return [(b["bar"], b["beats"], b["meter"], b["last"])
            for b in score_bars(lines)
            if b["beats"] is not None and abs(b["beats"] - b["meter"]) > 0.01]


def render_lines(exe, mid, td, tag):
    out = os.path.join(td, tag + ".txt")
    subprocess.run([exe, mid, "--output", out], capture_output=True,
                   timeout=120, check=True)
    return open(out, encoding="utf-8", errors="replace").read().splitlines()


def score_tokens(lines):
    """把谱面行拆成 token 序列，'|' 作为小节边界一并保留。"""
    toks = []
    seen_track = False
    for l in lines:
        if re.search(r"^\[[^\]]*\] \(\d+ 音符", l):
            seen_track = True
            continue
        if seen_track and "|" in l:
            toks.extend(l.split())
    return toks


def is_rest_token(tok):
    return bool(re.match(r"^(0|-)[^\s]*$", tok))


def rest_insert_only(old_lines, new_lines):
    """结构性约束：新版只在旧版里插入休止符/续写 token，不删不改任何旧 token。

    返回 (ok, 说明)。修「休止符按最近档记」只会把 `0. 4=` 变成
    `0. -= 4=`，正是纯插入；任何音符 token 的增删改都说明修错了地方。
    """
    a, b = score_tokens(old_lines), score_tokens(new_lines)
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    inserted = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if tag != "insert":
            return False, "出现删除或替换: %s %r -> %r" % (tag, a[i1:i2], b[j1:j2])
        inserted.extend(b[j1:j2])
    if not inserted:
        return False, "没有插入任何休止续写 token"
    bad = [t for t in inserted if not is_rest_token(t)]
    if bad:
        return False, "插入了非休止 token: %r" % bad[:4]
    return True, "插入 %d 个休止续写 token: %s" % (len(inserted), " ".join(inserted))


def audit_fixtures(exe):
    bad = []
    for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
        with tempfile.TemporaryDirectory() as td:
            lines = render_lines(exe, p, td, "new")
        for b in violations(lines):
            bad.append((os.path.basename(p), b["bar"], b["beats"], b["meter"],
                        b["text"][:46]))
    return bad


def structural_regress(old_exe, new_exe):
    changed, errors = [], []
    with tempfile.TemporaryDirectory() as td:
        for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
            name = os.path.basename(p)
            new_lines = render_lines(new_exe, p, td, "new")
            old_lines = render_lines(old_exe, p, td, "old")
            nv = violations(new_lines)
            if nv:
                errors.append("%s: 新版仍有 %d 处非末小节拍数不守恒" % (name, len(nv)))
            if new_lines == old_lines:
                continue
            changed.append(name)
            ok, why = rest_insert_only(old_lines, new_lines)
            if not ok:
                errors.append("%s: %s" % (name, why))
            # 净效果必须是不守恒小节严格减少；末小节本来允许不满，所以
            # 比较的是「含末小节在内的全部小节」，而不是只看 violations。
            ob, nb = bad_bars(old_lines), bad_bars(new_lines)
            if not (len(nb) < len(ob) and all(x in ob for x in nb)):
                errors.append("%s: 不守恒小节没有严格减少 (旧 %d / 新 %d)"
                              % (name, len(ob), len(nb)))
    return changed, errors


def self_test():
    n = 0
    failed = []

    def ck(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    # bar_beats：基础时值、附点、延续线、和弦、三连音、休止续写
    cases = [
        ("1 2 3 4", 4.0), ("1_ 2_ 3_ 4_", 2.0), ("1. 2_ 3_ 4_", 3.0),
        ("1 - -", 3.0), ("0 - - -", 4.0), ("1= 0. -= 2= 0 -= 3_", 4.0),
        ("0. ", 1.5), ("-_.", 0.75), ("0%%", 0.125), ("1=.", 0.375),
        ("1,+3,+5,", 1.0), ("3(1 2 3 )", 1.0), ("[C] 1 2 [Am] 3 4", 4.0),
        ("(3)5 1 2 3", 4.0),
    ]
    for text, want in cases:
        got = bar_beats(text)
        ck(got is not None and abs(got - want) < 1e-6,
           "bar_beats(%r) = %r, 期望 %r" % (text, got, want))
    ck(bar_beats("1?? 2") is None, "无法识别的 token 必须返回 None")

    # parse_meters：基础拍号、变拍号、弱起
    base, ch, pickup, has = parse_meters(["拍号=3/4"])
    ck(base == 3.0 and not ch and not has, "3/4 基础拍号")
    base, ch, pickup, has = parse_meters(["4/4 -> 3/4(第3小节)"])
    ck(base == 4.0 and ch == {3: 3.0}, "变拍号表解析")
    base, ch, pickup, has = parse_meters(["弱起 2 拍"])
    ck(has and pickup == 2.0, "弱起拍数解析")

    # rest_insert_only：只允许插入休止续写 token
    head = ["[Vocal] (7 音符)"]
    ok, _ = rest_insert_only(head + ["3= 0. 4= 0 5_  |"],
                             head + ["3= 0. -= 4= 0 -= 5_  |"])
    ck(ok, "纯插入休止续写 token 应通过")
    ok, _ = rest_insert_only(head + ["1 2 3 4  |"], head + ["1 2 3 4 5  |"])
    ck(not ok, "插入音符 token 应拒绝")
    ok, _ = rest_insert_only(head + ["1 2 3 4  |"], head + ["1 2 4  |"])
    ck(not ok, "删除旧 token 应拒绝")
    ok, _ = rest_insert_only(head + ["1 2 3 4  |"], head + ["1 2 3 5  |"])
    ck(not ok, "替换旧 token 应拒绝")
    ok, _ = rest_insert_only(head + ["1 2 3 4  |"], head + ["1 2 3 4  |"])
    ck(not ok, "没有插入任何 token 应拒绝")

    # score_bars / violations：末小节豁免、非末小节必须守恒
    lines = ["拍号=4/4", "[Vocal] (7 音符)", "1 2 3 4  |", "1 2 3  |"]
    bars = score_bars(lines)
    ck(len(bars) == 2 and bars[0]["bar"] == 1 and bars[1]["last"],
       "score_bars 小节编号与末小节标记")
    ck(violations(lines) == [], "末小节短一拍按惯例豁免")
    lines2 = ["拍号=4/4", "[Vocal] (7 音符)", "1 2 3  |", "1 2 3 4  |"]
    ck(len(violations(lines2)) == 1, "非末小节不守恒必须被抓到")

    print("check_bar_beats self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 0 if not failed else 1


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if "--regress" in args:
        old_exe = args[args.index("--regress") + 1]
        changed, errors = structural_regress(old_exe, exe)
        print("结构性零回归：%d 份素材的文本谱按预期变化（仅插入休止续写）"
              % len(changed))
        for c in changed:
            print("  ", c)
        for e in errors:
            print("  [FAIL]", e)
        return 0 if not errors else 1
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    bad = audit_fixtures(exe)
    if bad:
        print("文本谱拍数不守恒：%d 处" % len(bad))
        for name, bar, beats, meter, text in bad[:20]:
            print("  %s 第%d小节 印出 %s 拍 / 拍号 %s: %s" %
                  (name, bar, beats, meter, text))
        return 1
    print("文本谱拍数守恒：全部 fixture 0 处不守恒")
    return 0


if __name__ == "__main__":
    sys.exit(main())

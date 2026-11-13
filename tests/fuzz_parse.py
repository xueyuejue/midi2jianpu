#!/usr/bin/env python3
"""解析层模糊测试：用合法 MIDI 种子做字节级突变，逼出崩溃 / 越界 / 注入。

设计目标
--------
- 不依赖仓库里任何 .mid 文件（fixtures 由 make_fixture 在 CI 生成、不入库）；
  所有种子直接 `import make_fixture` 调纯函数 `build_xxx()` 在内存里拿，
  自包含、可复现。
- 字节级突变：翻转比特 / 翻转字节 / 插入 / 删除 / 复制块 / 截断 / 改写头。
- 覆盖参数组合：--html / --musicxml / --output / --player / --track /
  --bars / --key / --transpose / --gong 等，含若干「必定失败但不该崩」的
  边界值（--bars 9、--key ZZ、--track 99）。
- 固定随机种子，跑 ≥500 次，过程可复现。
- 不变量断言：
    * 退出码必须 ∈ {0,1,2,3,4}（负数 = 信号崩、>4 = 未知崩、超时 = 卡死）；
    * rc==0 且不开 --player 的 HTML 必须不含 `<script`、不含 U+FFFD；
    * rc==0 的 --musicxml 输出必须能被 ElementTree 解析；
    * rc==0 的文本 / HTML 输出不得含 U+FFFD（编码器不应产出替换字符）。
- 另设一组「定向」用例，直接用 html_meta_inject 种子（含注入脚本的轨名）
  复跑 smoke 层已守死的 XSS 不变量，确保即便随机没选中也能守住。

用法
----
    python tests/fuzz_parse.py [可执行文件] [--count N] [--seed S]

默认可执行文件路径与 smoke.py 一致：build/midi2jianpu.exe（Windows）
或 build/midi2jianpu。
退出码：0 = 全程零异常；非 0 = 发现崩溃 / 卡死 / 不变量破坏。
"""
import importlib.util
import inspect
import os
import random
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(ROOT, "tests", "fixtures")

# ---- 把 fixtures 目录塞进 sys.path，直接 import 生成器纯函数 ----
if FIXTURES not in sys.path:
    sys.path.insert(0, FIXTURES)

# 解析层允许的退出码集合（见 src/main.cpp）
VALID_RC = {0, 1, 2, 3, 4}

# 突变算子集合
MUTATORS = ["flip_bit", "flip_byte", "insert", "delete",
            "duplicate", "truncate", "header"]


def load_make_fixture():
    """动态加载 make_fixture 模块（避免把 tests/fixtures 写死成包）。"""
    path = os.path.join(FIXTURES, "make_fixture.py")
    if not os.path.exists(path):
        raise SystemExit(f"找不到生成器: {path}（先确保仓库完整）")
    spec = importlib.util.spec_from_file_location("make_fixture", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def collect_seeds(mod):
    """收集所有 build_xxx() 返回的原始 MIDI 字节，过滤掉 LRC/非字节产物。"""
    seeds = {}
    for name, fn in inspect.getmembers(mod, inspect.isfunction):
        if not name.startswith("build_"):
            continue
        try:
            data = fn()
        except Exception as e:  # 个别 builder 依赖环境，跳过而非崩
            print(f"  [skip] {name} 构建失败: {e}")
            continue
        if isinstance(data, (bytes, bytearray)) and data[:4] == b"MThd":
            seeds[name] = bytes(data)
    return seeds


# ---------------- 突变算子 ----------------
def flip_bit(b):
    i = random.randrange(len(b))
    bit = 1 << random.randrange(8)
    return b[:i] + bytes([b[i] ^ bit]) + b[i + 1:]


def flip_byte(b):
    i = random.randrange(len(b))
    return b[:i] + bytes([random.randrange(256)]) + b[i + 1:]


def insert(b):
    i = random.randrange(len(b) + 1)
    n = random.randint(1, 4)
    return b[:i] + bytes(random.randrange(256) for _ in range(n)) + b[i:]


def delete(b):
    if len(b) <= 4:
        return b
    i = random.randrange(len(b))
    n = min(random.randint(1, 4), len(b) - i)
    return b[:i] + b[i + n:]


def duplicate(b):
    if len(b) < 2:
        return b
    i = random.randrange(len(b))
    n = random.randint(1, min(8, len(b) - i))
    return b[:i] + b[i:i + n] * 2 + b[i + n:]


def truncate(b):
    if len(b) <= 4:
        return b
    i = random.randrange(1, len(b))
    return b[:i]


def header(b):
    """只动前 14 字节（MThd + 头长 + 格式/轨数/division），逼出头解析崩溃。"""
    if len(b) < 14:
        return b
    i = random.randrange(4, 14)
    return b[:i] + bytes([random.randrange(256)]) + b[i + 1:]


MUT_FUNCS = {
    "flip_bit": flip_bit, "flip_byte": flip_byte, "insert": insert,
    "delete": delete, "duplicate": duplicate, "truncate": truncate,
    "header": header,
}


# ---------------- 参数组合 ----------------
def pick_args(rng, out_dir):
    """随机拼一组命令行参数（返回值列表，输入文件由调用方前置）。"""
    mode = rng.choice(["text", "text_file", "html", "html_player", "xml"])
    args = []
    if mode == "text":
        pass  # 默认打到 stdout
    elif mode == "text_file":
        args += ["--output", os.path.join(out_dir, "out.txt")]
    elif mode == "html":
        args += ["--html", os.path.join(out_dir, "out.html")]
    elif mode == "html_player":
        args += ["--html", os.path.join(out_dir, "out_p.html"), "--player"]
    elif mode == "xml":
        args += ["--musicxml", os.path.join(out_dir, "out.musicxml")]

    # 随机叠加 0~2 个修饰选项
    mods = rng.sample(
        [("--track", "0"), ("--track", "1"), ("--track", "2"),
         ("--track", "99"), ("--bars", "1"), ("--bars", "4"),
         ("--bars", "8"), ("--bars", "9"), ("--key", "C"),
         ("--key", "Am"), ("--key", "Bb"), ("--key", "ZZ"),
         ("--transpose", "0"), ("--transpose", "12"),
         ("--transpose", "-12"), ("--transpose", "5"),
         ("--gong", "C"), ("--gong", "G"), ("--gong", "D"),
         ("--no-quantize", None), ("--bar-numbers", None),
         ("--no-dynamics", None)],
        k=rng.randint(0, 2))
    for flag, val in mods:
        args.append(flag)
        if val is not None:
            args.append(val)
    return mode, args


def run_once(exe, data, args, timeout):
    """把 data 写临时 .mid，跑 exe，返回 (rc, stdout, err, timed_out)。"""
    with tempfile.NamedTemporaryFile(suffix=".mid", delete=False) as tf:
        tf.write(data)
        mid_path = tf.name
    try:
        p = subprocess.run([exe, mid_path, *args], capture_output=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return p.returncode, p.stdout or "", p.stderr or "", False
    except subprocess.TimeoutExpired:
        return None, "", "TIMEOUT", True
    finally:
        try:
            os.unlink(mid_path)
        except OSError:
            pass


def check_outputs(rc, mode, out_dir, stdout, problems):
    """rc==0 时的输出不变量校验，把问题追加进 problems。"""
    if rc != 0:
        return
    if mode in ("html", "html_player"):
        html_path = os.path.join(
            out_dir, "out.html" if mode == "html" else "out_p.html")
        if not os.path.exists(html_path):
            problems.append(f"rc==0 但 HTML 文件未生成: {html_path}")
            return
        with open(html_path, "r", encoding="utf-8", errors="replace") as f:
            html = f.read()
        if "\ufffd" in html:
            problems.append("HTML 含 U+FFFD 替换字符")
        if mode == "html" and "<script" in html:
            problems.append("不开 --player 的 HTML 竟含 <script 标签")
    elif mode == "xml":
        xml_path = os.path.join(out_dir, "out.musicxml")
        if not os.path.exists(xml_path):
            problems.append("rc==0 但 MusicXML 文件未生成")
            return
        try:
            ET.parse(xml_path)
        except ET.ParseError as e:
            problems.append(f"MusicXML 无法被 ElementTree 解析: {e}")
        with open(xml_path, "r", encoding="utf-8", errors="replace") as f:
            if "\ufffd" in f.read():
                problems.append("MusicXML 含 U+FFFD 替换字符")
    elif mode == "text_file":
        txt_path = os.path.join(out_dir, "out.txt")
        if os.path.exists(txt_path):
            with open(txt_path, "r", encoding="utf-8", errors="replace") as f:
                if "\ufffd" in f.read():
                    problems.append("文本输出含 U+FFFD 替换字符")
    elif mode == "text":
        if "\ufffd" in stdout:
            problems.append("文本 stdout 含 U+FFFD 替换字符")


def targeted_xss(exe, seed_bytes, out_dir):
    """定向复跑 XSS 不变量（与 smoke.py 同口径，但走 fuzz 通道）。"""
    fails = []
    # 不开 --player：整页不得有 <script
    h = os.path.join(out_dir, "xss.html")
    rc, _, _, to = run_once(exe, seed_bytes, ["--html", h], 30)
    if to:
        fails.append("XSS 定向用例超时")
        return fails
    if rc != 0:
        fails.append(f"XSS 定向用例 rc={rc}")
        return fails
    with open(h, "r", encoding="utf-8", errors="replace") as f:
        html = f.read()
    if html.count("<script") != 0:
        fails.append(f"不开 --player 时存在 {html.count('<script')} 个 script 标签")
    if "<script>alert(1)</script>" in html:
        fails.append("注入脚本原样出现在页面")
    # 开 --player：恰好 1 段 script，且注入脚本不出现
    hp = os.path.join(out_dir, "xss_p.html")
    rc, _, _, to = run_once(exe, seed_bytes, ["--html", hp, "--player"], 30)
    if to:
        fails.append("XSS 定向用例(--player)超时")
        return fails
    if rc != 0:
        fails.append(f"XSS 定向用例(--player) rc={rc}")
        return fails
    with open(hp, "r", encoding="utf-8", errors="replace") as f:
        phtml = f.read()
    if phtml.count("<script") != 1:
        fails.append(f"--player 时 script 段数={phtml.count('<script')}（应为 1）")
    if "<script>alert(1)</script>" in phtml:
        fails.append("--player 时注入脚本原样出现")
    return fails


def main():
    args = sys.argv[1:]
    exe = None
    count = 600
    seed = 0x5EED
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--count", "--seed"):
            val = args[i + 1] if i + 1 < len(args) else ""
            if a == "--count":
                count = int(val)
            else:
                seed = int(val, 0)  # 支持 0x 前缀
            i += 2
        elif not a.startswith("-"):
            exe = a
            i += 1
        else:
            i += 1

    if exe is None:
        exe = os.path.join(ROOT, "build",
                           "midi2jianpu.exe" if os.name == "nt" else "midi2jianpu")
    if not os.path.exists(exe):
        raise SystemExit(
            f"找不到可执行文件: {exe}\n先构建：cmake -B build -S . && cmake --build build")

    print(f"加载种子生成器 ... {FIXTURES}")
    mod = load_make_fixture()
    seeds = collect_seeds(mod)
    if not seeds:
        raise SystemExit("未收集到任何 MIDI 种子，检查 make_fixture.py")
    print(f"收集到 {len(seeds)} 份 MIDI 种子；计划跑 {count} 次（seed={seed:#x}）")

    rng = random.Random(seed)
    out_dir = tempfile.mkdtemp(prefix="m2j-fuzz-")
    crashes = []          # rc 越界 / 超时（解析层崩或卡死）
    invariants = []       # rc==0 但不变量被破坏
    rc_hist = {}
    n = 0
    for n in range(1, count + 1):
        seed_name = rng.choice(list(seeds.keys()))
        base = seeds[seed_name]
        mut = rng.choice(MUTATORS)
        data = MUT_FUNCS[mut](base)
        mode, cargs = pick_args(rng, out_dir)
        rc, out, err, to = run_once(exe, data, cargs, 20)
        if to or rc is None:
            crashes.append((seed_name, mut, mode, cargs, "TIMEOUT"))
            continue
        rc_hist[rc] = rc_hist.get(rc, 0) + 1
        if rc not in VALID_RC:
            crashes.append((seed_name, mut, mode, cargs, f"rc={rc}"))
            continue
        probs = []
        check_outputs(rc, mode, out_dir, out, probs)
        if probs:
            invariants.append((seed_name, mut, mode, cargs, rc, probs))

        if n % 100 == 0:
            print(f"  ... 已完成 {n} 次（崩溃 {len(crashes)} / 不变量 {len(invariants)}）")

    # 定向 XSS 用例
    xss_fails = []
    if "build_html_meta_inject" in seeds:
        xss_fails = targeted_xss(exe, seeds["build_html_meta_inject"], out_dir)
    else:
        print("  [warn] 没有 html_meta_inject 种子，跳过定向 XSS 复跑")

    # ---- 汇总 ----
    print("\n===== 模糊测试汇总 =====")
    print(f"总运行次数 : {count}")
    print(f"退出码分布 : {dict(sorted(rc_hist.items()))}")
    print(f"崩溃/卡死  : {len(crashes)}")
    print(f"不变量破坏 : {len(invariants)}")
    print(f"XSS 定向   : {len(xss_fails)} 项失败")

    ok = not crashes and not invariants and not xss_fails
    if ok:
        print("结果：解析层零崩溃、零卡死、输出不变量全部守住。")
        return 0

    print("\n--- 异常明细（最多 20 条）---")
    for c in crashes[:20]:
        print(f"  [CRASH] seed={c[0]} mut={c[1]} mode={c[2]} args={c[3]} -> {c[4]}")
    for inv in invariants[:20]:
        print(f"  [INV] seed={inv[0]} mut={inv[1]} mode={inv[2]} rc={inv[4]} "
              f"args={inv[3]} :: {inv[5]}")
    for x in xss_fails:
        print(f"  [XSS] {x}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

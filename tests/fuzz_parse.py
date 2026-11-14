#!/usr/bin/env python3
"""解析层模糊测试：用合法 MIDI 种子做突变，逼出崩溃 / 越界 / 注入。

设计目标
--------
- 不依赖仓库里任何 .mid 文件（fixtures 由 make_fixture 在 CI 生成、不入库）；
  所有种子直接 `import make_fixture` 调纯函数 `build_xxx()` 在内存里拿，
  自包含、可复现。
- **两层突变**：
    * 7 个通用算子（字节级）：翻转比特 / 翻转字节 / 插入 / 删除 / 复制块 /
      截断 / 改写文件头；
    * 5 个**语法感知**算子（字段级）：改写 SysEx 长度字段 / 删或提前插 F7
      终结符 / 改写 RPN(NRPN) 数据字节 / 在 meta 事件处插入超长 VarLen /
      改写 MTrk chunk 长度。随机翻字节的命中率约为 1/文件长度，这 5 类
      「解析层有专门代码」的字段几乎打不中（103 份种子里含 SysEx 仅 18 份、
      含 RPN/CC64-65 仅 6 份），必须定点打。
- **定点选种子**：语法感知算子优先从含对应字段的种子池里挑（`MUT_SEED_HINTS`），
  找不到才退回全体，避免「打空气」。
- **边界选项矩阵（HOSTILE_ARGS）**：每个突变**强制**跑 5 组「必定失败但不该崩」
  的组合：`--track 99` / `--bars 9` / `--key ZZ` / `--transpose 999`（v1.96）/
  `--pickup 1 --no-pickup`（v1.97 矛盾开关），断言 rc ∈
  {0,1,2,3,4}（不崩、不卡死）；这比 v1.93 纯概率式 `pick_args` 抽修饰更
  能确定性地守住选项校验路径（一个回归会立刻在第一例突变上暴露，而非等
  概率抽中）。
- 随机配置（pick_args）：--html / --musicxml / --output / --player / --track /
  --bars / --key / --transpose / --gong 等，再叠加 0~2 个修饰，保证输出
  形态广度；`--player` 运行时额外校验整页恰好 1 段 `<script>`、注入脚本不出现。
- **多种子**：`--seed 0x5EED`（单种子）或 `--seeds 0x5EED,1,42`（逗号分隔，
  每个种子各跑 `--count` 次）；每个种子独立 `random.Random(seed)` 实例，
  逐种子完全可复现。
- 不变量断言：
    * 退出码必须 ∈ {0,1,2,3,4}（负数 = 信号崩、>4 = 未知崩、超时 = 卡死）；
    * rc==0 且不开 --player 的 HTML 必须不含 `<script`、不含 U+FFFD；
    * rc==0 的 --musicxml 输出必须能被 ElementTree 解析；
    * rc==0 的文本 / HTML 输出不得含 U+FFFD（编码器不应产出替换字符）。
- 另设一组「定向」用例，直接用 html_meta_inject 种子（含注入脚本的轨名）
  复跑 smoke 层已守死的 XSS 不变量，覆盖 HTML(不开/开 player) 与
  MusicXML 三条通道——确保即便随机突变没命中注入路径，这三条输出也
  绝不会把恶意轨名原样写出（v1.95.0 起 MusicXML 通道也纳入）。

用法
----
    python tests/fuzz_parse.py [可执行文件] [--count N] [--seed S | --seeds a,b,c]
    python tests/fuzz_parse.py --self-test

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

# 突变算子集合（前 7 个通用，后 5 个语法感知）
MUTATORS = ["flip_bit", "flip_byte", "insert", "delete",
            "duplicate", "truncate", "header",
            "sysex_len", "sysex_term", "rpn_param", "varlen_meta",
            "chunk_len"]

# 语法感知算子优先挑选含对应字段的种子，避免「打空气」
MUT_SEED_HINTS = {
    "sysex_len": b"\xf0",
    "sysex_term": b"\xf0",
    "rpn_param": b"\xb0\x65",
    "varlen_meta": b"\xff\x51",   # FF 51 = set tempo，绝大多数种子都有
    "chunk_len": b"MTrk",
}

# 强制敌对边界选项矩阵：每个突变都跑这组，确定性地覆盖「应报错但不该崩」的
# 选项校验路径（v1.93 之前只是 pick_args 里概率式抽到，覆盖率不稳）。
HOSTILE_ARGS = [
    ["--track", "99"],                      # 越界轨 → 期望 rc=4（若解析成功）
    ["--bars", "9"],                        # 越界小节数 → 期望 rc=1
    ["--key", "ZZ"],                        # 非法调名 → 期望 rc=1
    ["--transpose", "999"],                 # 越界移调 → 期望 rc=1（v1.96.0 起有上限）
    ["--pickup", "1", "--no-pickup"],       # 矛盾弱起开关 → 期望 rc=1（v1.97.0 起拒绝）
]


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


# ---------------- 结构化定位（语法感知突变的底座） ----------------
def _read_vlq(b, pos):
    """读一个 VarLen 值，返回 (value, next_pos)；超 4 字节视为非法返回 (None, pos)。"""
    val = 0
    start = pos
    while pos < len(b) and pos - start < 4:
        c = b[pos]
        val = (val << 7) | (c & 0x7F)
        pos += 1
        if not c & 0x80:
            return val, pos
    return None, start


def _write_vlq(v):
    """把数值写成 VarLen（最多 4 字节，超出按 28 位截断）。"""
    v &= 0x0FFFFFFF
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def _positions(b, needle):
    """返回 needle 在 b 中出现的全部位置。"""
    hits = []
    i = b.find(needle)
    while i >= 0:
        hits.append(i)
        i = b.find(needle, i + 1)
    return hits


# ---------------- 突变算子 ----------------
# 注意：所有算子都用调用方传入的「已用固定种子初始化的 rng」取随机数，
# 不能碰全局 random 模块——否则每次进程启动的全局种子不同，突变结果不可复现，
# CI 会随机红绿。固定 seed 才能让「≥500 次零崩溃」这个断言稳定成立。
def flip_bit(b, rng):
    i = rng.randrange(len(b))
    bit = 1 << rng.randrange(8)
    return b[:i] + bytes([b[i] ^ bit]) + b[i + 1:]


def flip_byte(b, rng):
    i = rng.randrange(len(b))
    return b[:i] + bytes([rng.randrange(256)]) + b[i + 1:]


def insert(b, rng):
    i = rng.randrange(len(b) + 1)
    n = rng.randint(1, 4)
    return b[:i] + bytes(rng.randrange(256) for _ in range(n)) + b[i:]


def delete(b, rng):
    if len(b) <= 4:
        return b
    i = rng.randrange(len(b))
    n = min(rng.randint(1, 4), len(b) - i)
    return b[:i] + b[i + n:]


def duplicate(b, rng):
    if len(b) < 2:
        return b
    i = rng.randrange(len(b))
    n = rng.randint(1, min(8, len(b) - i))
    return b[:i] + b[i:i + n] * 2 + b[i + n:]


def truncate(b, rng):
    if len(b) <= 4:
        return b
    i = rng.randrange(1, len(b))
    return b[:i]


def header(b, rng):
    """只动前 14 字节（MThd + 头长 + 格式/轨数/division），逼出头解析崩溃。"""
    if len(b) < 14:
        return b
    i = rng.randrange(4, 14)
    return b[:i] + bytes([rng.randrange(256)]) + b[i + 1:]


def sysex_len(b, rng):
    """改写 SysEx 长度字段：0 / 极端大值 / 与实际 payload 不符。

    解析层要为 SysEx 预留缓冲区并按长度推进，长度撒谎是它的直接输入。
    """
    hits = _positions(b, b"\xf0")
    if not hits:
        return flip_byte(b, rng)
    i = rng.choice(hits) + 1
    val, nxt = _read_vlq(b, i)
    if val is None:
        return flip_byte(b, rng)
    pick = rng.randrange(3)
    if pick == 0:
        new = b"\x00"
    elif pick == 1:
        new = b"\x7f\x7f\x7f\x7f"            # 0x0FFFFFFF，远超文件长度
    else:
        new = _write_vlq(max(0, len(b) - nxt + rng.randint(-4, 8)))
    return b[:i] + new + b[nxt:]


def sysex_term(b, rng):
    """删掉 F7 终结符，或提前插一个 F7 把 payload 截断。"""
    spans = []
    for s in _positions(b, b"\xf0"):
        e = b.find(b"\xf7", s + 1)
        if e > s:
            spans.append((s, e))
    if not spans:
        return flip_byte(b, rng)
    s, e = rng.choice(spans)
    if rng.randrange(2):
        return b[:e] + b[e + 1:]              # SysEx 无终结符，拖到轨尾
    mid = rng.randint(s + 1, e) if e > s + 1 else e
    return b[:mid] + b"\xf7" + b[mid:]        # 提前终结，多余字节留轨内


def rpn_param(b, rng):
    """改写 RPN/NRPN 参数号与 Data Entry 数据字节（含非法 0xFF 数据字节）。"""
    hits = []
    for i in range(len(b) - 2):
        if 0xB0 <= b[i] <= 0xBF and b[i + 1] in (0x64, 0x65, 0x06, 0x26):
            hits.append(i + 2)
    if not hits:
        return flip_byte(b, rng)
    i = rng.choice(hits)
    return b[:i] + bytes([rng.choice([0x00, 0x7F, 0xFF, 0x40])]) + b[i + 1:]


def varlen_meta(b, rng):
    """在 meta 事件类型字节后插入超长 / 溢出 VarLen 长度字段。"""
    hits = []
    for i in _positions(b, b"\xff"):
        if i + 1 < len(b):
            hits.append(i + 1)
    if not hits:
        return flip_byte(b, rng)
    i = rng.choice(hits)
    if rng.randrange(2):
        return b[:i] + b"\x80\x80\x80\x80\x80" + b[i:]   # 5 字节 VLQ，超 int32 位移
    return b[:i] + b"\xff\xff\xff\xff" + b[i:]           # 4 字节但值巨大


def chunk_len(b, rng):
    """改写 MTrk 的 4 字节长度：0 / 巨大 / 与实际不符，逼边界校正逻辑。"""
    hits = _positions(b, b"MTrk")
    if not hits:
        return flip_byte(b, rng)
    i = rng.choice(hits) + 4
    if i + 4 > len(b):
        return flip_byte(b, rng)
    val = rng.choice([0, 0xFFFFFFFF, max(0, len(b) - i - 4), 12, 0x7FFFFFFF])
    return b[:i] + val.to_bytes(4, "big") + b[i + 4:]


MUT_FUNCS = {
    "flip_bit": flip_bit, "flip_byte": flip_byte, "insert": insert,
    "delete": delete, "duplicate": duplicate, "truncate": truncate,
    "header": header, "sysex_len": sysex_len, "sysex_term": sysex_term,
    "rpn_param": rpn_param, "varlen_meta": varlen_meta, "chunk_len": chunk_len,
}


def pick_seed(rng, seeds, mut):
    """语法感知算子优先从含对应字段的种子池里挑，避免「打空气」。"""
    hint = MUT_SEED_HINTS.get(mut)
    if hint:
        cand = [k for k, v in seeds.items() if hint in v]
        if cand:
            return rng.choice(cand)
    return rng.choice(list(seeds.keys()))


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


def _player_script_ok(html):
    """--player 时整页必须恰好 1 段 <script>（播放器本体），且注入脚本不得原样出现。

    返回 (ok, msg)：ok 为 False 时 msg 描述问题。供 check_outputs 与
    targeted_xss 共用，确保随机 player 运行与定向用例守同一道口。
    """
    n = html.count("<script")
    if n != 1:
        return False, f"--player 时 script 段数={n}（应为 1）"
    if "<script>alert(1)</script>" in html:
        return False, "--player 时注入脚本原样出现"
    return True, ""


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
        if mode == "html_player":
            ok, msg = _player_script_ok(html)
            if not ok:
                problems.append(msg)
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
    """定向复跑 XSS 不变量（与 smoke.py 同口径，但走 fuzz 通道）。

    v1.95.0 起覆盖三条输出通道：HTML(不开 player) / HTML(--player) /
    MusicXML。每条通道独立校验，任一通道超时即终止（读不到产物），其余
    通道的失败只记账不中断——这样 MusicXML 的转义守护不会被 HTML 的
    偶发异常静默跳过。
    """
    fails = []
    # 不开 --player：整页不得有 <script
    h = os.path.join(out_dir, "xss.html")
    rc, _, _, to = run_once(exe, seed_bytes, ["--html", h], 30)
    if to:
        fails.append("XSS 定向用例超时")
        return fails
    if rc != 0:
        fails.append(f"XSS 定向用例 rc={rc}")
    else:
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
    else:
        with open(hp, "r", encoding="utf-8", errors="replace") as f:
            phtml = f.read()
        ok, msg = _player_script_ok(phtml)
        if not ok:
            fails.append(msg)
    # MusicXML 通道：恶意轨名同样必须转义，不得出现裸 <script>，且整份良构
    xm = os.path.join(out_dir, "xss.musicxml")
    rc, _, _, to = run_once(exe, seed_bytes, ["--musicxml", xm], 30)
    if to:
        fails.append("XSS 定向用例(--musicxml)超时")
        return fails
    if rc != 0:
        fails.append(f"XSS 定向用例(--musicxml) rc={rc}")
        return fails
    try:
        import xml.etree.ElementTree as ET
        with open(xm, "r", encoding="utf-8") as f:
            raw = f.read()
        ET.fromstring(raw)          # 非良构会抛异常
    except Exception as e:          # noqa: BLE001 - 良构性是断言目标
        fails.append("XSS 定向用例 MusicXML 非良构: %s" % e)
        return fails
    if "<script>alert(1)</script>" in raw:
        fails.append("注入脚本原样出现在 MusicXML")
    return fails


# ---------------- 种子列表解析 ----------------
def parse_seeds(s):
    """解析 `0x5EED,1,42` 这类逗号分隔的种子列表（支持 0x 前缀）。"""
    out = []
    for part in s.replace(" ", "").split(","):
        if part:
            out.append(int(part, 0))
    return out


# ---------------- 自测（不依赖 exe） ----------------
SAMPLE = (b"MThd\x00\x00\x00\x06\x00\x00\x01\x00\x01\xe0"
          b"MTrk\x00\x00\x00\x1e"
          b"\x00\xff\x51\x03\x07\xa1\x20"
          b"\x00\xf0\x05\x7e\x7f\x09\x01\xf7"
          b"\x00\xb0\x65\x00\xb0\x64\x00\xb0\x06\x0c"
          b"\x00\x90\x3c\x64\x83\x60\x80\x3c\x00")


def self_test():
    fails = []
    items = []

    def st(cond, name):
        items.append(name)
        if not cond:
            fails.append(name)

    st(VALID_RC == {0, 1, 2, 3, 4}, "合法退出码集合为 {0,1,2,3,4}")
    st(MUTATORS == list(MUT_FUNCS.keys()), "算子清单与实现表一一对应（12 个）")
    st(len(MUTATORS) == 12, "算子总数为 12（7 通用 + 5 语法感知）")

    # VarLen 编解码
    for v in (0x00, 0x40, 0x7F, 0x80, 0x3FFF, 0x0FFFFFFF):
        enc = _write_vlq(v)
        dec, nxt = _read_vlq(enc, 0)
        st(dec == v and nxt == len(enc), f"VarLen 往返 {v:#x}")
    dec, nxt = _read_vlq(b"\x80\x80\x80\x80\x01", 0)
    st(dec is None and nxt == 0, "5 字节 VarLen 判为非法")
    st(_write_vlq(0x0FFFFFFF) == b"\xff\xff\xff\x7f", "最大 VarLen 编码为 FF FF FF 7F")

    # 定位函数
    st(_positions(SAMPLE, b"\xf0") and len(_positions(SAMPLE, b"\xf0")) == 1,
       "F0 定位命中 1 处")
    st(len(_positions(SAMPLE, b"MTrk")) == 1, "MTrk 定位命中 1 处")

    # 每个算子对样例数据都能产出 bytes，且固定种子下完全可复现
    for name in MUTATORS:
        fn = MUT_FUNCS[name]
        a = fn(SAMPLE, random.Random(7))
        b = fn(SAMPLE, random.Random(7))
        st(isinstance(a, bytes) and a == b, f"算子 {name} 可复现且返回 bytes")

    # 5 个语法感知算子必须真的改到目标字段（多次尝试中至少一次生效）
    for name in ("sysex_len", "sysex_term", "rpn_param",
                 "varlen_meta", "chunk_len"):
        fn = MUT_FUNCS[name]
        hit = any(fn(SAMPLE, random.Random(k)) != SAMPLE for k in range(1, 21))
        st(hit, f"语法感知算子 {name} 在样例上确实改动字节")

    # 找不到目标时退回通用突变，不得抛异常
    plain = b"MThd\x00\x00\x00\x06\x00\x00\x01\x00\x60"
    st(all(isinstance(MUT_FUNCS[n](plain, random.Random(3)), bytes)
           for n in ("sysex_len", "sysex_term", "rpn_param",
                     "varlen_meta", "chunk_len")),
       "无目标字段时 5 个算子均安全退回")

    # 种子列表解析
    st(parse_seeds("0x5EED,1,42") == [0x5EED, 1, 42], "种子列表解析 0x5EED,1,42")
    st(parse_seeds(" 7 , 8 ") == [7, 8], "种子列表容错空格")
    st(parse_seeds("") == [], "空种子串解析为空列表")

    # 定点选种子：语法感知算子必须挑到含对应字段的种子
    pool = {"a": b"MThd" + b"\x00" * 8, "b": SAMPLE}
    picked = {pick_seed(random.Random(k), pool, "sysex_len") for k in range(1, 21)}
    st(picked == {"b"}, "sysex_len 只从含 F0 的种子池里挑")
    st(pick_seed(random.Random(1), pool, "flip_bit") in ("a", "b"),
       "通用算子不受种子池限制")

    # player 脚本不变量辅助函数
    ok, _ = _player_script_ok("<html><script src=x></script></html>")
    st(ok, "player 单段 script 视为合法")
    bad0, m0 = _player_script_ok("<html>no script</html>")
    st((not bad0) and "1" in m0, "player 0 段 script 判非法")
    bad2, _ = _player_script_ok("<s></s><script></script><script></script>")
    st(not bad2, "player 2 段 script 判非法")
    inj, _ = _player_script_ok("<script>alert(1)</script>")
    st(not inj, "player 注入脚本判非法")
    st(bool(HOSTILE_ARGS) and all(a for a in HOSTILE_ARGS),
       "敌对矩阵非空且每项有内容")
    st(any("--no-pickup" in h and "--pickup" in h for h in HOSTILE_ARGS),
       "敌对矩阵含 --pickup / --no-pickup 矛盾组合")

    total = len(items)
    print(f"自测：共 {total} 项，失败 {len(fails)} 项")
    for name in fails:
        print(f"  [FAIL] {name}")
    if not fails:
        print("自测全部通过。")
        return 0
    return 1


def main():
    args = sys.argv[1:]
    exe = None
    count = 600
    seed_list = [0x5EED]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--self-test":
            return self_test()
        if a in ("--count", "--seed", "--seeds"):
            val = args[i + 1] if i + 1 < len(args) else ""
            if a == "--count":
                count = int(val)
            elif a == "--seed":
                seed_list = [int(val, 0)]      # 支持 0x 前缀
            else:
                seed_list = parse_seeds(val)
            i += 2
        elif not a.startswith("-"):
            exe = a
            i += 1
        else:
            i += 1

    if not seed_list:
        raise SystemExit("--seeds 至少给一个种子")
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
    total_plan = count * len(seed_list)
    seeds_desc = ",".join(f"{s:#x}" for s in seed_list)
    print(f"收集到 {len(seeds)} 份 MIDI 种子；计划跑 {total_plan} 次"
          f"（{len(seed_list)} 个种子 × {count}：{seeds_desc}）")

    out_dir = tempfile.mkdtemp(prefix="m2j-fuzz-")
    crashes = []          # rc 越界 / 超时（解析层崩或卡死）
    invariants = []       # rc==0 但不变量被破坏
    rc_hist = {}
    mut_stats = {}
    done = 0
    hostile_done = 0
    for sv in seed_list:
        rng = random.Random(sv)
        for _ in range(count):
            done += 1
            mut = rng.choice(MUTATORS)
            seed_name = pick_seed(rng, seeds, mut)
            data = MUT_FUNCS[mut](seeds[seed_name], rng)
            mode, cargs = pick_args(rng, out_dir)
            mut_stats[mut] = mut_stats.get(mut, 0) + 1
            rc, out, err, to = run_once(exe, data, cargs, 20)
            if to or rc is None:
                crashes.append((sv, seed_name, mut, mode, cargs, "TIMEOUT"))
                continue
            rc_hist[rc] = rc_hist.get(rc, 0) + 1
            if rc not in VALID_RC:
                crashes.append((sv, seed_name, mut, mode, cargs, f"rc={rc}"))
                continue
            probs = []
            check_outputs(rc, mode, out_dir, out, probs)
            if probs:
                invariants.append((sv, seed_name, mut, mode, cargs, rc, probs))

            # 强制敌对边界选项矩阵：每个突变都踩一遍「应报错但不该崩」路径，
            # 确定性地守住选项校验（不再依赖 pick_args 的概率式抽修饰）。
            for hargs in HOSTILE_ARGS:
                hrc, _, _, hto = run_once(exe, data, hargs, 20)
                hostile_done += 1
                if hto or hrc is None or hrc not in VALID_RC:
                    crashes.append((sv, seed_name, mut, "hostile", hargs,
                                    "TIMEOUT" if hto else f"rc={hrc}"))

            if done % 100 == 0:
                print(f"  ... 已完成 {done}/{total_plan} 次"
                      f"（崩溃 {len(crashes)} / 不变量 {len(invariants)}）")

    # 定向 XSS 用例
    xss_fails = []
    if "build_html_meta_inject" in seeds:
        xss_fails = targeted_xss(exe, seeds["build_html_meta_inject"], out_dir)
    else:
        print("  [warn] 没有 html_meta_inject 种子，跳过定向 XSS 复跑")

    # ---- 汇总 ----
    print("\n===== 模糊测试汇总 =====")
    print(f"总运行次数 : {total_plan}")
    print(f"种子       : {seeds_desc}")
    print(f"算子分布   : {dict(sorted(mut_stats.items()))}")
    print(f"退出码分布 : {dict(sorted(rc_hist.items()))}")
    print(f"敌对矩阵运行 : {hostile_done}")
    print(f"崩溃/卡死  : {len(crashes)}")
    print(f"不变量破坏 : {len(invariants)}")
    print(f"XSS 定向   : {len(xss_fails)} 项失败")

    ok = not crashes and not invariants and not xss_fails
    if ok:
        print("结果：解析层零崩溃、零卡死、输出不变量全部守住。")
        return 0

    print("\n--- 异常明细（最多 20 条）---")
    for c in crashes[:20]:
        print(f"  [CRASH] seed={c[0]:#x} fixture={c[1]} mut={c[2]} "
              f"mode={c[3]} args={c[4]} -> {c[5]}")
    for inv in invariants[:20]:
        print(f"  [INV] seed={inv[0]:#x} fixture={inv[1]} mut={inv[2]} "
              f"mode={inv[3]} rc={inv[5]} args={inv[4]} :: {inv[6]}")
    for x in xss_fails:
        print(f"  [XSS] {x}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

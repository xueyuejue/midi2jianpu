#!/usr/bin/env python3
r"""HTML 试听播放器校验（v1.72.0）。

`--player` 给导出的 HTML 内嵌一个 Web Audio 试听播放器：工具条 + tempo 分段表
+ 每个音符列一个 `data-t`，点播放就合成旋律并高亮当前音。这一层最容易出的
问题不是「长得不好看」，而是：

  1. 内嵌的 PLAY JSON 坏了（少逗号 / 音高越界 / 时长 0）——浏览器里一片静音，
     而且控制台报错在手机上根本看不到；
  2. JS 有语法错——整页脚本不执行，按钮点了没反应；
  3. JS 能解析但跑不起来（选中了不存在的 DOM、少写一个变量）——同样点了没反应；
  4. 不带 `--player` 的页面被顺手改了——既有素材的零回归就没了。

所以这里做五层校验：
  A. 结构：playerbar / script / PLAY JSON / CSS 齐全且自洽；
  B. 语义：notes 的时长 > 0、音高 0~127、tick ≥ 0、力度 1~127 且真的导出
    了（v1.73.0 起第 5 位）；tempo 段升序且 us > 0；每个 `data-t` 都能在
    某条 note 的起点里找到（否则高亮永远不亮）；
  C. 运行时：用假的 DOM + 假 AudioContext 在 node 里真跑一遍那段 JS，断言
    ①「排出去的振荡器个数 == PLAY 里的音符数」，②峰值增益集合恰好等于各音
    力度换算出的集合（力度没有真正影响音量就会红），③点击某个音符后从该
    tick 起播、第一个排出的音正是它；
  D. 反向：不带 `--player` 时必须没有 data-t / script / playerbar；
  E. 组合（v1.98.0）：此前 audit 只跑 `--html` / `--html --player` 两种
     flagset，`--bars N` / `--no-dynamics` / `--no-midi-lyrics` 与 `--player`
     叠加后的页面从没有常驻守护。组合页一律要过 A/B 两层结构校验，且
     playerbar 照样在 `<h1>` 之前（sticky 依赖文档流位置）；PLAY 数据按开关
     分两类守：
       · 纯排版/显示开关（`--bars` / `--no-midi-lyrics`）：PLAY JSON 必须与
         基准**逐字节一致**——播放数据只取决于音符与速度；
       · 会改谱面结构的开关：不要求逐字节一致，但要求 **division / tempo
         不变且音符 multiset 守恒**（不许悄悄掉音或多音）。
     `--no-dynamics` 属于后者：它关掉整个 ExpressionMap，倚音不再被识别合并
     （`parts.mid` 的音符列 50 → 55），声部拆分顺序随之改变，PLAY 里的音符
     顺序也变，但音符集合一个不少。`--no-quantize` 同理（改的是时值），
     两者都有意不进「逐字节不变」契约，否则会把合理行为误判成缺陷。

用法：
  python tests/check_html_player.py                # 校验全部 fixture
  python tests/check_html_player.py --self-test    # 纯逻辑自测（不需要 exe）
  python tests/check_html_player.py --exe PATH     # 指定可执行文件
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

PLAY_RE = re.compile(r"var M2J_PLAY = (\{.*?\});", re.S)
TICK_RE = re.compile(r"data-t='(\d+)'")
SCRIPT_RE = re.compile(r"<script>\r?\n(.*?)</script>", re.S)

# v1.98.0 组合矩阵。分两类：纯排版/显示开关要求 PLAY JSON 逐字节不变；
# 会改谱面结构的开关只要求「音符集合守恒」（见 combo_errors）。
# 不放 --track（会换掉整份音符集，集合守恒不适用）。
COMBOS = [
    ["--bars", "1"], ["--bars", "2"], ["--bars", "3"], ["--bars", "4"],
    ["--bars", "5"], ["--bars", "6"], ["--bars", "7"], ["--bars", "8"],
    ["--no-dynamics"],
    ["--no-midi-lyrics"],
    ["--bars", "4", "--no-dynamics"],
    ["--bars", "4", "--no-midi-lyrics"],
]
# 全素材只跑这三个核心组合，--bars 1..8 全边界交给代表子集跑，控制 CI 时长。
CORE_COMBOS = [
    ["--bars", "4"],
    ["--no-dynamics"],
    ["--bars", "4", "--no-dynamics"],
]

# 会改谱面结构的开关：--no-dynamics 关掉整个 ExpressionMap，倚音不再合并、
# 声部拆分顺序随之变；--no-quantize 改音符时值。两者都允许 PLAY 的排列/时值
# 变化，但仍必须守住音符集合守恒。
PLAY_MUTABLE_FLAGS = ("--no-dynamics", "--no-quantize")


def play_is_mutable(combo):
    """该组合是否允许 PLAY JSON 与基准不同（改用「音符集合守恒」口径）。"""
    return any(f in combo for f in PLAY_MUTABLE_FLAGS)

# 用假 DOM / 假 AudioContext 跑那段内嵌 JS，数「排了几个音」。
# 断言点：恰好排 N 个振荡器（N = PLAY.notes 的长度），否则就是播放逻辑坏了。
HARNESS = r"""
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');

function mkEl(tick) {
  const set = new Set();
  return {
    _h: {}, _tick: (tick === undefined || tick === null) ? null : String(tick),
    parentNode: null,
    classList: {
      add: c => set.add(c),
      remove: c => set.delete(c),
      contains: c => set.has(c),
    },
    addEventListener(t, fn) { this._h[t] = fn; },
    getAttribute(n) { return n === 'data-t' ? this._tick : null; },
    textContent: '', value: '1', style: {},
    getBoundingClientRect: () => ({ top: 100, bottom: 120 }),
    scrollIntoView() {},
    _classes: set,
  };
}
const btn = mkEl(), sel = mkEl(), stat = mkEl();
const byTick = {};
function elsFor(tick) {
  const k = String(tick);
  if (!byTick[k]) byTick[k] = [mkEl(tick)];
  return byTick[k];
}
const docHandlers = {};
global.document = {
  getElementById: id => (id === 'pp' ? btn : id === 'spd' ? sel
                        : id === 'pstat' ? stat : null),
  querySelectorAll: q => {
    const m = /^\[data-t='(\d+)'\]$/.exec(q);
    return m ? elsFor(m[1]) : [];
  },
  addEventListener(t, fn) { docHandlers[t] = fn; },
};
const started = [];
class Gain {
  constructor() {
    this._ramps = [];
    const self = this;
    this.gain = { value: 1, setValueAtTime() {},
                  exponentialRampToValueAtTime(v) { self._ramps.push(v); } };
  }
  connect() {}
}
class Osc {
  constructor() { this.type = ''; this.frequency = { value: 0 }; }
  connect(t) { this._g = t; }
  start(t) {
    const g = (this._g && this._g._ramps.length) ? Math.max.apply(null, this._g._ramps) : 1;
    started.push({ t: t, f: this.frequency.value, gain: g });
  }
  stop() {}
}
global.window = {
  innerHeight: 800, addEventListener() {},
  AudioContext: class {
    constructor() { this.currentTime = 0; this.state = 'running';
                    this.destination = {}; }
    createOscillator() { return new Osc(); }
    createGain() { return new Gain(); }
    resume() {}
  },
};
global.requestAnimationFrame = () => 0;
global.cancelAnimationFrame = () => {};
global.setTimeout = (fn) => { fn(); return 0; };
global.clearTimeout = () => {};

eval(src);

if (!btn._h.click) { console.log('NO_HANDLER'); process.exit(0); }
btn._h.click();
const full = started.slice();

const P = JSON.parse(/var M2J_PLAY = (\{[\s\S]*?\});/.exec(src)[1]);
const peak = v => 0.04 + 0.26 * ((v > 0 && v <= 127) ? v : 96) / 127;
const uniq = a => [...new Set(a)].sort((x, y) => x - y);
const r6 = a => uniq(a.map(v => Math.round(v * 1e6)));

// 力度 -> 增益：出现的每个峰值增益都必须对应某个音符的力度换算值。
const wantGain = r6(P.notes.map(n => peak(n.length > 4 ? n[4] : 96)));
const gotGain = r6(full.map(s => s.gain));
const gainOK = JSON.stringify(wantGain) === JSON.stringify(gotGain);
const gainSpread = full.length
  ? Math.max.apply(null, full.map(s => s.gain)) - Math.min.apply(null, full.map(s => s.gain))
  : 0;

// 点击某个音符：应从该 tick 起播，第一个排出的音就是它。
const ticks = uniq(P.notes.map(n => n[0]));
const from = ticks[Math.floor(ticks.length / 2)];
started.length = 0;
let seekOK = false, seekCount = -1, wantCount = 0;
const els = elsFor(from);
if (docHandlers.click && els.length) {
  docHandlers.click({ target: els[0] });
  const seeked = started.slice();
  wantCount = P.notes.filter(n => n[0] >= from).length;
  seekCount = seeked.length;
  const wantF = 440 * Math.pow(2,
    (Math.min.apply(null, P.notes.filter(n => n[0] === from).map(n => n[2])) - 69) / 12);
  if (seeked.length) {
    const minT = Math.min.apply(null, seeked.map(s => s.t));
    const atMin = seeked.filter(s => Math.abs(s.t - minT) < 1e-9);
    seekOK = seeked.length === wantCount && Math.abs(minT - 0.12) < 1e-9 &&
             atMin.some(s => Math.abs(s.f - wantF) < 1e-6);
  }
}

console.log(JSON.stringify({ scheduled: full.length, gainOK, gainSpread,
                             seekOK, seekCount, wantCount }));
"""


def default_exe():
    for name in ("midi2jianpu.exe", "midi2jianpu"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return "midi2jianpu"


def fixtures():
    return sorted(glob.glob(os.path.join(FIX, "*.mid")))


def render(exe, mid, out, player, extra=None):
    args = [exe, mid, "--html", out]
    if player:
        args.append("--player")
    if extra:
        args.extend(extra)
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    with open(out, "rb") as f:
        return f.read().decode("utf-8", "replace")


def scan(html):
    """纯函数：返回 (data 或 None, 问题列表)。自测直接调它。"""
    errs = []
    if html.count("id='playerbar'") != 1:
        errs.append("playerbar 出现 %d 次（应为 1）" % html.count("id='playerbar'"))
    n_script = len(SCRIPT_RE.findall(html))
    if n_script != 1:
        errs.append("<script> 块 %d 个（应为 1）" % n_script)
    m = PLAY_RE.search(html)
    if not m:
        errs.append("找不到 M2J_PLAY 播放数据")
        return None, errs
    try:
        data = json.loads(m.group(1))
    except ValueError as e:
        errs.append("PLAY JSON 解析失败: %s" % e)
        return None, errs

    div = data.get("division")
    if not isinstance(div, int) or div <= 0:
        errs.append("division 非法: %r" % (div,))
    tempo = data.get("tempo") or []
    if not tempo:
        errs.append("tempo 段为空")
    prev = -1
    for seg in tempo:
        if not (isinstance(seg, list) and len(seg) == 2):
            errs.append("tempo 段格式非法: %r" % (seg,))
            continue
        if seg[0] < prev:
            errs.append("tempo 段未按 tick 升序: %r" % (seg,))
        if not (isinstance(seg[1], int) and seg[1] > 0):
            errs.append("tempo 段 us/拍 非法: %r" % (seg,))
        prev = seg[0]

    notes = data.get("notes") or []
    if not notes:
        errs.append("notes 为空（点了播放会没声音）")
    note_ticks = set()
    n_with_vel = 0
    for n in notes:
        if not (isinstance(n, list) and len(n) in (4, 5)):
            errs.append("音符格式非法: %r" % (n,))
            continue
        tick, dur, pitch, _track = n[:4]
        if dur <= 0:
            errs.append("音符时长 %d <= 0 (tick=%d)" % (dur, tick))
        if not (0 <= pitch <= 127):
            errs.append("音高越界 %d (tick=%d)" % (pitch, tick))
        if tick < 0:
            errs.append("起点 tick 为负: %d" % tick)
        if len(n) == 5:
            n_with_vel += 1
            if not (1 <= n[4] <= 127):
                errs.append("力度越界 %r (tick=%d)" % (n[4], tick))
        note_ticks.add(tick)
    # v1.73.0 起力度必须真的送进播放层；老页面（全 4 元音）按 96 处理能跑，
    # 但本项目自己导出的页面不许退回去——否则重音与弱起又听着一样响。
    if notes and n_with_vel == 0:
        errs.append("播放数据里没有力度（第 5 位），强弱会全一样响")

    ticks = TICK_RE.findall(html)
    if not ticks:
        errs.append("没有任何 data-t：播放时高亮永远不亮")
    stray = [t for t in ticks if int(t) not in note_ticks]
    if stray:
        errs.append("有 %d 个 data-t 在 PLAY 里找不到对应音（例: %s）"
                    % (len(stray), stray[:5]))

    if ".note.playing" not in html:
        errs.append("缺少 .note.playing 高亮样式")
    if "document.addEventListener('click'" not in html:
        errs.append("缺少点击谱面定位起播的委托监听")
    if "@media print" not in html or "#playerbar{display:none}" not in html:
        errs.append("打印样式没有隐藏播放器")
    return data, errs


def plain_errors(html):
    """不带 --player 时：一个播放器痕迹都不该有。"""
    errs = []
    if "<script" in html:
        errs.append("默认输出出现了 <script>")
    if "data-t='" in html:
        errs.append("默认输出出现了 data-t")
    if "playerbar" in html:
        errs.append("默认输出出现了 playerbar")
    return errs


def combo_errors(base_html, combo_html, label, expect_same_play=True):
    """v1.98.0 纯函数：组合渲染相对 `--player` 基准必须满足三件事。

    1. 结构照样合法（scan 的 A/B 两层）；
    2. PLAY 数据守契约：
       - expect_same_play=True（纯排版/显示开关）：逐字节不变；
       - expect_same_play=False（会改谱面结构的开关）：division / tempo
         不变，且音符 multiset 守恒——顺序与时值可变，但不许掉音或多音；
    3. playerbar 仍在 `<h1>` 之前（position:sticky 依赖文档流位置）。
    """
    errs = []
    _, e = scan(combo_html)
    errs.extend("%s: %s" % (label, x) for x in e)
    if not isinstance(base_html, str) or not isinstance(combo_html, str):
        return errs + ["%s: 页面不是字符串" % label]
    a = PLAY_RE.search(base_html)
    b = PLAY_RE.search(combo_html)
    if not a or not b:
        errs.append("%s: 基准或组合页缺 M2J_PLAY" % label)
    elif expect_same_play:
        if a.group(1) != b.group(1):
            errs.append("%s: 排版/显示开关改变了 PLAY JSON" % label)
    else:
        try:
            da = json.loads(a.group(1))
            db = json.loads(b.group(1))
        except ValueError as ex:
            errs.append("%s: PLAY JSON 解析失败: %s" % (label, ex))
        else:
            if (da.get("division") != db.get("division")
                    or da.get("tempo") != db.get("tempo")):
                errs.append("%s: 结构开关改了 division/tempo" % label)
            if sorted(da.get("notes") or []) != sorted(db.get("notes") or []):
                errs.append("%s: 音符集合不守恒（掉音或多音）" % label)
    pb = combo_html.find("id='playerbar'")
    h1 = combo_html.find("<h1>")
    if pb < 0 or h1 < 0 or pb > h1:
        errs.append("%s: playerbar 不在 <h1> 之前" % label)
    return errs


def runtime_errors(html, expect, td):
    """在 node 里真跑一遍内嵌 JS，返回问题列表。"""
    node = None
    for cand in ("node", "node.exe"):
        try:
            r = subprocess.run([cand, "--version"], capture_output=True)
            if r.returncode == 0:
                node = cand
                break
        except OSError:
            continue
    if node is None:
        return None                      # 没有 node 就跳过运行时这一层
    m = SCRIPT_RE.search(html)
    if not m:
        return ["提取不到 <script>，无法做运行时校验"]
    js = os.path.join(td, "player.js")
    with open(js, "w", encoding="utf-8", newline="\n") as f:
        f.write(m.group(1))
    harn = os.path.join(td, "harness.js")
    with open(harn, "w", encoding="utf-8", newline="\n") as f:
        f.write(HARNESS)
    r = subprocess.run([node, harn, js], capture_output=True)
    out = r.stdout.decode("utf-8", "replace").strip()
    if r.returncode != 0:
        return ["node 运行播放器脚本失败: %s"
                % r.stderr.decode("utf-8", "replace").strip().splitlines()[:2]]
    if out == "NO_HANDLER":
        return ["播放按钮没有绑定 click 事件"]
    try:
        got = json.loads(out)
    except ValueError:
        return ["node 输出不可解析: %r" % out[:120]]
    errs = []
    if got.get("scheduled") != expect:
        errs.append("排了 %r 个音，PLAY 里有 %d 个（应相等）"
                    % (got.get("scheduled"), expect))
    if not got.get("gainOK"):
        errs.append("峰值增益与音符力度对不上（力度没有真正影响音量）")
    if not got.get("seekOK"):
        errs.append("点击第 %r 个 tick 应从该处起播 %r 个音，实际 %r 个"
                    % (got.get("seekCount"), got.get("wantCount"),
                       got.get("seekCount")))
    return errs


def combo_fixtures(mids):
    """代表子集：前 8 份 + 拍号/弱起/声部/大谱表等特殊素材，跑 --bars 全边界。"""
    keys = ("meter", "pickup", "voice", "grand", "poly", "modulation", "tempo")
    picked = mids[:8] + [m for m in mids
                         if any(k in os.path.basename(m) for k in keys)][:6]
    return set(picked)


def audit(exe):
    bad = []
    stats = dict(files=0, notes=0, data_t=0, runtime=0, combos=0)
    mids = fixtures()
    combo_mids = combo_fixtures(mids)
    extra_combos = [c for c in COMBOS if c not in CORE_COMBOS]
    with tempfile.TemporaryDirectory() as td:
        for mid in mids:
            name = os.path.basename(mid)
            plain = render(exe, mid, os.path.join(td, "p.html"), False)
            if plain is None:
                bad.append(name + ": 默认渲染失败")
                continue
            for e in plain_errors(plain):
                bad.append("%s: %s" % (name, e))
            html = render(exe, mid, os.path.join(td, "q.html"), True)
            if html is None:
                bad.append(name + ": --player 渲染失败")
                continue
            data, errs = scan(html)
            for e in errs:
                bad.append("%s: %s" % (name, e))

            # v1.98.0：组合矩阵。全素材跑核心 3 组，代表子集再跑 --bars 1..8
            # 全边界与其余组合；每组都要过结构 + PLAY 不变量 + playerbar 位置。
            combos = list(CORE_COMBOS)
            if mid in combo_mids:
                combos.extend(extra_combos)
            for combo in combos:
                tag = "%s %s" % (name, " ".join(combo))
                ch = render(exe, mid, os.path.join(td, "c.html"), True, combo)
                if ch is None:
                    bad.append(tag + ": 组合渲染失败")
                    continue
                for e in combo_errors(html, ch, tag,
                                      expect_same_play=not play_is_mutable(combo)):
                    bad.append(e)
                stats["combos"] += 1

            if data:
                stats["files"] += 1
                stats["notes"] += len(data.get("notes") or [])
                stats["data_t"] += len(TICK_RE.findall(html))
                # 全量跑 node 太慢（94 份 × 启动 node），抽查前两份即可：
                # 播放逻辑是同一段代码，素材差异只体现在 PLAY 数据里。
                if stats["runtime"] < 2:
                    rerr = runtime_errors(html, len(data.get("notes") or []), td)
                    if rerr is None:
                        stats["runtime"] = -1
                    elif rerr:
                        bad += ["%s: %s" % (name, e) for e in rerr]
                    else:
                        stats["runtime"] += 1
    return bad, stats


def main():
    if "--self-test" in sys.argv:
        return self_test()
    exe = default_exe()
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    bad, stats = audit(exe)
    if bad:
        print("试听播放器校验发现 %d 处问题：" % len(bad))
        for b in bad[:30]:
            print("  -", b)
        return 1
    rt = "已跑" if stats["runtime"] > 0 else "跳过（无 node）"
    print("试听播放器校验通过：%d 份素材 / %d 个音 / %d 个 data-t 高亮锚点 / "
          "%d 组组合参数（逐字节不变 + 音符集合守恒两类契约），运行时检查%s"
          % (stats["files"], stats["notes"], stats["data_t"],
             stats["combos"], rt))
    return 0


def self_test():
    n = 0
    failed = []

    def ok(cond, what):
        nonlocal n
        n += 1
        if not cond:
            failed.append(what)

    def page(json_text, extra=""):
        return ("<div id='playerbar'><button id='pp'></button></div>"
                "<style>.note.playing{}@media print{#playerbar{display:none}}"
                "</style>"
                "<span class='note' data-t='0'></span>"
                "<script>\nvar M2J_PLAY = %s;\n(function(){"
                "document.addEventListener('click',function(){});})();\n"
                "</script>%s" % (json_text, extra))

    good = ('{"division":480,"tempo":[[0,500000]],'
            '"notes":[[0,480,60,0,96],[480,480,62,0,40]]}')
    data, errs = scan(page(good))
    ok(errs == [] and data is not None and len(data["notes"]) == 2,
       "正向：合法页面应无问题，实际 %s" % errs)

    # v1.73.0 力度：老页面（全 4 元音）能跑，但本项目导出的页面不许退回去。
    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0]]}'))
    ok(any("没有力度" in x for x in e), "全 4 元音必须报「没有力度」，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0,0]]}'))
    ok(any("力度越界" in x for x in e), "力度 0 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0,128]]}'))
    ok(any("力度越界" in x for x in e), "力度 128 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0,1]]}'))
    ok(e == [], "力度 1 合法，实际 %s" % e)

    _, e = scan(page(good).replace("document.addEventListener('click'", "x"))
    ok(any("委托监听" in x for x in e), "缺点击定位必须报错，实际 %s" % e)

    _, e = scan(page('{"division":0,"tempo":[[0,500000]],"notes":[[0,480,60,0,96]]}'))
    ok(any("division" in x for x in e), "division=0 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[],"notes":[[0,480,60,0,96]]}'))
    ok(any("tempo" in x for x in e), "tempo 为空必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000],[0,400000]],'
                     '"notes":[[0,480,60,0,96]]}'))
    ok(not any("升序" in x for x in e), "同 tick 的 tempo 段不算乱序")

    _, e = scan(page('{"division":480,"tempo":[[480,500000],[0,400000]],'
                     '"notes":[[0,480,60,0,96]]}'))
    ok(any("升序" in x for x in e), "tempo 倒序必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,0]],"notes":[[0,480,60,0,96]]}'))
    ok(any("us/拍" in x for x in e), "tempo 的 us=0 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,0,60,0,96]]}'))
    ok(any("时长" in x for x in e), "时长为 0 的音必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,200,0,96]]}'))
    ok(any("音高越界" in x for x in e), "音高 200 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[-5,480,60,0,96]]}'))
    ok(any("负" in x for x in e), "负 tick 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[]}'))
    ok(any("notes 为空" in x for x in e), "空 notes 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0,96]]}')
                .replace("data-t='0'", "data-t='999'"))
    ok(any("找不到对应音" in x for x in e), "data-t 对不上必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,"tempo":[[0,500000]],"notes":[[0,480,60,0,96]]}')
                .replace("data-t='0'", ""))
    ok(any("没有任何 data-t" in x for x in e), "缺 data-t 必须报错，实际 %s" % e)

    _, e = scan(page(good).replace("<script>", "<scriptx>"))
    ok(any("script" in x for x in e), "script 数量不对必须报错，实际 %s" % e)

    _, e = scan(page(good).replace("id='playerbar'", "id='nobar'"))
    ok(any("playerbar" in x for x in e), "缺 playerbar 必须报错，实际 %s" % e)

    _, e = scan(page('{"division":480,}'))
    ok(any("JSON" in x for x in e), "坏 JSON 必须报错，实际 %s" % e)

    ok(plain_errors("<html><body><p>谱面</p></body></html>") == [],
       "反向：不含播放器的页面应通过")
    ok(plain_errors("<html><script>x</script></html>") != [],
       "反向：默认输出出现 script 必须报错")
    ok(plain_errors("<img data-t='1'>") != [],
       "反向：默认输出出现 data-t 必须报错")

    # ---- v1.98.0：组合矩阵 / PLAY JSON 排版不变量的纯逻辑自测 ----
    def combo_page(json_text=good, extra=""):
        return page(json_text, extra="<h1>谱面</h1>" + extra)

    base = combo_page()
    ok(combo_errors(base, base, "same") == [],
       "组合：与基准同页应无问题，实际 %s" % combo_errors(base, base, "same"))

    changed = combo_page(good.replace("500000", "400000"))
    ok(any("PLAY JSON" in x for x in combo_errors(base, changed, "tempo")),
       "组合：PLAY JSON 被排版开关改掉必须报错")

    no_h1 = page(good)
    ok(any("<h1>" in x for x in combo_errors(base, no_h1, "noh1")),
       "组合：缺 <h1> 必须报 playerbar 位置问题")

    moved = base.replace("<div id='playerbar'><button id='pp'></button></div>", "")
    moved += "<div id='playerbar'><button id='pp'></button></div>"
    ok(any("<h1>" in x for x in combo_errors(base, moved, "moved")),
       "组合：playerbar 被挪到 <h1> 之后必须报错")

    broken = combo_page().replace("<script>", "<scriptx>")
    ok(any("broken: " in x and "script" in x
           for x in combo_errors(base, broken, "broken")),
       "组合：结构被破坏必须带 label 报错，实际 %s"
       % combo_errors(base, broken, "broken"))

    bar_vals = {c[1] for c in COMBOS if c[0] == "--bars"}
    ok(bar_vals == set("12345678"),
       "组合矩阵覆盖 --bars 1~8 全边界，实际 %s" % sorted(bar_vals))
    ok(any("--no-dynamics" in c for c in COMBOS)
       and any("--no-midi-lyrics" in c for c in COMBOS),
       "组合矩阵覆盖 --no-dynamics 与 --no-midi-lyrics")
    ok(all(c in COMBOS for c in CORE_COMBOS)
       and all("--player" not in c and "--track" not in c for c in COMBOS),
       "核心组合是全矩阵子集，且矩阵不含 --player / --track")

    # 会改谱面结构的开关走「音符集合守恒」而非逐字节一致。
    ok(play_is_mutable(["--no-dynamics"])
       and play_is_mutable(["--bars", "4", "--no-dynamics"])
       and play_is_mutable(["--no-quantize"])
       and not play_is_mutable(["--bars", "4"])
       and not play_is_mutable(["--no-midi-lyrics"]),
       "PLAY 可变分类：--no-dynamics / --no-quantize 放行，纯排版开关不放行")

    ok(any("division/tempo" in x
           for x in combo_errors(base, changed, "tempo", expect_same_play=False)),
       "集合守恒口径下改 tempo 仍必须报错")

    reordered = combo_page('{"division":480,"tempo":[[0,500000]],'
                           '"notes":[[480,480,62,0,40],[0,480,60,0,96]]}')
    ok(combo_errors(base, reordered, "order", expect_same_play=False) == []
       and any("PLAY JSON" in x
               for x in combo_errors(base, reordered, "order")),
       "音符仅换序：集合守恒口径放行，逐字节口径报错")

    dropped = combo_page('{"division":480,"tempo":[[0,500000]],'
                         '"notes":[[0,480,60,0,96]]}')
    ok(any("音符集合" in x
           for x in combo_errors(base, dropped, "drop", expect_same_play=False)),
       "集合守恒口径下掉一个音必须报错")

    print("check_html_player self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  -", f)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""网页文件提示栏的「状态先推、后刷新」顺序守护（v1.173.0 起）。

背景：updateFileHints() 通过若干 WASM 导出读取转换开关的**全局状态**
（g_useMidiLyrics / g_usePentatonic / g_useDynamics / g_transpose）。这些
导出每次都从 wasm 全局读当前值，而不是从网页控件读。于是改变这些开关的
onchange 必须先把新值 set_* 推到 wasm，再调 updateFileHints() 刷新提示；
否则提示栏读的是上一次转换留下的旧值，与随后 convert() 渲染出的谱面
自相矛盾。历史上撞过的两次：

  * v1.171.0：关掉内置歌词后提示仍说「自带 N 个音节、选回自动才会用上」。
  * v1.173.0：改移调后宫音提示停留旧调（谱面「1=D」、提示仍「1=C」）。

本守护把这条顺序钉成静态规则：凡 onchange 里同时含 updateFileHints()
和某个 set_* 的，set_* 必须出现在 updateFileHints() 之前。并对五个已知
受影响控件逐一点名（含 v1.174.0 的软踏板强度），防止整段 set 调用被删掉导致规则空转。

v1.174.0 补同一类问题的另一半：提示栏里的「歌词：已加载 N 行」与「LRC
已按 tempo map 对齐」由**歌词框内容**决定，不只有 wasm 全局态。贴歌词、
敲歌词、清空歌词三个入口只要改了 lyricsArea.value，就必须刷一次提示；
否则清空后仍宣称歌词在，与 CLI 不给 --lyrics 时两句话都不打印相反。

用法：
  python tests/check_html_hints.py                # 校验 docs/index.html
  python tests/check_html_hints.py --self-test    # 纯逻辑自测（内存样本）
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX = os.path.join(ROOT, "docs", "index.html")

# (控件锚点, 必须在其 onchange 内出现的 set 导出, 该开关影响的提示)
RULES = [
    ("useMidiBox.onchange", "_midi2jianpu_set_use_midi_lyrics", "内置歌词开关"),
    ("usePentBox.onchange", "_midi2jianpu_set_use_pentatonic", "五声开关"),
    ("useDynBox.onchange", "_midi2jianpu_set_use_dynamics", "力度表情开关"),
    ("keyChgBox.onchange", "_midi2jianpu_set_key_changes", "中途转调开关"),
    ("$('softPedal').onchange", "_midi2jianpu_set_soft_pedal_scale", "软踏板强度"),
    ("$('transpose').onchange", "_midi2jianpu_set_transpose", "移调下拉"),
]

# 改了歌词文本、必须刷提示的入口。清空按钮曾只 convert()（v1.174.0 修）。
LYRIC_RULES = [
    ("lyricsArea.oninput", "敲歌词"),
    ("$('lyricsClear').onclick", "清空歌词按钮"),
    ("function applyLyricBytes", "拖入歌词文件"),
]

# 改变谱面的控件必须有 onchange 并触发 convert()。callWasm 每次渲染都会
# 重读控件值，所以只改了下拉、不重渲染的话，屏幕上的谱还是旧的，而随即
# 「复制简谱」「下载 MusicXML」却用新值——同一页面看到与拿到的是两份谱。
# v1.178.0 的弱起下拉正是漏了绑定（bars / transpose 等都有）。
RENDER_RULES = [
    ("trackBox.onchange", "轨道选择"),
    ("useMidiBox.onchange", "内置歌词开关"),
    ("usePentBox.onchange", "五声开关"),
    ("useDynBox.onchange", "力度表情开关"),
    ("quantBox.onchange", "时值量化开关"),
    ("keyChgBox.onchange", "中途转调开关"),
    ("$('softPedal').onchange", "软踏板强度"),
    ("$('transpose').onchange", "移调下拉"),
    ("$('bars').onchange", "每行小节数"),
    ("$('pickup').onchange", "弱起下拉"),
    ("$('key').onchange", "调性下拉"),
    ("$('gong').onchange", "宫音下拉"),
]


def onchange_body(src, anchor):
    """取某个 onchange 的函数体（从锚点到配对的 };），找不到返回 None。"""
    start = src.find(anchor)
    if start < 0:
        return None
    brace = src.find("{", start)
    if brace < 0:
        return None
    end = src.find("};", brace)
    return src[start:end + 2] if end > 0 else src[start:]


def function_body(src, anchor):
    """取锚点后第一个完整函数体（花括号配对，跳过字符串与行注释）。"""
    start = src.find(anchor)
    if start < 0:
        return None
    brace = src.find("{", start)
    if brace < 0:
        return None
    depth, i, quote, esc = 0, brace, None, False
    while i < len(src):
        c = src[i]
        if quote:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == quote:
                quote = None
        elif c in ("'", '"', "`"):
            quote = c
        elif c == "/" and i + 1 < len(src) and src[i + 1] == "/":
            nl = src.find("\n", i)
            i = len(src) if nl < 0 else nl - 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
        i += 1
    return None


def check_lyric_entries(src):
    """改了歌词文本的入口必须刷提示，否则清空后文案残留。"""
    errs = []
    for anchor, label in LYRIC_RULES:
        body = function_body(src, anchor)
        if body is None:
            errs.append("缺少歌词入口 %s" % anchor)
        elif "updateFileHints()" not in body:
            errs.append("%s 改了歌词文本却不刷提示（%s 的文案会残留）"
                        % (anchor, label))
    return errs


def check_tempo_hint_format(src):
    """变速提示里的 LRC 说法必须经 WASM 判格式，不能只看歌词框非空。"""
    body = function_body(src, "function updateFileHints")
    if body is None:
        return ["缺少 updateFileHints() 函数"]
    if "LRC 已按 tempo map" in body and "_midi2jianpu_lyric_format" not in body:
        return ["网页变速提示只看歌词框非空就说 LRC tempo map，"
                "未经 WASM 判格式（贴 JSON 会说谎）"]
    return []


def check_convert_failure(src):
    """转换失败必须清掉旧谱并禁用复制/下载/播放按钮。"""
    body = function_body(src, "function convert")
    if body is None:
        return ["缺少 convert() 函数"]
    if "disabled = true" not in body or "$('score').innerHTML = ''" not in body:
        return ["convert() 失败分支没清旧谱 / 没禁用按钮（会复制或下载错误内容）"]
    return []


def check_render_controls(src):
    """改谱面的控件必须重渲染，否则屏幕上的谱与复制/导出的谱分叉。"""
    errs = []
    for anchor, label in RENDER_RULES:
        body = function_body(src, anchor)
        if body is None:
            errs.append("改变谱面的 %s 没有 onchange（页面谱面与复制/下载产物会分叉）"
                        % label)
        elif "convert()" not in body:
            errs.append("%s 改了却不重渲染（页面谱面与复制/下载产物会分叉）"
                        % label)
    return errs


def check_meta_source(src):
    """状态行的「1=X」只能从 .meta 区取，不能从整段 HTML 抓。"""
    body = function_body(src, "function convert")
    if body is None:
        return ["缺少 convert() 函数"]
    if "html.match(/1=[^｜<]*/)" in body:
        return ["状态行从整段 HTML 抓调号，文件名含 1= 时会把 <h1> 标题当调号"]
    if '<div class="meta">' not in body:
        return ["状态行调号没有限定在 .meta 区取"]
    return []


def check(src):
    """返回错误信息列表；空列表表示全部通过。"""
    errs = []
    for anchor, setter, label in RULES:
        body = onchange_body(src, anchor)
        if body is None:
            errs.append("缺少控件 %s" % anchor)
            continue
        if "updateFileHints()" not in body:
            # 该控件的 onchange 不刷提示，就不受此规则约束（例如将来改成只 convert）。
            continue
        i_set = body.find(setter)
        i_hint = body.find("updateFileHints()")
        if i_set < 0:
            errs.append("%s 的 onchange 刷了提示却没推 %s（%s 会用旧状态）"
                        % (anchor, setter, label))
        elif i_set > i_hint:
            errs.append("%s 的 onchange 在 updateFileHints() 之后才推 %s（顺序颠倒，%s 用旧状态）"
                        % (anchor, setter, label))
    return (errs + check_lyric_entries(src) + check_tempo_hint_format(src)
            + check_convert_failure(src) + check_render_controls(src)
            + check_meta_source(src))


def self_test():
    """用内存样本验证检查器本身：正确顺序过，缺失/颠倒都要报。"""
    def block(anchor, setter):
        return ("  %s = function () {\n"
                "    wasmMod.%s(1);\n"
                "    if (midiBytes) { updateFileHints(); convert(); }\n"
                "  };\n") % (anchor, setter)

    def lyric_block(anchor):
        if anchor.startswith("function "):
            return ("  %s(bytes, name) {\n"
                    "    lyricsArea.value = decodeLyricBytes(bytes);\n"
                    "    if (midiBytes && wasmReady) { updateFileHints(); convert(); }\n"
                    "  }\n") % anchor
        return ("  %s = function () {\n"
                "    lyricsArea.value = '';\n"
                "    if (midiBytes && wasmReady) { updateFileHints(); convert(); }\n"
                "  };\n") % anchor

    # 完整 5 控件 + 3 歌词入口的正确样本。
    parts = [block(a, s) for a, s, _ in RULES]
    parts += [lyric_block(a) for a, _ in LYRIC_RULES]
    parts.append(
        "function updateFileHints() {\n"
        "  var f = wasmMod._midi2jianpu_lyric_format(lp);\n"
        "  if (f === 1) tmsg += '，LRC 已按 tempo map 分段对齐';\n"
        "}\n")
    parts.append(
        "function convert() {\n"
        "  var fail = function (msg) {\n"
        "    $('score').innerHTML = '';\n"
        "    ['copyText'].forEach(function (id) { $(id).disabled = true; });\n"
        "  };\n"
        "  var metaDiv = html.match(/<div class=\"meta\">([\\s\\S]*?)<\\/div>/);\n"
        "  var m = metaDiv && metaDiv[1].match(/1=[^｜<]*/);\n"
        "  fail('错误');\n"
        "}\n")
    # RULES 六个块已带 convert()；其余渲染控件单独补绑定。
    for anchor, _ in RENDER_RULES:
        if any(anchor in b for b in parts):
            continue
        parts.append("  %s = function () {\n"
                     "    if (midiBytes) convert();\n"
                     "  };\n" % anchor)
    good = "\n".join(parts)
    assert check(good) == [], "正确顺序样本应通过：%s" % check(good)

    # 缺 set：歌词、移调两处删掉 set，应各报一条。
    missing = good.replace(
        "    wasmMod._midi2jianpu_set_use_midi_lyrics(1);\n", "") \
                  .replace(
        "    wasmMod._midi2jianpu_set_transpose(1);\n", "")
    assert len(check(missing)) == 2, "缺 set 应报两条：%s" % check(missing)

    # 顺序颠倒：把移调那块的 set 挪到 updateFileHints() 之后，应报一条。
    flipped = good.replace(
        "    wasmMod._midi2jianpu_set_transpose(1);\n"
        "    if (midiBytes) { updateFileHints(); convert(); }\n",
        "    if (midiBytes) { updateFileHints(); convert(); }\n"
        "    wasmMod._midi2jianpu_set_transpose(1);\n")
    got = check(flipped)
    assert len(got) == 1 and "顺序颠倒" in got[0], "颠倒顺序应报：%s" % got

    # 清空按钮漏刷提示：应单独报一条，且点出「文案残留」。
    no_clear = good.replace(
        "  $('lyricsClear').onclick = function () {\n"
        "    lyricsArea.value = '';\n"
        "    if (midiBytes && wasmReady) { updateFileHints(); convert(); }\n",
        "  $('lyricsClear').onclick = function () {\n"
        "    lyricsArea.value = '';\n"
        "    if (midiBytes) convert();\n")
    got = check(no_clear)
    assert len(got) == 1 and "残留" in got[0], "清空漏刷应报：%s" % got

    # 变速提示删掉格式判定：应单独报一条，且点出「贴 JSON 会说谎」。
    no_format = good.replace(
        "  var f = wasmMod._midi2jianpu_lyric_format(lp);\n", "")
    got = check(no_format)
    assert len(got) == 1 and "说谎" in got[0], "格式漏判应报：%s" % got

    # 失败分支不收尾：应单独报一条，且点出「错误内容」。
    no_fail = good.replace("    $('score').innerHTML = '';\n", "")
    got = check(no_fail)
    assert len(got) == 1 and "错误内容" in got[0], "失败态漏收尾应报：%s" % got

    # 弱起下拉漏绑：应单独报一条，且点出「分叉」。
    pickup_block = ("  $('pickup').onchange = function () {\n"
                    "    if (midiBytes) convert();\n"
                    "  };\n")
    assert pickup_block in good
    got = check(good.replace(pickup_block, ""))
    assert len(got) == 1 and "分叉" in got[0], "弱起漏绑应报：%s" % got

    # 状态行回到整段 HTML 抓调号：应单独报一条，且点出「标题」。
    good_meta = ("  var metaDiv = html.match(/<div class=\"meta\">([\\s\\S]*?)<\\/div>/);\n"
                 "  var m = metaDiv && metaDiv[1].match(/1=[^｜<]*/);\n")
    assert good_meta in good
    got = check(good.replace(good_meta, "  var m = html.match(/1=[^｜<]*/);\n"))
    assert len(got) == 1 and "标题" in got[0], "调号来源错应报：%s" % got

    print("check_html_hints 自测通过（正确/缺 set/颠倒/漏刷/格式漏判/失败态/漏绑渲染/调号来源 八类样本）")
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()
    with open(INDEX, encoding="utf-8") as f:
        errs = check(f.read())
    if errs:
        for e in errs:
            print("X", e)
        print("check_html_hints：%d 项失败" % len(errs))
        return 1
    print("check_html_hints：%d 组顺序守护 + %d 组歌词入口守护 + 1 组格式守护"
          " + 1 组失败态守护 + %d 组渲染控件守护 + 1 组调号来源守护全过"
          % (len(RULES), len(LYRIC_RULES), len(RENDER_RULES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

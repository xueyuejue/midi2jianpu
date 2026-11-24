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
和某个 set_* 的，set_* 必须出现在 updateFileHints() 之前。并对四个已知
受影响控件逐一点名，防止整段 set 调用被删掉导致规则空转。

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
    ("$('transpose').onchange", "_midi2jianpu_set_transpose", "移调下拉"),
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
    return errs


def self_test():
    """用内存样本验证检查器本身：正确顺序过，缺失/颠倒都要报。"""
    def block(anchor, setter):
        return ("  %s = function () {\n"
                "    wasmMod.%s(1);\n"
                "    if (midiBytes) { updateFileHints(); convert(); }\n"
                "  };\n") % (anchor, setter)

    # 完整 5 控件、全部「先推状态、后刷提示」的正确样本。
    parts = [block(a, s) for a, s, _ in RULES]
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

    print("check_html_hints 自测通过（正确/缺 set/颠倒 三类样本）")
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
    print("check_html_hints：%d 组 onchange 顺序守护全过" % len(RULES))
    return 0


if __name__ == "__main__":
    sys.exit(main())

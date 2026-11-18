#!/usr/bin/env python3
"""和弦列的歌词三端一致（v1.137.0）。

简谱里一个和弦列只有一个词位，所以 MusicXML 把该列的歌词写在整列的第一个音
上（`!isChordMember` 分支），HTML 的和弦列也只有一个 `<span class='lyric'>`。
两端落点本就相同，非空歌词序列必须逐字一致。

但 html_writer.cpp 的和弦分支从 v1.8.0 引入和弦竖排起就把 `chordColumn()` 的
歌词形参写死成空串——那个形参一直没被赋过值。于是「主旋律轨本身就是和弦」的
曲子（钢琴独奏、人声叠一句双音、或用 `--track` 指到伴奏轨）在 HTML 里整行歌词
凭空消失，命令行却照样报「挂上 N 字」，三端只剩 MusicXML 留着字：

    pedal.mid --lyrics 甲乙.lrc --html x.html --musicxml x.xml
    -> 命令行「挂上 2 字」，MusicXML 有 <text>甲</text><text>乙</text>，
       HTML 两个和弦列的 .lyric 全是空的。

本脚本钉两件事：

  1. 表内每份素材的 HTML 非空 .lyric 序列必须与 MusicXML 的 <text> 序列逐字
     相同。
  2. 每份素材各带一条**自检**，防止本闸空转：
     - `chord` 类（主旋律轨含和弦列）：至少有一个**挂着词的和弦列**。素材若被
       换掉、或和弦列重新丢词，这一条会先红。
     - `merge` 类（主旋律轨含首尾相接的同音）：带词时的词位必须**多于**不带词
       时，也就是同音合并确实被第二个字的音节挡住了。素材若不再含同音相接，
       这一条先红。

为什么不把全部素材都扫一遍：只有「主旋律轨本身含和弦列」或「含同音相接」的
素材才会踩到这两条路径，其余素材的歌词序列天然一致，扫了也只是空转。

用法：
  python tests/check_lyric_sync.py              # 校验表内素材
  python tests/check_lyric_sync.py --self-test  # 纯逻辑自测
  python tests/check_lyric_sync.py --exe PATH   # 指定可执行文件
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 表内素材：`kind` 决定这条用例的自检口径（见文件头）。
CASES = [
    ("pedal.mid", [], "主旋律轨是两个三音和弦", "chord"),
    ("sostenuto.mid", [], "声部1 只有一个和弦列", "chord"),
    ("html_dots.mid", [], "主旋律轨混有 3 个和弦列", "chord"),
    ("xml_structure.mid", [], "主旋律轨混有 1 个和弦列", "chord"),
    ("karaoke.mid", ["--track", "2"], "用 --track 指到纯和弦的钢琴轨", "chord"),
    ("parts.mid", ["--track", "2"], "多声部素材里的伴奏轨", "chord"),
    ("retrigger.mid", [], "主旋律轨头两个音同音相接", "merge"),
    ("sample.mid", [], "第 3 小节有一对同音相接", "merge"),
]

COL_SPLIT = "<span class='note "
LYRIC = re.compile(r"class='lyric'>(.*?)</span>")
XML_TEXT = re.compile(r"<text>(.*?)</text>")
SLOT = re.compile(r"class='lyric'>")


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def html_columns(body):
    """把 HTML 谱面按音符列切开，返回 [(是否和弦列, 歌词文本), ...]。"""
    out = []
    for chunk in body.split(COL_SPLIT)[1:]:
        cls = chunk[:chunk.find("'")]
        m = LYRIC.search(chunk)
        out.append((cls.startswith("chordcol"), m.group(1) if m else ""))
    return out


def compare(html_body, xml_body):
    """返回 (html 非空歌词序列, xml 歌词序列, 挂着词的和弦列数)。"""
    cols = html_columns(html_body)
    hw = [t for _, t in cols if t.strip()]
    chord_lyrics = sum(1 for is_chord, t in cols if is_chord and t.strip())
    xw = [t for t in XML_TEXT.findall(xml_body) if t.strip()]
    return hw, xw, chord_lyrics


def slot_count(html_body):
    """HTML 里的词位总数（含空词位）。判同音合并有没有被抑制就看它。"""
    return len(SLOT.findall(html_body))


def make_lrc(path):
    """40 个互不相同的汉字，一音一字时不会因重字掩盖错位。"""
    syl = "".join(chr(0x4E00 + i) for i in range(40))
    with open(path, "w", encoding="utf-8") as f:
        f.write("[00:00.00]" + syl + "\n")


def self_test():
    """比对逻辑必须能抓到「HTML 空词 / XML 有词」这一种失败。"""
    fails = 0
    good_html = ("<span class='note chordcol'><span class='core'><span class='stack'>"
                 "<span class='tone'></span></span></span>"
                 "<span class='lyric'>甲</span></span>"
                 "<span class='note quarter'><span class='core'></span>"
                 "<span class='lyric'></span></span>"
                 "<span class='note quarter'><span class='core'></span>"
                 "<span class='lyric'>乙</span></span>")
    bad_html = good_html.replace("<span class='lyric'>甲</span>",
                                 "<span class='lyric'></span>")
    xml = "<note><lyric><text>甲</text></lyric></note><note><lyric><text>乙</text></lyric></note>"

    hw, xw, cl = compare(good_html, xml)
    if hw == xw == ["甲", "乙"] and cl == 1:
        print("  [PASS] 正常的和弦列被识别为「挂着词的和弦列」")
    else:
        print("  [FAIL] 正常的和弦列未被识别: %r / %r / %d" % (hw, xw, cl))
        fails += 1

    hw, xw, cl = compare(bad_html, xml)
    if hw != xw:
        print("  [PASS] 和弦列丢词时序列比对报不一致")
    else:
        print("  [FAIL] 和弦列丢词却没被发现: %r / %r" % (hw, xw))
        fails += 1
    if cl == 0:
        print("  [PASS] 丢词后「挂着词的和弦列」计数归零（自检生效）")
    else:
        print("  [FAIL] 丢词后计数仍为 %d" % cl)
        fails += 1

    # 空列（延续段 / 休止）不该被当成歌词，也不该算进和弦列计数
    pad = "<span class='note chordcol'><span class='lyric'> </span></span>"
    hw, xw, cl = compare(pad + good_html, xml)
    if hw == xw == ["甲", "乙"] and cl == 1:
        print("  [PASS] 空白歌词列被忽略")
    else:
        print("  [FAIL] 空白歌词列干扰了比对: %r / %r / %d" % (hw, xw, cl))
        fails += 1

    # 词位计数：merge 类用例靠它判「同音合并有没有被抑制」
    merged_html = good_html.replace(
        "<span class='note quarter'><span class='core'></span>"
        "<span class='lyric'></span></span>",
        "<span class='note quarter'><span class='core'></span>"
        "<span class='lyric'></span></span>"
        "<span class='note quarter'><span class='core'></span>"
        "<span class='lyric'>丙</span></span>")
    if slot_count(good_html) == 3:
        print("  [PASS] 词位计数只数 class='lyric' 的列")
    else:
        print("  [FAIL] 词位计数不对: %d" % slot_count(good_html))
        fails += 1
    if slot_count(merged_html) > slot_count(good_html):
        print("  [PASS] 合并被抑制时词位变多，自检能判出来")
    else:
        print("  [FAIL] 词位没变多却判不出差异")
        fails += 1

    print("自测 %d 项，失败 %d 项" % (6, fails))
    return fails


def main():
    if "--self-test" in sys.argv:
        return 1 if self_test() else 0

    exe = default_exe()
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    if not os.path.exists(exe):
        print("找不到可执行文件: %s" % exe)
        return 2

    tmp = tempfile.mkdtemp(prefix="m2j-lyric-")
    lrc = os.path.join(tmp, "long.lrc")
    make_lrc(lrc)

    bad = 0
    checked = 0
    for name, flags, note, kind in CASES:
        mid = os.path.join(FIX, name)
        if not os.path.exists(mid):
            print("[FAIL] %s 缺失（先跑 tests/fixtures/make_fixture.py）" % name)
            bad += 1
            continue
        h = os.path.join(tmp, name.replace(".mid", ".html"))
        x = os.path.join(tmp, name.replace(".mid", ".xml"))
        cmd = [exe, mid, "--lyrics", lrc, "--html", h, "--musicxml", x] + flags
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0 or not os.path.exists(h) or not os.path.exists(x):
            print("[FAIL] %s %s 未能出谱（rc=%d）%s"
                  % (name, " ".join(flags), r.returncode,
                     r.stderr.decode("utf-8", "replace")[:200]))
            bad += 1
            continue
        checked += 1
        hbody = open(h, encoding="utf-8").read()
        hw, xw, cl = compare(hbody, open(x, encoding="utf-8").read())
        tag = "%s %s" % (name, " ".join(flags)) if flags else name
        if kind == "chord":
            if cl == 0:
                print("[FAIL] %s：没有挂着词的和弦列（%s）——本闸空转" % (tag, note))
                bad += 1
        else:
            # merge 类：同一份素材不带词时的词位数必然更少，否则说明它已经
            # 不含同音相接，这条用例也就测不到东西了。
            ph = os.path.join(tmp, name.replace(".mid", "_plain.html"))
            subprocess.run([exe, mid, "--html", ph] + flags, capture_output=True)
            if os.path.exists(ph):
                n0 = slot_count(open(ph, encoding="utf-8").read())
                n1 = slot_count(hbody)
                if n1 <= n0:
                    print("[FAIL] %s：带词 %d 个词位、不带词 %d 个（%s）——"
                          "同音合并没被抑制，本闸空转" % (tag, n1, n0, note))
                    bad += 1
            else:
                print("[FAIL] %s：不带词的那份没出谱——本闸空转" % tag)
                bad += 1
        if hw != xw:
            print("[FAIL] %s：HTML 歌词 %r 与 MusicXML %r 不一致" % (tag, hw, xw))
            bad += 1
    print("校验 %d 份素材（%d 组），%d 处不一致" % (checked, len(CASES), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

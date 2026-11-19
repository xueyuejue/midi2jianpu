#!/usr/bin/env python3
"""三端歌词落位一致：HTML / MusicXML / 命令行统计（v1.137.0 起）。

钉三件事：

  1. **每一行的词序列必须相同**。HTML 按「轨 + 声部」成块，MusicXML 在同一个
     `<part>` 里按小节 + 声部交错，两边的整体顺序天生不同；能对齐的是每一行
     （HTML 的 `.track` 块 ↔ MusicXML 的 `<voice>`）内部逐字相同。
  2. **字数必须相同**。少了是丢词，多了是同一个字被画了两遍。
  3. **命令行说「挂上 K 字」时，K 必须等于谱面上真正印出来的字数**。前两轮
     的缺陷都栽在这一点上：字挂在「不印词的那一列」上，统计照算、页面全空。

历史上撞过的三个坑（都已闭环，表内素材分别守着）：

  * v1.137.0 和弦列丢词：`chordColumn()` 的歌词形参被写死成空串，
    「主旋律轨本身就是和弦」时整行歌词在 HTML 里凭空消失。
  * v1.138.0 带词的同音仍合并：`5 5` -> `5 -` 之后第二个字的词位没了。
  * v1.139.0 同轨复调的副声部丢词：`assignLyrics` 把词铺在「旋律轨的全部
    音头」上，输出层却只给声部 1 印词——`track_pedal.mid` 的 8 个字只印 3 个。
  * v1.139.0 倚音占词位：倚音不占一列，却算了一个音头，整句词从那里往后错开。
  * v1.140.0 合法 MIDI 音高 0（C-1）被 HTML 当成休止符占位，四个字只印后三个。

每份素材各带一条**自检**，防止本闸空转：

  * `chord`：至少有一个**挂着词的和弦列**。素材被换掉、或和弦列重新丢词，
    这一条先红。
  * `merge`：带词时的词位必须**多于**不带词时，也就是同音合并确实被第二个
    字的音节挡住了。
  * `voice`：至少**两行**挂着词，也就是字确实铺到了副声部上。
  * `pitch0`：MusicXML 里真有 C-1，且 HTML 至少挂上一个字。

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
    ("track_pedal.mid", [], "延音踏板把单线旋律撑成 3 个声部", "voice"),
    ("poly_gate.mid", [], "同轨两条各 12 音的音线", "voice"),
    ("stray_pedal.mid", [], "同轨 3 个声部、7 个音头", "voice"),
    ("port_standard.mid", [], "同轨 2 个声部各 1 个音", "voice"),
    ("overfull.mid", [], "主旋律轨里有一个倚音", "grace"),
    ("pitch0.mid", [], "四个合法 MIDI 音高 0~3，pitch 0 是 C-1", "pitch0"),
]

COL_SPLIT = "<span class='note "
LYRIC = re.compile(r"class='lyric'>(.*?)</span>")
XML_TEXT = re.compile(r"<text>(.*?)</text>")
XML_NOTE = re.compile(r"<note>(.*?)</note>", re.S)
XML_VOICE = re.compile(r"<voice>(\d+)</voice>")
PITCH0 = re.compile(r"<step>C</step><octave>-1</octave>")
SLOT = re.compile(r"class='lyric'>")
PLACED = re.compile(r"共\s*(\d+)\s*字，挂上\s*(\d+)\s*字")


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


def html_rows(body):
    """HTML 按「轨 + 声部」成块，返回每个块里的非空词序列。"""
    rows = []
    for block in body.split("class='track'")[1:]:
        words = tuple(t for t in LYRIC.findall(block) if t.strip())
        if words:
            rows.append(words)
    return rows


def xml_rows(body):
    """MusicXML 按 `<part>` 分块、块内按 `<voice>` 分组，返回非空词序列。

    单声部 part 不写 `<voice>`（旧版逐字节输出不能变），一律当声部 1。
    """
    rows = []
    for part in body.split("<part ")[1:]:
        groups = {}
        order = []
        for note in XML_NOTE.findall(part):
            m = XML_TEXT.search(note)
            if not m or not m.group(1).strip():
                continue
            v = XML_VOICE.search(note)
            key = v.group(1) if v else "1"
            if key not in groups:
                groups[key] = []
                order.append(key)
            groups[key].append(m.group(1))
        for key in order:
            rows.append(tuple(groups[key]))
    return rows


def compare(html_body, xml_body):
    """返回 (html 行序列, xml 行序列, 挂着词的和弦列数)。"""
    chord_lyrics = sum(1 for is_chord, t in html_columns(html_body)
                       if is_chord and t.strip())
    return html_rows(html_body), xml_rows(xml_body), chord_lyrics


def slot_count(html_body):
    """HTML 里的词位总数（含空词位）。判同音合并有没有被抑制就看它。"""
    return len(SLOT.findall(html_body))


def make_lrc(path):
    """40 个互不相同的汉字，一音一字时不会因重字掩盖错位。"""
    syl = "".join(chr(0x4E00 + i) for i in range(40))
    with open(path, "w", encoding="utf-8") as f:
        f.write("[00:00.00]" + syl + "\n")


def self_test():
    fails = 0
    good_html = ("<div class='track'>"
                 "<span class='note chordcol'><span class='core'><span class='stack'>"
                 "<span class='tone'></span></span></span>"
                 "<span class='lyric'>甲</span></span>"
                 "<span class='note quarter'><span class='core'></span>"
                 "<span class='lyric'></span></span>"
                 "<span class='note quarter'><span class='core'></span>"
                 "<span class='lyric'>乙</span></span>"
                 "</div>")
    good_xml = ('<part id="P1">'
                "<note><lyric><text>甲</text></lyric></note>"
                "<note><lyric><text>乙</text></lyric></note>"
                "</part>")

    def expect(name, cond, extra=""):
        nonlocal fails
        if cond:
            print("  [PASS] " + name)
        else:
            print("  [FAIL] " + name + ("  " + extra if extra else ""))
            fails += 1

    hr, xr, cl = compare(good_html, good_xml)
    expect("正常单声部：两行识别成一条词序列",
           hr == xr == [("甲", "乙")] and cl == 1, "%r / %r / %d" % (hr, xr, cl))

    bad = good_html.replace("<span class='lyric'>甲</span>",
                            "<span class='lyric'></span>")
    hr, xr, cl = compare(bad, good_xml)
    expect("和弦列丢词：行序列不等，且「挂着词的和弦列」归零",
           hr != xr and cl == 0, "%r / %r / %d" % (hr, xr, cl))

    pad = "<div class='track'><span class='note chordcol'>" \
          "<span class='lyric'> </span></span></div>"
    hr, xr, cl = compare(pad + good_html, good_xml)
    expect("空白歌词列既不算词、也不算和弦列",
           hr == xr == [("甲", "乙")] and cl == 1, "%r / %r / %d" % (hr, xr, cl))

    # 多声部：HTML 按声部成块、XML 按小节 + 声部交错，整体顺序不同不算错
    two_html = ("<div class='track'><span class='note quarter'>"
                "<span class='lyric'>甲</span></span>"
                "<span class='note quarter'><span class='lyric'>戊</span></span></div>"
                "<div class='track'><span class='note quarter'>"
                "<span class='lyric'>乙</span></span>"
                "<span class='note quarter'><span class='lyric'>己</span></span></div>")
    two_xml = ('<part id="P1">'
               "<note><lyric><text>甲</text></lyric></note>"
               "<note><voice>2</voice><lyric><text>乙</text></lyric></note>"
               "<note><lyric><text>戊</text></lyric></note>"
               "<note><voice>2</voice><lyric><text>己</text></lyric></note>"
               "</part>")
    hr, xr, _ = compare(two_html, two_xml)
    expect("多声部：行间顺序不同不算错，按行对齐",
           sorted(hr) == sorted(xr) == [("乙", "己"), ("甲", "戊")],
           "%r / %r" % (hr, xr))

    # 副声部丢词（v1.139 的形态）：XML 有两行、HTML 只剩一行
    drop_html = ("<div class='track'><span class='note quarter'>"
                 "<span class='lyric'>甲</span></span>"
                 "<span class='note quarter'><span class='lyric'>戊</span></span></div>"
                 "<div class='track'><span class='note quarter'>"
                 "<span class='lyric'></span></span></div>")
    hr, xr, _ = compare(drop_html, two_xml)
    expect("副声部丢词：行序列不等",
           sorted(hr) != sorted(xr), "%r / %r" % (hr, xr))

    # 同一行内部顺序错乱必须报出来（行内是严格序列，不是集合）
    swap_html = good_html.replace(">甲<", ">TMP<").replace(">乙<", ">甲<") \
                         .replace(">TMP<", ">乙<")
    hr, xr, _ = compare(swap_html, good_xml)
    expect("同一行内两个字对调：行序列不等",
           hr != xr, "%r / %r" % (hr, xr))

    # 词位计数：merge 类用例靠它判「同音合并有没有被抑制」
    merged_html = good_html.replace(
        "<span class='lyric'>乙</span>",
        "<span class='lyric'>乙</span></span>"
        "<span class='note quarter'><span class='core'></span>"
        "<span class='lyric'>丙</span>")
    expect("词位计数只数 class='lyric' 的列，合并被抑制时变多",
           slot_count(good_html) == 3 and slot_count(merged_html) > 3,
           "%d / %d" % (slot_count(good_html), slot_count(merged_html)))

    pitch0_xml = "<note><pitch><step>C</step><octave>-1</octave></pitch></note>"
    pitch4_xml = "<note><pitch><step>C</step><octave>4</octave></pitch></note>"
    expect("音高 0 在 MusicXML 里写作 C-1，别把普通 C 当成它",
           bool(PITCH0.search(pitch0_xml)) and not PITCH0.search(pitch4_xml))

    print("自测 8 项，失败 %d 项" % fails)
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
        tag = "%s %s" % (name, " ".join(flags)) if flags else name
        hbody = open(h, encoding="utf-8").read()
        hr, xr, cl = compare(hbody, open(x, encoding="utf-8").read())
        hn = sum(len(t) for t in hr)
        xn = sum(len(t) for t in xr)

        if hn != xn:
            print("[FAIL] %s：HTML %d 字与 MusicXML %d 字不等（%s）"
                  % (tag, hn, xn, note))
            bad += 1
        if sorted(hr) != sorted(xr):
            print("[FAIL] %s：逐行的词序列不一致（%s）\n       html=%r\n       xml =%r"
                  % (tag, note, hr, xr))
            bad += 1

        # 命令行统计与谱面字数必须对得上：这一条正是前两轮缺陷的藏身处
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        m = PLACED.search(out)
        if m and int(m.group(2)) != hn:
            print("[FAIL] %s：命令行说「挂上 %s 字」，谱面只有 %d 字（%s）"
                  % (tag, m.group(2), hn, note))
            bad += 1

        if kind == "chord":
            if cl == 0:
                print("[FAIL] %s：没有挂着词的和弦列（%s）——本闸空转" % (tag, note))
                bad += 1
        elif kind == "merge":
            # 同一份素材不带词时的词位数必然更少，否则说明它已经不含同音相接。
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
        elif kind == "voice":
            if len(hr) < 2:
                print("[FAIL] %s：只有 %d 行挂着词（%s）——副声部的字又没了，"
                      "本闸空转" % (tag, len(hr), note))
                bad += 1
        elif kind == "pitch0":
            xbody = open(x, encoding="utf-8").read()
            if not PITCH0.search(xbody):
                print("[FAIL] %s：MusicXML 里没有 C-1（%s）——素材已变，本闸空转"
                      % (tag, note))
                bad += 1
            if hn == 0:
                print("[FAIL] %s：一个字都没挂上（%s）——本闸空转" % (tag, note))
                bad += 1
        else:                                   # grace
            ph = os.path.join(tmp, name.replace(".mid", "_plain.html"))
            subprocess.run([exe, mid, "--html", ph] + flags, capture_output=True)
            if not os.path.exists(ph):
                print("[FAIL] %s：不带词的那份没出谱——本闸空转" % tag)
                bad += 1
            else:
                n0 = slot_count(open(ph, encoding="utf-8").read())
                n1 = slot_count(hbody)
                if n1 != n0:
                    print("[FAIL] %s：带词 %d 个词位、不带词 %d 个（%s）——"
                          "倚音不该多占一列" % (tag, n1, n0, note))
                    bad += 1
                if hn == 0:
                    print("[FAIL] %s：一个字都没挂上（%s）——本闸空转" % (tag, note))
                    bad += 1
    print("校验 %d 份素材（%d 组），%d 处不一致" % (checked, len(CASES), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

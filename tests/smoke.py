#!/usr/bin/env python3
"""命令行冒烟测试：跑一遍 README 里承诺的所有参数，并校验关键输出。

用法：
    python tests/smoke.py [可执行文件路径]
默认路径按平台猜测：build/midi2jianpu.exe（Windows）或 build/midi2jianpu。

不依赖第三方库；失败时返回非 0 退出码，可直接用于 CI。
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(ROOT, "tests", "fixtures")

failures = []


def check(cond, msg, extra=""):
    if cond:
        print(f"  [PASS] {msg}")
    else:
        print(f"  [FAIL] {msg}")
        if extra:
            print(f"         {extra}")
        failures.append(msg)


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=120)
    return p.returncode, p.stdout or "", p.stderr or ""


def inner_span(html, marker):
    """取出 html 里第一个含 marker 的 <span> 的内部内容（按标签配平）。

    比正则可靠：span 是嵌套的，非贪婪正则会提前停在某个内层闭合处。
    """
    i = html.find(marker)
    if i < 0:
        return ""
    j = html.find(">", i) + 1
    depth, pos = 1, j
    while depth > 0 and pos < len(html):
        a = html.find("<span", pos)
        b = html.find("</span>", pos)
        if b < 0:
            return ""
        if 0 <= a < b:
            depth += 1
            pos = a + 5
        else:
            depth -= 1
            pos = b + 7
    return html[j:pos - 7]


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "build",
        "midi2jianpu.exe" if os.name == "nt" else "midi2jianpu")
    if not os.path.exists(exe):
        print(f"找不到可执行文件: {exe}\n先构建：cmake -B build -S . && cmake --build build")
        return 2

    mid = os.path.join(FIXTURES, "sample.mid")
    lrc = os.path.join(FIXTURES, "sample.lrc")
    if not os.path.exists(mid):
        print("素材缺失，正在生成...")
        subprocess.run([sys.executable, os.path.join(FIXTURES, "make_fixture.py")],
                       check=True)
    tmp = tempfile.mkdtemp(prefix="m2j-smoke-")

    print("\n[1] 默认输出（自动识别调性）")
    rc, out, err = run(exe, mid)
    check(rc == 0, "退出码为 0", err)
    check("1=C 大调" in out, "识别出 C 大调", out[:200])
    check("[Vocal]" in out and "[Piano]" in out and "[Pad]" in out, "分离出三个轨道")
    check(re.search(r"5\s+-", out) is not None, "二分音符标出延长线（5 -）")
    check("3(5 5 5" in out, "三连音标记 3(5 5 5)")
    check("1' -" in out, "同音重复合并成延音（1' -）")
    check("[p]" in out and "[ff]" in out, "力度记号 p / ff")
    # C 和弦与紧接的 G 和弦共用一个 5：同音合并不该把分属两个和弦的 5 并掉
    check("5, - 5, -" in out, "分属两个和弦的同音不被误合并（m3 两个 5 都在）")

    print("\n[2] 手动调性 --key G（八度基准回归）")
    rc, out, _ = run(exe, mid, "--key", "G")
    check(rc == 0 and "1=G 大调" in out, "输出 G 大调")
    vocal = [l for l in out.splitlines() if l.startswith("[C]") or l.startswith("[F]")]
    joined = " ".join(vocal)
    check("4'" not in joined, "G 大调里 C5 不再错误地带高八度点", joined)

    print("\n[3] 降号调 --key Bb")
    rc, out, _ = run(exe, mid, "--key", "Bb")
    check(rc == 0 and "1=Bb 大调" in out, "输出 Bb 大调")
    check("b" in out, "出现降号写法")

    print("\n[4] 非法调性应给出警告而不是静默失败")
    rc, out, err = run(exe, mid, "--key", "H")
    check(rc == 0 and "无法识别" in err, "对非法调性给出警告", err)

    print("\n[5] --track 轨道过滤")
    rc, out, _ = run(exe, mid, "--track", "1")
    check(rc == 0 and "[Vocal]" in out
          and "[Piano]" not in out and "[Pad]" not in out, "只输出轨道 1")

    print("\n[6] --transpose 移调")
    rc, out, _ = run(exe, mid, "--key", "C", "--transpose", "2")
    check(rc == 0 and "移调: +2" in out, "提示移调幅度")

    print("\n[7] --musicxml 导出")
    xml = os.path.join(tmp, "out.xml")
    rc, out, _ = run(exe, mid, "--musicxml", xml)
    check(rc == 0 and os.path.exists(xml), "生成 XML 文件")
    body = open(xml, encoding="utf-8").read()
    check("<score-partwise" in body, "根元素为 score-partwise")
    check("<divisions>480</divisions>" in body, "divisions 与源文件一致")
    check(re.search(r"<duration>\d+</duration>", body) is not None, "duration 为整数")
    check("." not in re.sub(r"<\?xml[^>]*\?>", "",
                           "".join(re.findall(r"<duration>[^<]*<", body))),
          "没有非整数 duration")
    check("<chord/>" in body, "同时发声的音符标了 <chord/>")
    check(re.search(r"<measure number=\"1\">", body) is not None, "存在小节编号")

    # 结构化校验：这些是 MuseScore 能正确打开、不乱排的前提
    import xml.etree.ElementTree as ET
    try:
        root = ET.fromstring(body)
        parsed = True
    except ET.ParseError as e:
        check(False, "XML 可以被解析", str(e))
        parsed = False

    if parsed:
        div = int(root.find(".//divisions").text)
        ts = root.find(".//time")
        bar_len = int(ts.find("beats").text) * div * 4 // int(ts.find("beat-type").text)

        # 小节时长必须等于拍号规定值，否则 MuseScore 会报错或强行排乱
        bad = []
        for pi, part in enumerate(root.findall("part")):
            for m in part.findall("measure"):
                tot = sum(int(n.find("duration").text)
                          for n in m.findall("note") if n.find("chord") is None)
                if tot != bar_len:
                    bad.append(f"P{pi+1}m{m.get('number')}={tot}")
        check(not bad, f"每个小节时长都等于 {bar_len}", " ".join(bad[:6]))

        # 三连音的 <type> 必须是名义时值（eighth），不能写成实际时长对应的 16th
        trip_types = set()
        for n in root.iter("note"):
            if n.find("time-modification") is not None and n.find("type") is not None:
                trip_types.add(n.find("type").text)
        check(trip_types == {"eighth"},
              "三连音 <type> 为名义时值 eighth", str(trip_types))

        # 速度标记：MuseScore 导入后应能看到 BPM
        check(root.find(".//sound[@tempo]") is not None, "带速度标记 <sound tempo>")
        check(root.find(".//metronome") is not None, "带节拍器标记 <metronome>")

        # 谱号：低音轨应自动用 F 谱号，避免满屏加线
        clefs = [c.find("sign").text for c in root.iter("clef")]
        check(len(clefs) == len(root.findall("part")), "每个声部都有谱号")
        check("F" in clefs, "低音域轨道自动使用低音谱号")

        # 跨小节音符：必须切开并用 tie 首尾配对
        starts = len(list(root.iter("tie")))
        start_n = sum(1 for t in root.iter("tie") if t.get("type") == "start")
        stop_n = sum(1 for t in root.iter("tie") if t.get("type") == "stop")
        check(start_n > 0, "跨小节长音被切开并加了 <tie>", f"tie 总数={starts}")
        check(start_n == stop_n, "tie start/stop 数量配对",
              f"start={start_n} stop={stop_n}")
        check(len(list(root.iter("tied"))) == starts,
              "<tie> 与 <notations><tied> 数量一致")

    print("\n[8] --html + --lyrics 导出")
    html = os.path.join(tmp, "out.html")
    rc, out, _ = run(exe, mid, "--html", html, "--lyrics", lrc)
    check(rc == 0 and os.path.exists(html), "生成 HTML 文件")
    body = open(html, encoding="utf-8").read()
    check("<!DOCTYPE html>" in body, "带 DOCTYPE")
    check("class='note" in body, "含音符样式元素")
    check("歌词已加载" in out, "歌词加载成功")

    # 排版结构回归：这几条对应曾经真实出现过的排版事故
    check("class='exts'" in body, "延音线单独成层（不会掉到数字下一行）")
    check("class='oct'" in body, "八度点是独立层（不会压住歌词）")
    check("class='barno'" in body, "小节线旁标注小节号")
    check(re.search(r"class='note [a-z]+' style='--n:", body) is not None,
          "带延音线的音符预留了右侧空间")
    # 跨小节长音必须在小节线处断开，否则该小节时值溢出
    tracks = body.split("<div class='track'>")
    pad = next((t for t in tracks if "<h3>Pad" in t), "")
    check(pad.count("class='note") >= 2, "Pad 轨内容已渲染")
    # 歌词只能出现在旋律轨，伴奏轨出现歌词说明对齐逻辑串轨了
    piano = next((t for t in tracks if "<h3>Piano" in t), "")
    piano_chunk = piano.split("<div class='track'>")[0]
    check("风" not in piano_chunk and "江湖" not in piano_chunk,
          "伴奏轨不串入歌词")

    print("\n[9] --output 文本落盘")
    txt = os.path.join(tmp, "out.txt")
    rc, out, _ = run(exe, mid, "--output", txt)
    check(rc == 0 and os.path.exists(txt), "生成文本文件")
    check("1=C 大调" in open(txt, encoding="utf-8").read(), "文件内有调性行")

    print("\n[10] --help / --version")
    rc, out, _ = run(exe, "--help")
    check(rc == 0 and "--musicxml" in out, "帮助里列出了 --musicxml")
    rc, out, _ = run(exe, "--version")
    check(rc == 0 and re.search(r"\d+\.\d+\.\d+", out) is not None, "版本号正常")

    # ---- [11] 真实录音 MIDI：时值量化 + 首调调性 ----
    # humanized.mid 是 A 羽调式、八分音符、85% 门限（note-off 提前），
    # 这两点正是真实录音 MIDI 最容易让记谱出错的地方。
    print("\n[11] 真实录音 MIDI：时值量化与首调调性")
    human = os.path.join(FIXTURES, "humanized.mid")
    if not os.path.exists(human):
        check(False, "素材 humanized.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, human)
        check(rc == 0, "退出码为 0", err)
        # 五声音阶 + 终止音 A -> a 小调；首调记作 1=C，主音落在 6 上
        check("1=C" in out, "a 小调记为 1=C（首调）", out[:160])
        check("小调" in out and "主音=6" in out, "调性描述标出主音=6")
        # 门限 0.425 拍的音若不做量化，会被判成附点十六分 "=."
        check("=." not in out, "八分音符未被误判成附点十六分", out[:400])
        check(re.search(r"6_\s+5_\s+3_\s+5_", out) is not None,
              "音符 A G E G 都记成八分音符", out[:400])
        check(re.search(r"6\s+-", out) is not None, "终止音 A 为二分音符（6 -）", out[-80:])

        # --no-quantize 应保留原始时长（此时就会出现非八分音符）
        rc2, out2, _ = run(exe, human, "--no-quantize")
        check(rc2 == 0 and out2 != out, "关闭量化后输出与量化版不同")

        # MusicXML 必须带 attributes，否则 MuseScore 打不开
        hx = os.path.join(tmp, "h.xml")
        rc3, _, _ = run(exe, human, "--musicxml", hx)
        body = open(hx, encoding="utf-8").read()
        check(rc3 == 0 and "<divisions>960</divisions>" in body,
              "开头全是休止时仍写出 <divisions>")
        check("<mode>minor</mode>" in body, "调式写成 minor")
        check("<time><beats>4</beats>" in body, "拍号已写出")

    print("\n[12] 打击乐轨（MIDI 通道 10）")
    drums = os.path.join(FIXTURES, "drums.mid")
    if not os.path.exists(drums):
        check(False, "素材 drums.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, drums)
        check(rc == 0, "带鼓轨的文件能正常转换", err)
        check("192 个打击乐音符" in out, "提示跳过了 192 个打击乐音符", out[:200])
        check("[Drums]" not in out, "谱面里没有鼓轨")
        # 鼓的最低音是底鼓 36 -> 简谱 1,, ；旋律最低是 C4 -> 1，
        # 出现 1,, 就说明鼓的音高混进谱面了
        check("1,," not in out, "鼓的音高没有混进简谱")
        # 旋律轨必须是单声部的 Vocal，而不是音符更多的 Piano 或鼓
        check("[Vocal] (8 音符) · 和弦" in out,
              "旋律轨判给单声部的 Vocal（而不是音符更多的伴奏/鼓）", out[:400])
        # 和弦标记：连续同一和弦只标一次，8 小节全是 C 时只出现一次
        check(out.count("[C]") == 1,
              f"连续相同和弦只标一次，实际出现 {out.count('[C]')} 次")

        hx = os.path.join(tmp, "drums.html")
        rc2, _, _ = run(exe, drums, "--html", hx)
        html = open(hx, encoding="utf-8").read()
        check(rc2 == 0 and "已跳过鼓轨" in html, "HTML 页眉标注已跳过鼓轨")
        check("Drums" not in html, "HTML 里没有鼓轨")
        # 鼓的音高若漏进来，会以极低八度的 1,, 形式出现
        check("1,,</span>" not in html, "HTML 里没有鼓的音高")

    print("\n[13] 减时线连通与歌词跨小节")
    # 同一拍里的八分/十六分音符，减时线应连成一条，而不是每个音各画一小段。
    # 三连音尤其明显：1/3 拍的三连八分必须是「一条线 + 一个 3」，
    # 早先按时值表取最近档位会画成附点十六分——两条线外加一个附点。
    bh = os.path.join(tmp, "beam.html")
    rc, _, err = run(exe, os.path.join(FIXTURES, "sample.mid"), "--html", bh,
                     "--lyrics", os.path.join(FIXTURES, "sample.lrc"))
    html = open(bh, encoding="utf-8").read()
    check(rc == 0, "示例文件能导出 HTML", err)
    check("class='bgrp'" in html, "同一拍的音符共用一个减时线组")
    tip = html.find("class='trip'")
    check(tip > 0, "三连音标记仍在")
    if tip > 0:
        grp = inner_span(html[tip:], "class='bgrp'")
        check(grp.count("class='note eighth'") == 3, "三连八分用一条减时线（eighth）",
              grp[:300])
        check("dotted" not in grp, "三连音没有多余的附点", grp[:300])
        check(grp.count("class='note") == 3, "三个音共用一个减时线组", grp[:300])

    # 一字多音：一行词跨两小节、音数多于字数时，两小节都应该有词。
    # 早先按「第 k 个音领第 k 个字」顺序发，四个字会被第一小节一次吃完。
    mel = os.path.join(FIXTURES, "melisma.mid")
    if not os.path.exists(mel):
        check(False, "素材 melisma.mid 存在（先跑 make_fixture.py）")
    else:
        mh = os.path.join(tmp, "melisma.html")
        rc, _, err = run(exe, mel, "--html", mh,
                         "--lyrics", os.path.join(FIXTURES, "melisma.lrc"))
        mhtml = open(mh, encoding="utf-8").read()
        measures = mhtml.split("<span class='measure'>")[1:]
        per_bar = []
        for seg in measures:
            per_bar.append([x for x in re.findall(r"<span class='lyric'>([^<]*)</span>", seg) if x])
        check(rc == 0 and len(per_bar) == 2, "素材渲染成两小节", err)
        check(all(len(b) > 0 for b in per_bar), "两小节都有歌词（不再全挤在第一小节）",
              str(per_bar))
        check(sum(len(b) for b in per_bar) == 4, "四个字一个不丢", str(per_bar))

    print("\n[14] 一小节两个和弦 + 中途变速")
    hm = os.path.join(FIXTURES, "harmony.mid")
    rc, out, err = run(exe, hm)
    check(rc == 0, "带小节内换和弦的文件能正常转换", err)
    # 每小节一个起点和弦 + 一个第三拍和弦，8 小节共 4 种和弦
    check("和弦: 4 个" in out, "识别出 4 种和弦", out[:300])
    check("[C] 1' 7 [Am] 6 5" in out,
          "同一小节里标出 C -> Am（第三拍换和弦）", out[:400])
    check("[F] 1' 7 [G] 6 5" in out,
          "同一小节里标出 F -> G", out[:400])
    # 每个和弦都只标一次：8 小节 x 2
    check(out.count("[C]") == 4 and out.count("[Am]") == 4,
          "C / Am 各标 4 次，实际 %d / %d" % (out.count("[C]"), out.count("[Am]")))
    check("♩=120 → 90(第5小节)" in out,
          "页眉写出变速（不是只印第一个速度）", out[:300])

    hh = os.path.join(tmp, "harmony.html")
    rc2, _, _ = run(exe, hm, "--html", hh)
    html = open(hh, encoding="utf-8").read()
    check(rc2 == 0, "HTML 导出成功")
    # 和弦一律浮在它所管的那个音符上方（标记行是从左往右排的，写在那里
    # 会被速度标记顶右移，和第 3 拍的和弦糊成一团），8 小节 x 2 = 16 个
    check(html.count("class='chordx'") == 16,
          "16 个和弦全部挂在音符上方，实际 %d" % html.count("class='chordx'"))
    check(html.count("class='tempo'") == 2,
          "速度标记只印 2 个（第1、5小节），不是每轨各印一遍，实际 %d"
          % html.count("class='tempo'"))
    check("♩=90" in html, "第 5 小节标出了 ♩=90")

    print("\n[15] 弱起（anacrusis）小节")
    # 歌本里大量曲子从弱拍进唱：`| 5 | 1' 7 6 5 |`。MIDI 里这个 5 就在 tick 0，
    # 第一小节天然不满四拍。照 `tick / 小节长` 划小节线会让整条网格左移一拍，
    # 谱面上每个小节的重音都落到第 4 拍，和弦也跟着错（G 会被算成 Cmaj7）。
    pk = os.path.join(FIXTURES, "pickup.mid")
    if not os.path.exists(pk):
        check(False, "素材 pickup.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, pk)
        check(rc == 0, "弱起文件能正常转换", err)
        check("弱起 1 拍" in out, "判出 1 拍弱起", out[:200])
        # 弱起小节自成一行，之后才是编号 1、2 的完整小节
        check("5  |\n" in out.split("[Vocal]")[1].split("[Piano]")[0],
              "弱起小节单独成栏（没有和后面的音挤在一起）", out[:400])
        check("[C] 1' 7 [Am] 6 5" in out,
              "第一个完整小节的音符与和弦都对", out[:400])
        # 弱起小节的和弦是 G（属和弦），不是把弱起的 5 并进 C 得到的 Cmaj7
        check("[G] 5  |" in out, "弱起小节标 G 而不是 Cmaj7", out[:400])
        check("Cmaj7" not in out, "没有把弱起的音的并进主和弦")

        # 关掉弱起应立即回到旧行为（方便用户对照/回退）
        rc2, out2, _ = run(exe, pk, "--no-pickup")
        check(rc2 == 0 and "弱起" not in out2, "--no-pickup 关闭弱起判定")
        check("5 1'" in out2, "关闭后第一小节变成「5 1' …」（网格未平移）", out2[:400])

        # 手动指定长度优先
        rc3, out3, _ = run(exe, pk, "--pickup", "2")
        check(rc3 == 0 and "弱起 2 拍" in out3, "--pickup 2 手动指定生效")

        # 没有弱起的曲子不能被误判
        rc4, out4, _ = run(exe, os.path.join(FIXTURES, "sample.mid"))
        check(rc4 == 0 and "弱起" not in out4, "普通曲子不误判弱起", out4[:300])

        ph = os.path.join(tmp, "pickup.html")
        rc5, _, _ = run(exe, pk, "--html", ph)
        phtml = open(ph, encoding="utf-8").read()
        check(rc5 == 0 and "弱起 1 拍" in phtml, "HTML 页眉标注弱起")
        vocal = phtml.split("<h3>Vocal")[1].split("<h3>")[0]   # 两条轨各 3 栏
        measures = vocal.split("<span class='measure'>")[1:]
        check(len(measures) == 3, f"旋律轨渲染成 3 栏（弱起 + 2 小节），实际 {len(measures)}")
        check("class='barno'" not in measures[0], "弱起小节不印小节号")
        check("class='barno'>1<" in measures[1] and "class='barno'>2<" in measures[2],
              "第一个完整小节编号为 1、第二个为 2")

        px = os.path.join(tmp, "pickup.xml")
        rc6, _, _ = run(exe, pk, "--musicxml", px)
        body = open(px, encoding="utf-8").read()
        check(rc6 == 0 and 'number="0" implicit="yes"' in body,
              "弱起小节写成 number=0 implicit=yes")
        import xml.etree.ElementTree as ET2
        proot = ET2.fromstring(body)
        durs = []
        for part in proot.findall("part"):
            durs.append([(m.get("number"), sum(int(n.find("duration").text)
                                               for n in m.findall("note")
                                               if n.find("chord") is None))
                         for m in part.findall("measure")])
        check(all(d[0] == ("0", 480) for d in durs),
              "弱起小节时长是 1 拍（不补满 4 拍）", str(durs[:1]))
        check(all(all(x[1] == 1920 for x in d[1:]) for d in durs),
              "之后的完整小节仍是 4 拍", str(durs[:1]))

    print("\n[16] 一字多音：HTML 连音线 + MusicXML 歌词")
    # 8 个音、一行 4 个字 -> 每个字拖两个音。HTML 要给每个字画一条弧线，
    # MusicXML 要写 syllabic=begin/end 加成对的 <slur>；两边共用 assignLyrics。
    mel = os.path.join(FIXTURES, "melisma.mid")
    mlrc = os.path.join(FIXTURES, "melisma.lrc")
    if not os.path.exists(mel) or not os.path.exists(mlrc):
        check(False, "素材 melisma.mid / melisma.lrc 存在")
    else:
        mh = os.path.join(tmp, "melisma_slur.html")
        mx = os.path.join(tmp, "melisma_lyric.xml")
        rc, _, err = run(exe, mel, "--html", mh, "--musicxml", mx, "--lyrics", mlrc)
        h = open(mh, encoding="utf-8").read()
        x = open(mx, encoding="utf-8").read()
        check(rc == 0, "HTML 与 MusicXML 同一次导出都成功", err)
        check("class='staff hasslur'" in h, "有连音线时旋律轨抬高和弦带")
        check(h.count("class='sgrp'") == 4, "四个字各一条连音线弧线",
              str(h.count("class='sgrp'")))

        # 每条弧线必须正好圈住两个音符，不能只圈一个、也不能把整小节圈进去
        sizes, pos = [], 0
        while True:
            i = h.find("class='sgrp'", pos)
            if i < 0:
                break
            sizes.append(inner_span(h[i:], "class='sgrp'").count("class='note "))
            pos = i + 1
        check(sizes == [2, 2, 2, 2], "每条弧线正好跨两个音符", str(sizes))

        import xml.etree.ElementTree as ET3
        root = ET3.fromstring(x)      # 顺便验证 XML 良构
        texts = [n.findtext("lyric/text") for n in root.iter("note")
                 if n.find("lyric") is not None and n.findtext("lyric/text")]
        check("".join(texts) == "一二三四", "MusicXML 歌词完整且顺序正确", str(texts))
        syl = [n.findtext("lyric/syllabic") for n in root.iter("note")
               if n.find("lyric") is not None]
        check(syl == ["begin", "end"] * 4, "每字写成 begin/end 一对", str(syl))
        starts = len(root.findall(".//slur[@type='start']"))
        stops = len(root.findall(".//slur[@type='stop']"))
        check(starts == 4 and stops == 4, "slur 起止各 4 个且配对", f"{starts}/{stops}")
        first = next(iter(root.iter("note")), None)
        tags = [c.tag for c in first] if first is not None else []
        check("notations" in tags and "lyric" in tags
              and tags.index("notations") < tags.index("lyric"),
              "notations 写在 lyric 之前（MusicXML 子元素顺序）", str(tags))
        durs = [sum(int(n.find("duration").text) for n in m.findall("note")
                    if n.find("chord") is None)
                for m in root.iter("measure")]
        check(all(v == 1920 for v in durs), "带歌词后每小节仍是 4 拍", str(durs))

        # 不给 --lyrics 要完全回到旧格式
        mx0 = os.path.join(tmp, "melisma_nolyric.xml")
        run(exe, mel, "--musicxml", mx0)
        x0 = open(mx0, encoding="utf-8").read()
        check("<lyric" not in x0 and "<slur" not in x0,
              "不带 --lyrics 的 MusicXML 保持旧格式")

    # ---- 17. MIDI 内置歌词（0x05 lyric meta） ----
    # 卡拉 OK .mid 自带精确 tick 的歌词流，不需要另外找 LRC。事件流里混着
    # @ 元数据、-/_ 延续符、/ 换行、+ 续行前缀、中文标点、英文单词。
    print("\n[17] MIDI 内置歌词（0x05 lyric meta）")
    kar = os.path.join(FIXTURES, "karaoke.mid")
    if not os.path.exists(kar):
        check(False, "素材 karaoke.mid 存在（先跑 make_fixture.py）")
    else:
        kh = os.path.join(tmp, "karaoke.html")
        kx = os.path.join(tmp, "karaoke.xml")
        rc, out, err = run(exe, kar, "--html", kh, "--musicxml", kx)
        check(rc == 0, "带内置歌词的 MIDI 能正常转换", err)
        check("MIDI 内置歌词: 5 个音节" in out, "控制台提示已自动加载内置歌词")
        h = open(kh, encoding="utf-8").read()
        check("歌词：MIDI 内置（5 音节）" in h, "页眉标注歌词来源")
        words = [w for w in re.findall(r"<span class='lyric'>([^<]*)</span>", h) if w]
        check(words == ["你", "好", "世", "界", "Hello"],
              "五个音节（含英文单词）按 tick 精确落位", str(words))
        check(h.count("class='sgrp'") == 3,
              "3 条连音线（你/好/Hello 各拖两个音）", str(h.count("class='sgrp'")))

        import xml.etree.ElementTree as ET4
        root = ET4.fromstring(open(kx, encoding="utf-8").read())
        texts = [n.findtext("lyric/text") for n in root.iter("note")
                 if n.find("lyric") is not None and n.findtext("lyric/text")]
        check("".join(texts) == "你好世界Hello", "MusicXML 歌词完整且顺序正确", str(texts))
        syl = [n.findtext("lyric/syllabic") for n in root.iter("note")
               if n.find("lyric") is not None]
        check(syl == ["begin", "end", "begin", "end", "single", "single",
                      "begin", "end"],
              "一字多音写 begin/end，一音一字写 single", str(syl))
        st = len(root.findall(".//slur[@type='start']"))
        sp = len(root.findall(".//slur[@type='stop']"))
        check(st == 3 and sp == 3, "3 对 slur 起止配对", f"{st}/{sp}")
        durs = [sum(int(n.find("duration").text) for n in mm.findall("note")
                    if n.find("chord") is None) for mm in root.iter("measure")]
        check(all(v == 1920 for v in durs), "带内置歌词后每小节仍是 4 拍", str(durs))

        koff = os.path.join(tmp, "karaoke_off.html")
        rc2, out2, _ = run(exe, kar, "--no-midi-lyrics", "--html", koff)
        hoff = open(koff, encoding="utf-8").read()
        check(rc2 == 0 and "已忽略 MIDI 内置歌词" in out2, "--no-midi-lyrics 生效")
        check(not [w for w in re.findall(r"<span class='lyric'>([^<]*)</span>", hoff) if w]
              and "歌词：MIDI 内置" not in hoff,
              "关掉后没有任何歌词字，页眉也不再标注来源")

        kext = os.path.join(tmp, "karaoke_ext.html")
        rc3, out3, _ = run(exe, kar, "--html", kext,
                           "--lyrics", os.path.join(FIXTURES, "melisma.lrc"))
        hext = open(kext, encoding="utf-8").read()
        check(rc3 == 0 and "外部歌词优先" in out3, "显式 LRC 时控制台说明优先级")
        check("歌词：外部 LRC" in hext and "<span class='lyric'>一</span>" in hext
              and "<span class='lyric'>你</span>" not in hext,
              "谱面用的是外部 LRC 而不是内置歌词")

        snoly = os.path.join(tmp, "sample_nolyrics.xml")
        run(exe, os.path.join(FIXTURES, "sample.mid"), "--musicxml", snoly)
        xs = open(snoly, encoding="utf-8").read()
        check("<lyric" not in xs and "<slur" not in xs,
              "无内置歌词的素材仍导出无词 MusicXML")

    print("\n[18] 中途变拍号 + 变速 + 鼓轨 + 内置歌词（混合压力素材）")
    import xml.etree.ElementTree as ET
    meter = os.path.join(FIXTURES, "meter.mid")
    if not os.path.exists(meter):
        check(False, "素材 meter.mid 存在（先跑 make_fixture.py）")
    else:
        mh = os.path.join(tmp, "meter.html")
        mx = os.path.join(tmp, "meter.xml")
        rc, out, err = run(exe, meter, "--html", mh, "--musicxml", mx)
        check(rc == 0, "混合素材能正常转换", err)
        check("拍号=4/4 → 3/4(第3小节) → 6/8(第5小节)" in out,
              "控制台报出完整拍号摘要，而不是只报最后一个 6/8")
        check("拍号变化 2 处" in out, "控制台提示按段划分小节线")
        check("弱起" not in out and "弱起" not in open(mh, encoding="utf-8").read(),
              "不再误判出「弱起 4 拍」")
        check("已跳过 6 个打击乐音符" in out, "鼓轨仍被正确丢弃")

        txt = out
        check("[3/4]" in txt and "[6/8]" in txt,
              "文本谱在变化小节印出新拍号")
        check(txt.count("|") >= 6, "谱面按 6 个小节划分", str(txt.count("|")))

        h = open(mh, encoding="utf-8").read()
        check("4/4 → 3/4(第3小节) → 6/8(第5小节)" in h, "HTML 页眉写出拍号摘要")
        check(h.count("class='meter'>") == 2 and
              "class='meter'>3/4" in h and "class='meter'>6/8" in h,
              "HTML 恰好两个拍号标签，且标在正确的小节")
        check(h.count("class='tempo'>") == 2, "变速标记与拍号标记共存")
        check(h.count("<span class='measure'>") == 12,
              "两条轨各 6 小节，没有跳号或补错", str(h.count("<span class='measure'>")))
        check(h.count("歌词：MIDI 内置（24 音节）") == 1 and h.count("class='sgrp'") == 2,
              "变拍号下内置歌词仍能对齐，且只有 2 组一字多音")

        root = ET.parse(mx).getroot()
        parts = root.findall("part")
        check(len(parts) == 2, "MusicXML 两个声部（鼓轨已丢弃）", str(len(parts)))
        p1 = parts[0].findall("measure")
        check(len(p1) == 6, "6 个小节", str(len(p1)))
        durs = [sum(int(n.find("duration").text) for n in m.findall("note")
                    if n.find("chord") is None) for m in p1]
        check(durs == [1920, 1920, 1440, 1440, 1440, 1440],
              "每小节时长跟着拍号变（4/4=1920、3/4=6/8=1440）", str(durs))
        times = [(m.get("number"),
                  (m.find("attributes/time").findtext("beats") + "/" +
                   m.find("attributes/time").findtext("beat-type"))
                  if m.find("attributes/time") is not None else None)
                 for m in p1]
        check([t for _, t in times if t] == ["4/4", "3/4", "6/8"] and
              [n for n, t in times if t] == ["1", "3", "5"],
              "只在第 1/3/5 小节重写 <time>，值分别是 4/4、3/4、6/8", str(times))
        lyr = "".join(n.findtext("lyric/text") or ""
                      for n in p1[0].iter("note") if n.find("lyric") is not None)
        check(lyr == "春风又", "MusicXML 第一小节歌词正确", lyr)
        sl = [x.get("type") for x in parts[0].iter("slur")]
        check(sl.count("start") == 2 and sl.count("stop") == 2,
              "2 对 slur 起止配对", str(sl))

        # 单拍号素材不受影响
        sh = os.path.join(tmp, "sample_meter_reg.html")
        sx = os.path.join(tmp, "sample_meter_reg.xml")
        run(exe, os.path.join(FIXTURES, "sample.mid"), "--html", sh, "--musicxml", sx)
        shs = open(sh, encoding="utf-8").read()
        check("class='meter'>" not in shs and "拍号变化" not in shs,
              "单拍号素材不出现拍号标签")
        sroot = ET.parse(sx).getroot()
        per_part = [sum(1 for m in p.findall("measure")
                        if m.find("attributes/time") is not None)
                    for p in sroot.findall("part")]
        check(per_part and all(v == 1 for v in per_part),
              "单拍号 MusicXML 每个声部只写一次 <time>", str(per_part))

    print("\n[19] 中途转调 1=C → 1=D → 1=F（调号 map 全链路）")
    mod = os.path.join(FIXTURES, "modulation.mid")
    if not os.path.exists(mod):
        check(False, "素材 modulation.mid 存在（先跑 make_fixture.py）")
    else:
        mh = os.path.join(tmp, "modulation.html")
        mx = os.path.join(tmp, "modulation.xml")
        rc, out, err = run(exe, mod, "--html", mh, "--musicxml", mx)
        check(rc == 0, "转调素材能正常转换", err)
        check("1=C 大调 → 1=D(第5小节) → 1=F(第9小节)" in out,
              "文本谱页眉写出完整转调链")
        check("转调 2 处" in out, "控制台提示唱名按各段调号换算")
        check("[转1=D]" in out and "[转1=F]" in out,
              "文本谱在第 5 / 第 9 小节标注转调")
        # 三段旋律每小节第一个音都是 C4，唱名必须各不相同
        check("1 2 3 4" in out and "#6, 1 2 3" in out and "5, 1 2 3" in out,
              "同一个 C4 在三段分别记作 1 / #6, / 5,")
        check("[Bb]" in out and "[A#]" not in out,
              "降号调里 Bb 和弦不写成 A#")

        h = open(mh, encoding="utf-8").read()
        check("1=C 大调 → 1=D(第5小节) → 1=F(第9小节)" in h,
              "HTML 页眉写出完整转调链")
        check(h.count("class='keychg'>") == 4,
              "两条轨各 2 个转调标签", str(h.count("class='keychg'>")))
        check("class='keychg'>转 1=D" in h and "class='keychg'>转 1=F" in h,
              "转调标签内容正确")
        check(".keychg{" in h, "转调标签有独立样式")
        check(h.count("<span class='measure'>") == 24,
              "两条轨各 12 小节，没有跳号", str(h.count("<span class='measure'>")))
        check(h.count("歌词：MIDI 内置（46 音节）") == 1 and h.count("class='sgrp'") == 2,
              "转调下内置歌词与一字多音弧线不受影响")

        root = ET.parse(mx).getroot()
        for idx, p in enumerate(root.findall("part")):
            fifths = [k.findtext("fifths") for k in p.iter("key")]
            modes = [k.findtext("mode") for k in p.iter("key")]
            check(fifths == ["0", "2", "-1"],
                  f"声部{idx + 1} 依次写出 C / D / F 三个调号", str(fifths))
            check(modes == ["major", "major", "major"],
                  f"声部{idx + 1} 的 mode 跟着调号走", str(modes))
            measures = p.findall("measure")
            check(len(measures) == 12, f"声部{idx + 1} 仍是 12 小节",
                  str(len(measures)))
            durs = [sum(int(n.find("duration").text) for n in m.findall("note")
                        if n.find("chord") is None) for m in measures]
            check(all(v == 1920 for v in durs),
                  f"声部{idx + 1} 转调不改变小节时长", str(durs))
            # attributes 子元素顺序固定：key 必须排在 time/divisions 之后、
            # clef 之前；本素材没有变拍号，变化小节只应出现一个 <key>
            ks = [m.get("number") for m in measures if m.find("attributes/key") is not None]
            check(ks == ["1", "5", "9"],
                  f"声部{idx + 1} 只在第 1/5/9 小节写 <key>", str(ks))
            ts = [m.get("number") for m in measures if m.find("attributes/time") is not None]
            check(ts == ["1"], f"声部{idx + 1} 没变拍号就只写一次 <time>", str(ts))
        check(len(root.findall("part")) == 2, "鼓轨仍被丢弃，只剩两个声部")
        sl = [x.get("type") for x in root.iter("slur")]
        check(sl.count("start") == 2 and sl.count("stop") == 2,
              "2 对 slur 起止配对", str(sl))

        # --key 手工覆盖：转调是相对量，整条链跟着平移
        rc2, out2, _ = run(exe, mod, "--key", "G")
        check(rc2 == 0 and "1=G 大调 → 1=A(第5小节) → 1=C(第9小节)" in out2,
              "--key G 时转调链整体平移，没有跳回 MIDI 绝对调号")
        rc3, out3, _ = run(exe, mod, "--key", "Em")
        check("1=G (E小调, 主音=6) → 1=A(第5小节) → 1=C(第9小节)" in out3,
              "小调按首调 1=关系大调 参与转调")

        # --no-key-changes：全曲一个调，XML 也只写一次 <key>
        oh = os.path.join(tmp, "modulation_off.html")
        ox = os.path.join(tmp, "modulation_off.xml")
        run(exe, mod, "--no-key-changes", "--html", oh, "--musicxml", ox)
        rc4, out4, _ = run(exe, mod, "--no-key-changes")
        check("1=C 大调    4/4" in out4 and "→ 1=D" not in out4,
              "--no-key-changes 后页眉只有曲首调性")
        check("转调" not in out4, "--no-key-changes 后不再提示转调")
        check("class='keychg'>" not in open(oh, encoding="utf-8").read(),
              "关闭后 HTML 没有转调标签")
        oroot = ET.parse(ox).getroot()
        off_fifths = [k.findtext("fifths") for k in oroot.iter("key")]
        check(off_fifths == ["0", "0"],
              "关闭后每个声部只写一次曲首调号", str(off_fifths))

        # 只有一个 0x59 的普通素材：行为与旧版完全一致
        sh = os.path.join(tmp, "sample_key_reg.html")
        run(exe, os.path.join(FIXTURES, "sample.mid"), "--html", sh)
        check("class='keychg'>" not in open(sh, encoding="utf-8").read() and
              "→ 1=" not in open(sh, encoding="utf-8").read(),
              "单调号素材不出现任何转调标记")

    # ---- [20] 五声调式：宫音校正 ----
    # pentatonic.mid 故意不写 0x59，KS 七声轮廓会把它判成 G 大调（1=G），
    # 但它是 C 宫系统的 G 徵调式，正确记法应是 1=C。
    print("\n[20] 五声调式宫音校正（C 宫 / G 徵）")
    pent = os.path.join(FIXTURES, "pentatonic.mid")
    if not os.path.exists(pent):
        check(False, "素材 pentatonic.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, pent)
        check(rc == 0, "五声素材正常转换", err)
        check("1=C 大调 · G 徵调式（宫音 C）" in out,
              "自动猜调按宫音校正为 1=C 并标出调式", out[:260])
        check("[C] 1 2 3 5" in out,
              "C D E G 记作 1 2 3 5（终止音 G 不是 1）", out[:400])
        check("五声调式: G 徵调式（宫音 C）" in out, "控制台提示调式判定")

        rc2, off, _ = run(exe, pent, "--no-pentatonic")
        check("1=G 大调    4/4" in off and "调式" not in off,
              "--no-pentatonic 回到 KS 的 1=G 且无调式标注", off[:260])
        check("[C] 4, 5, 6, 1" in off,
              "关闭后 G 被记成 1（对照，证明校正确实生效）", off[:400])

        rc3, gong, _ = run(exe, pent, "--gong", "F")
        check("1=F 大调 · G 商调式（宫音 F）" in gong,
              "--gong F 手工指定宫音并按终止音推调式", gong[:260])
        rc4, manual, _ = run(exe, pent, "--key", "C")
        check("1=C 大调    4/4" in manual and "调式" not in manual,
              "--key 手工指定时不附加调式标注", manual[:260])
        rc5, helpout, _ = run(exe, "--help")
        check("--gong" in helpout and "--no-pentatonic" in helpout,
              "帮助列出五声调式参数")

        ph = os.path.join(tmp, "pentatonic.html")
        px = os.path.join(tmp, "pentatonic.xml")
        rc6, _, _ = run(exe, pent, "--html", ph, "--musicxml", px)
        check(rc6 == 0, "导出五声素材的 HTML / MusicXML")
        body = open(ph, encoding="utf-8").read()
        check("G 徵调式（宫音 C）" in body and "五声宫音校正" in body,
              "HTML 页眉写出调式名与识别来源")
        xroot = ET.parse(px).getroot()
        fifths = [k.findtext("fifths") for k in xroot.iter("key")]
        check(fifths == ["0", "0"],
              "XML 按宫音写 C 大调调号（每个声部一次）", str(fifths))
        words = [w.text for w in xroot.iter("words")]
        # v1.20 起 0x06 marker 也写成 <words>，所以这里不再要求列表只剩一项
        check("G 徵调式（宫音 C）" in words and "Pentatonic" in words,
              "XML 谱头写出调式文字标记与 0x06 段落名", str(words))
        m1 = xroot.find("part/measure")
        tags = [c.tag for c in m1]
        check("direction" in tags and tags.index("attributes") < tags.index("direction"),
              "direction 排在 attributes 之后", str(tags))

    # ---- [21] LRC × tempo map：变速曲目的歌词不能整体错位 ----
    # tempolrc.mid 前 4 小节 120BPM、后 4 小节 60BPM，LRC 时间戳写真实秒。
    # 旧解析器按曲首 120BPM 换算：12 秒 -> tick 11520（差一整小节），
    # 16 / 20 秒甚至超出曲终，后半段歌词直接消失。
    print("\n[21] LRC 时间轴按 tempo map 分段对齐")
    import xml.etree.ElementTree as ETlrc
    tlm = os.path.join(FIXTURES, "tempolrc.mid")
    tll = os.path.join(FIXTURES, "tempolrc.lrc")
    if not (os.path.exists(tlm) and os.path.exists(tll)):
        check(False, "素材 tempolrc.mid / tempolrc.lrc 存在（先跑 make_fixture.py）")
    else:
        expect = ["春风又绿", "江南岸边", "明月何时", "照我还乡",
                  "小楼昨夜", "又东风起", "故国不堪", "回首月中"]

        def bar_lyrics(exe, mid, lrc, tag):
            xmlp = os.path.join(tmp, f"tl_{tag}.xml")
            rc, out, err = run(exe, mid, "--musicxml", xmlp, "--lyrics", lrc)
            if rc != 0:
                return [], out, err
            root = ETlrc.parse(xmlp).getroot()
            return [''.join((ly.findtext("text") or "")
                            for ly in m.iter("lyric"))
                    for m in root.find("part").findall("measure")], out, err

        bars, out, err = bar_lyrics(exe, tlm, tll, "main")
        check(len(bars) == 8, "MusicXML 导出 8 个小节", str(len(bars)))
        check(bars == expect,
              "8 小节歌词逐节落在正确位置（后半段没有跑偏/丢失）", str(bars))
        check("LRC 时间轴: 检测到 1 处变速" in out,
              "控制台提示歌词秒数已按 tempo map 分段换算", out[:300])
        check("♩=120 → 60(第5小节)" in out, "页眉写出变速摘要", out[:300])

        ob = os.path.join(FIXTURES, "tempolrc_offset.lrc")
        bars_off, _, _ = bar_lyrics(exe, tlm, ob, "offset")
        check(bars_off == expect,
              "[offset:-2000] + 大写 OFFSET/元数据 与主 LRC 等价", str(bars_off))

        mb = os.path.join(FIXTURES, "tempolrc_multi.lrc")
        bars_multi, _, _ = bar_lyrics(exe, tlm, mb, "multi")
        check(len(bars_multi) == 8 and bars_multi[0] == "重复一句"
              and bars_multi[4].startswith("重"),
              "一行两个时间戳时，副歌重复句在 0 秒与 8 秒各出现一次",
              str(bars_multi))

        th = os.path.join(tmp, "tempolrc.html")
        run(exe, tlm, "--html", th, "--lyrics", tll)
        body = open(th, encoding="utf-8").read()
        # HTML 里每个字是独立的 <span class='lyric'>，整句不会连成字符串
        words = re.findall(r"class='lyric'>([^<]*)<", body)
        check(''.join(words) == ''.join(expect),
              "HTML 谱面逐字写出变速后的完整 32 个歌词字", ''.join(words))

        # 单调速素材零回归：sample.mid 的 LRC 行为不能因为换了解析器而变
        bars_plain, _, _ = bar_lyrics(
            exe, os.path.join(FIXTURES, "sample.mid"),
            os.path.join(FIXTURES, "sample.lrc"), "plain")
        check(any(b for b in bars_plain),
              "单调速素材的 LRC 仍能正常出词", str(bars_plain))

    # ---- [22] 演奏表情：解析层早就有的数据要真正落到三个输出层 ----
    # expression.mid 把 CC11 渐强/渐弱、note-on 力度重音、等间隔断奏、
    # 弯音上滑/下滑/颤音、0x06 段落标记全混在一条等间隔旋律上。
    print("\n[22] 力度与演奏法三层输出同步")
    import xml.etree.ElementTree as ETex
    exm = os.path.join(FIXTURES, "expression.mid")
    if not os.path.exists(exm):
        check(False, "素材 expression.mid 存在（先跑 make_fixture.py）")
    else:
        exh = os.path.join(tmp, "expression.html")
        exx = os.path.join(tmp, "expression.xml")
        rc, out, err = run(exe, exm, "--html", exh, "--musicxml", exx)
        check(rc == 0, "导出表情素材的 HTML / MusicXML", err[:200])
        check("演奏表情: 力度 11" in out and "渐强 1" in out and
              "渐弱 1" in out and "滑音/颤音 3" in out and "段落 2" in out,
              "CLI 汇总各类表情计数", out[:300])
        check("[Verse]" in out and "[Chorus]" in out and
              "[cresc.]" in out and "[dim.]" in out,
              "文本谱写出段落名与渐强渐弱")
        check("▼" in out and "·" in out and "↗" in out and
              "↘" in out and "tr" in out and "4>" in out,
              "文本谱写出断奏/重音/上滑/下滑/颤音")
        piano = out.split("[Piano]")[-1] if "[Piano]" in out else ""
        check("↘" not in piano and "↗" not in piano and "tr" not in piano,
              "伴奏轨不继承旋律的弯音记号（按通道索引）", piano[:200])

        body = open(exh, encoding="utf-8").read()
        for cls, label in (("section", "段落名"), ("wedge", "渐强渐弱"),
                           ("dyn", "力度"), ("artic", "演奏法")):
            check(f"class='{cls}'>" in body, f"HTML 写出{label}标记")
        check("Verse" in body and "Chorus" in body, "HTML 段落名内容正确")
        check("渐强 1" in body and "段落 2" in body, "HTML 页眉写出表情摘要")
        check(body.count("class='artic'>") == 9,
              "HTML 演奏法记号共 9 处（4 重音 + 2 断奏 + 3 弯音）",
              str(body.count("class='artic'>")))

        root = ETex.parse(exx).getroot()
        dyns = [c.tag for d in root.iter("dynamics") for c in d]
        check("p" in dyns and "ff" in dyns, "MusicXML 写出 <dynamics>")
        check([w.get("type") for w in root.iter("wedge")] ==
              ["crescendo", "stop", "diminuendo", "stop"],
              "MusicXML 渐强渐弱的 start/stop 成对")
        words = [w.text for w in root.iter("words")]
        check("Verse" in words and "Chorus" in words, "MusicXML 写出段落名")
        arts = [c.tag for a in root.iter("articulations") for c in a]
        check("staccatissimo" in arts and "staccato" in arts and
              arts.count("accent") == 4, "MusicXML 断奏与 4 个重音", str(arts))
        check(len(list(root.iter("slide"))) == 4 and
              len(list(root.iter("trill-mark"))) == 1,
              "MusicXML 上滑/下滑用 slide 成对，颤音用 trill-mark")
        sounds = [s.get("dynamics") for s in root.iter("sound")
                  if s.get("dynamics")]
        check(bool(sounds) and all(0 < int(s) <= 100 for s in sounds),
              "MusicXML 试听力度百分比合法", str(sounds))

        # 关掉开关后三层必须一起静默，不能只关掉其中一两个
        oh = os.path.join(tmp, "ex_off.html")
        ox = os.path.join(tmp, "ex_off.xml")
        rc2, out2, _ = run(exe, exm, "--no-dynamics", "--html", oh, "--musicxml", ox)
        offbody = open(oh, encoding="utf-8").read()
        offroot = ETex.parse(ox).getroot()
        check(rc2 == 0 and "演奏表情:" not in out2,
              "--no-dynamics 关闭控制台摘要")
        check("class='section'" not in offbody and "class='artic'" not in offbody
              and "class='wedge'" not in offbody,
              "--no-dynamics 的 HTML 无表情标记")
        check(not list(offroot.iter("dynamics")) and
              not list(offroot.iter("wedge")) and
              not list(offroot.iter("articulations")),
              "--no-dynamics 的 MusicXML 无表情元素")
        _, help_out, _ = run(exe, "--help")
        check("--no-dynamics" in help_out, "帮助列出 --no-dynamics")

    # ---- [23] 声部乐器标注 + 倚音（装饰音） ----
    # parts.mid：Vocal=长笛(73) 带一个 B3->C4 倚音、Piano=钢琴(0) 柱式和弦、
    # Bass=原声贝斯(32) 低音线。验证「已解析未使用」的 0xC0 / 0x51 数据
    # 真正落到谱面，倚音识别按通道收紧、不误判快速经过音。
    print("\n[23] 声部乐器标注 + 倚音（装饰音）")
    import xml.etree.ElementTree as ETpart
    parts = os.path.join(FIXTURES, "parts.mid")
    if not os.path.exists(parts):
        check(False, "素材 parts.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, parts)
        check(rc == 0, "带声部标注的 MIDI 能正常转换", err[:200])
        check("声部：Vocal·长笛，Piano·钢琴，Bass·原声贝斯" in out,
              "文本谱页眉写出声部与 GM 乐器名", out[:300])
        # 倚音写成 (装饰音数字) 直接贴在主音前，例如 B3(7,) -> C4(1)
        check("(7,)1" in out,
              "文本谱写出倚音 B3->C4： (7,)1", out[:500])

        ph = os.path.join(tmp, "parts.html")
        px = os.path.join(tmp, "parts.xml")
        rc2, _, _ = run(exe, parts, "--html", ph, "--musicxml", px)
        check(rc2 == 0, "导出 parts.mid 的 HTML / MusicXML")
        hbody = open(ph, encoding="utf-8").read()
        check("声部：Vocal·长笛" in hbody, "HTML 页眉写出声部乐器标注")
        check("class='grace'>" in hbody, "HTML 写出倚音 <span class='grace'>")
        xroot = ETpart.parse(px).getroot()
        inst = [t.text for t in xroot.iter("instrument-name")]
        check("长笛" in inst and "钢琴" in inst and "原声贝斯" in inst,
              "MusicXML part-list 写出每个声部的 GM 乐器中文名", str(inst))
        n_grace = len(list(xroot.iter("grace")))
        check(n_grace == 1, "MusicXML 写出一个倚音 <grace/>", str(n_grace))

        # 真实曲目零回归：等间隔四分音符流（样本 / 真实录音）不应被误标成倚音。
        # 「已解析未使用」的字段（0xC0 音色号等）在无对应事件时不凭空产出标注，
        # 这一点由单元测试的构造样例锁死；这里用真实曲风样本做冒烟兜底。
        rc3, out3, _ = run(exe, os.path.join(FIXTURES, "sample.mid"))
        check("(7,)1" not in out3 and "(6,)1" not in out3,
              "普通等间隔旋律不被误标倚音", out3[:500])

    # ---- [24] 和弦转位 + 七和弦扩充 + MusicXML 和弦符号 ----
    # chords.mid 的四个和声窗：C/E（转位）、Bm7b5（半减七）、G7sus4（挂四七）、
    # 原位 C。转位只能由低音判定——音级集合完全相同，谱面却必须写 C/E。
    print("\n[24] 和弦转位 + 七和弦扩充 + MusicXML 和弦符号")
    import xml.etree.ElementTree as ETch
    cmid = os.path.join(FIXTURES, "chords.mid")
    if not os.path.exists(cmid):
        check(False, "素材 chords.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, cmid)
        check(rc == 0, "带转位和弦的 MIDI 能正常转换", err[:200])
        check("[C/E]" in out, "E 在低音时标出 C/E 转位", out[:400])
        check("[Bm7b5]" in out, "半减七标出 Bm7b5", out[:400])
        check("[G7sus4]" in out, "挂四七标出 G7sus4", out[:400])
        # 原位 C 不能带斜杠：整份谱里 "[C]" 只出现一次
        check(out.count("[C]") == 1,
              f"原位 C 不写斜杠，[C] 实际出现 {out.count('[C]')} 次")

        ch = os.path.join(tmp, "chords.html")
        cx = os.path.join(tmp, "chords.xml")
        rc2, _, err2 = run(exe, cmid, "--html", ch, "--musicxml", cx)
        check(rc2 == 0, "导出 chords.mid 的 HTML / MusicXML", err2[:200])
        hbody = open(ch, encoding="utf-8").read()
        check("C/E" in hbody and "Bm7b5" in hbody,
              "HTML 和弦带同步写出转位与半减七")

        xroot = ETch.parse(cx).getroot()
        harmonies = list(xroot.iter("harmony"))
        check(len(harmonies) == 4,
              f"MusicXML 写出 4 个 <harmony>，实际 {len(harmonies)}")
        bass = [b.text for b in xroot.iter("bass-step")]
        check(bass == ["E"], f"转位和弦写出 <bass-step>E</bass-step>，实际 {bass}")
        kinds = [k.get("text") for k in xroot.iter("kind")]
        check("m7b5" in kinds and "7sus4" in kinds,
              f"<kind text> 保留 m7b5 / 7sus4 显示后缀，实际 {kinds}")

    # ---- [25] 渐快 / 渐慢折叠 ----
    # tempo_ramp.mid 里 accel. 13 个节点、rit. 13 个节点，中间夹恒定速度，
    # 最后阶跃回 120。旧版会把 28 个点全当变速列进页眉（五条全指同一小节），
    # HTML 每小节采样一次速度，MusicXML 则因为节点不落在小节线上而整段丢失。
    print("\n[25] 渐快 / 渐慢：连续 set_tempo 收敛成 rit. / accel.")
    rmid = os.path.join(FIXTURES, "tempo_ramp.mid")
    if not os.path.exists(rmid):
        check(False, "素材 tempo_ramp.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, rmid)
        check(rc == 0, "带渐变速度的 MIDI 能正常转换", err[:200])
        head = out.split("----")[0]
        check("accel. 100→140" in head, "页眉写出渐快 accel. 100→140", head[:300])
        check("rit. 140→80" in head, "页眉写出渐慢 rit. 140→80", head[:300])
        check(head.count("第2小节") <= 1,
              f"页眉不再重复罗列同一小节，实际 {head.count('第2小节')} 次")
        # 28 个自动化节点不能逐点出现在页眉里：分隔符 " → " 只有 3 个
        # （渐变内部的 100→140 没有空格，不会误计）
        check(head.count(" → ") <= 4,
              f"页眉分隔符 <= 4（折叠生效），实际 {head.count(' → ')}")

        rh = os.path.join(tmp, "ramp.html")
        rx = os.path.join(tmp, "ramp.xml")
        rc2, _, err2 = run(exe, rmid, "--html", rh, "--musicxml", rx)
        check(rc2 == 0, "导出 tempo_ramp.mid 的 HTML / MusicXML", err2[:200])
        hbody = open(rh, encoding="utf-8").read()
        temples = re.findall(r"<span class='tempo[^']*'>([^<]*)</span>", hbody)
        check(len(temples) == 4,
              f"HTML 只有 4 个速度标记（曲首 + 渐快 + 渐慢 + a tempo），实际 {temples}")
        check(any("accel." in t for t in temples), "HTML 写出 accel. 记号")
        check(any("rit." in t for t in temples), "HTML 写出 rit. 记号")
        # 渐变段不再逐小节采样：谱面上只该有曲首 100 与 a tempo 120 两个
        # 具体 BPM，中间那些 116 / 108 之类的采样值一个都不该出现
        bpms = set(re.findall(r"♩=(\d+)", " ".join(temples)))
        check(bpms == {"100", "120"},
              f"HTML 只出现曲首与 a tempo 的具体 BPM，实际 {sorted(bpms)}")

        import xml.etree.ElementTree as ETt
        troot = ETt.parse(rx).getroot()
        words = [w.text or "" for w in troot.iter("words")]
        check("rit." in words, f"MusicXML 写出 <words>rit.</words>，实际 {words}")
        check("accel." in words, f"MusicXML 写出 <words>accel.</words>，实际 {words}")
        tempos = [(m.findtext("beat-unit"), m.findtext("per-minute"))
                  for m in troot.iter("metronome")]
        check(len(tempos) <= 6,
              f"<metronome> 不超过 6 个（不是逐节点），实际 {tempos}")
        check(("quarter", "100") in tempos and ("quarter", "120") in tempos,
              f"曲首与 a tempo 的阶跃速度仍在，实际 {tempos}")

        # 真实曲目零回归：单调速文件不应该冒出速度记号
        smid = os.path.join(FIXTURES, "sample.mid")
        rc3, sout, _ = run(exe, smid)
        check(rc3 == 0 and "rit." not in sout and "accel." not in sout,
              "单调速素材 sample.mid 不产生渐快渐慢记号")

    # ---- [26] 长音跨小节：延音线而不是休止符 ----
    # long_tie.mid 的旋律轨在第 4 小节放一个 6 拍长音、在第 7 小节放一个
    # 10 拍长音，钢琴轨每小节一个 4 拍柱式和弦（复调，用来验证长音不被门限放大）。
    # 旧版把超过全音符的时值钳到 4 拍，长音的余下时间变成休止符：
    #   | 3 - - - | 0 - 4 5 |     而不是     | 3 - - - | - - 4 5 |
    print("\n[26] 长音跨小节：延音线（- / <tie>）而不是休止符")
    lmid = os.path.join(FIXTURES, "long_tie.mid")
    if not os.path.exists(lmid):
        check(False, "素材 long_tie.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, lmid)
        check(rc == 0, "带跨小节长音的 MIDI 能正常转换", err[:200])
        # 只看旋律轨那一段
        sec = out.split("[Vocal]")[1].split("[Piano]")[0] if "[Vocal]" in out else out
        lines = [re.sub(r"\[[^\]]*\]\s*", "", ln).strip() for ln in sec.splitlines()]
        lines = [ln for ln in lines if ln and "|" in ln]
        check(len(lines) == 9, f"旋律轨共 9 小节，实际 {len(lines)}: {lines}")

        def barsig(i):
            return lines[i].replace("|", "").replace(" ", "")

        check(barsig(3) == "3---", f"6 拍长音首段写满一小节，实际 {lines[3]!r}")
        check(barsig(4).startswith("--45"),
              f"长音后段在下一小节续写「- -」，不是休止符，实际 {lines[4]!r}")
        check("0" not in barsig(4), f"延音段里不能出现休止符 0，实际 {lines[4]!r}")
        check(barsig(7) == "----", f"10 拍长音跨过整小节，实际 {lines[7]!r}")
        check(barsig(8).startswith("--76"),
              f"10 拍长音在第三小节收尾，实际 {lines[8]!r}")
        # 长音不该被门限放大：每个小节必须正好 4 拍。
        # 这份素材的音符全是四分及更长，所以「拍数 = token 数」。
        for i, ln in enumerate(lines):
            toks = re.sub(r"\[[^\]]*\]", "", ln).replace("|", " ").split()
            check(len(toks) == 4,
                  f"第 {i + 1} 小节正好 4 拍（长音没被放大），实际 {len(toks)}: {ln}")

        lh = os.path.join(tmp, "longtie.html")
        lx = os.path.join(tmp, "longtie.xml")
        rc2, _, err2 = run(exe, lmid, "--html", lh, "--musicxml", lx)
        check(rc2 == 0, "导出 long_tie.mid 的 HTML / MusicXML", err2[:200])

        hbody = open(lh, encoding="utf-8").read()
        dash_cells = hbody.count("<span class='num'>-</span>")
        check(dash_cells == 8,
              f"HTML 里单独写出的延音线共 8 个（6 拍 2 个 + 10 拍 6 个），实际 {dash_cells}")

        import xml.etree.ElementTree as ETl
        lroot = ETl.parse(lx).getroot()
        starts = len(lroot.findall(".//tie[@type='start']"))
        stops = len(lroot.findall(".//tie[@type='stop']"))
        check(starts == 3, f"MusicXML 写出 3 个 <tie type=start>，实际 {starts}")
        check(stops == 3, f"MusicXML 写出 3 个 <tie type=stop>，实际 {stops}")
        check(len(lroot.findall(".//tied[@type='start']")) == 3,
              "MusicXML 的 <tied> 与 <tie> 同步，记谱软件才画得出弧线")

        # 钢琴轨（复调）的 4 拍和弦不能被门限放大成 4.76 拍。
        # 柱式和弦在文本谱里写成 `1, - - - 3, - - - 5, - - -`（三音 × 4 拍 = 12 个 token），
        # 一旦被放大就会多出 `-.` 这类续接 token，行内 token 数随之变多。
        psec = out.split("[Piano]")[1] if "[Piano]" in out else ""
        plines = [ln for ln in psec.splitlines() if "|" in ln]
        check(len(plines) == 9, f"钢琴轨 9 个柱式和弦各占一行，实际 {len(plines)}")
        bad = [ln for ln in plines
               if len(ln.replace("|", " ").split()) != 12 or "0" in ln]
        check(not bad,
              f"复调轨的每个和弦正好 12 个 token（三音各 4 拍），实际异常行 {bad[:2]}")

    # ---- [27] 同轨复调：一条轨里两个声部 ----
    # polyphony.mid 是钢琴左右手写进同一个轨：右手四分音符，左手长音铺底，
    # 时值各不相同、互相重叠。旧版把「同拍不同值」也当成和弦，长音叠短音
    # 写进同一格，4/4 的小节被撑成 8 拍（<chord/> 成员时值不等），
    # 导出的 MusicXML 是坏文件。现在按声部拆开、用 <backup> 回退游标。
    print("\n[27] 同轨复调（一条轨两个声部）")
    poly = os.path.join(FIXTURES, "polyphony.mid")
    rc, out, err = run(exe, poly)
    check(rc == 0, "解析 polyphony.mid", err[:200])

    check("[Piano · 声部1]" in out and "[Piano · 声部2]" in out,
          "文本谱按声部拆成两个谱行，实际:\n" + out[-400:])

    # 每个声部块里的每一行都必须是 4 拍：声部拆对了，小节线才对得上
    blocks = re.split(r"\[([^\]]*声部\d)\]", out)
    ok_rows = True
    bad_rows = []
    for i in range(1, len(blocks) - 1, 2):
        for ln in blocks[i + 1].splitlines():
            if "|" not in ln:
                continue
            toks = re.sub(r"\[[^\]]*\]", "", ln).replace("|", " ").split()
            if len(toks) != 4:
                ok_rows = False
                bad_rows.append(f"{blocks[i]}: {ln}")
    check(ok_rows, f"两个声部每小节都正好 4 个 token，实际异常 {bad_rows[:2]}")

    # 低声部在第 2 小节第 3 拍才进来，前面必须是休止而不是空白
    check(re.search(r"\[Piano · 声部2\].*?\n(0 - 3, -|0 - 3,)", out, re.S) is not None,
          "声部 2 的后半小节从休止起，位置正确")

    px = os.path.join(tmp, "poly.xml")
    ph = os.path.join(tmp, "poly.html")
    rc2, _, err2 = run(exe, poly, "--musicxml", px, "--html", ph)
    check(rc2 == 0, "导出 polyphony.mid 的 MusicXML / HTML", err2[:200])

    xml = open(px, encoding="utf-8").read()
    check(xml.count("<backup>") == 4,
          f"每个含第二声部的小节各一次 <backup>，实际 {xml.count(chr(60) + 'backup' + chr(62))}")
    check("<voice>2</voice>" in xml, "第二个声部写成 <voice>2</voice>")

    # 逐小节核对总时值：<chord/> 不推进时间，<backup> 回退
    div = int(re.search(r"<divisions>(\d+)</divisions>", xml).group(1))
    bad = []
    for body in re.findall(r"<measure [^>]*>(.*?)</measure>", xml, re.S):
        pos = mx = 0
        for frag in re.findall(
                r"<note>.*?</note>|<backup>.*?</backup>", body, re.S):
            d = re.search(r"<duration>(\d+)</duration>", frag)
            if not d:
                continue
            dur = int(d.group(1))
            if frag.startswith("<backup>"):
                pos -= dur
            elif "<chord/>" not in frag:
                pos += dur
                mx = max(mx, pos)
        if mx != 4 * div:
            bad.append((mx, 4 * div))
    check(not bad, f"每个小节正好 4 拍（声部叠加不溢出），实际 {bad[:3]}")

    check(xml.count('<tie type="start"') == 1 and xml.count('<tie type="stop"') == 1,
          "跨小节的低音在声部内写成一对延音线")

    html = open(ph, encoding="utf-8").read()
    check(html.count("<div class='staff") == 2,
          f"HTML 拆成两块谱面，实际 {html.count(chr(60) + 'div class=' + chr(39) + 'staff')}")
    check("voiceTag" in html and html.count("声部 2") >= 1,
          "HTML 标题标出声部号")

    # 单声部素材不该被拆：回归锁
    rc3, sample_out, _ = run(exe, os.path.join(FIXTURES, "sample.mid"))
    check("声部1" not in sample_out and "声部2" not in sample_out,
          "单声部素材不拆声部（不出现声部N 标签）")

    print("\n[28] SMPTE 时间基准：division 高字节为负")
    smpte = os.path.join(FIXTURES, "smpte.mid")
    rc, out, err = run(exe, smpte)
    check(rc == 0, "SMPTE 文件解析成功", err[:200])
    check("时间基准 SMPTE 25 fps × 40 tick → 500 tick/拍" in out,
          "报出 SMPTE 基准并折算成 500 tick/拍")
    check("division=500" in out, "division 已折算（不再是无符号的 59176）")
    check("division=59176" not in out, "不再把 0xE728 读成 59176")
    check("♩=120" in out and "→ 400" not in out,
          "中途 400BPM 的 set_tempo 被归一，页眉只报 ♩=120")
    check("声部2" not in out,
          "不再把量化错乱的音符拆成假声部")
    body = out.split("---")[-1]
    check(body.count("|") == 4, f"16 拍划成 4 小节，实际小节线 {body.count('|')}")

    sx = os.path.join(tmp, "smpte.xml")
    rc, _, err = run(exe, smpte, "--musicxml", sx)
    check(rc == 0, "SMPTE 文件导出 MusicXML", err[:200])
    sxml = open(sx, encoding="utf-8").read()
    sdiv = int(re.search(r"<divisions>(\d+)</divisions>", sxml).group(1))
    check(sdiv == 500, f"MusicXML divisions 用折算后的 500，实际 {sdiv}")
    check(sxml.count("<measure ") == 4,
          f"MusicXML 四个小节，实际 {sxml.count(chr(60) + 'measure ')}")
    sbad = []
    for mbody in re.findall(r"<measure [^>]*>(.*?)</measure>", sxml, re.S):
        tot = sum(int(d) for d in re.findall(r"<duration>(\d+)</duration>", mbody)
                  if "<chord/>" not in mbody)
        if tot != 4 * sdiv:
            sbad.append(tot)
    check(not sbad, f"每小节正好 4 拍 = {4 * sdiv} tick，实际 {sbad[:3]}")

    sh = os.path.join(tmp, "smpte.html")
    rc, _, err = run(exe, smpte, "--html", sh)
    check(rc == 0, "SMPTE 文件导出 HTML", err[:200])
    shtml = open(sh, encoding="utf-8").read()
    check("SMPTE 25 fps × 40 tick" in shtml, "HTML 页眉写有时间基准")

    print("\n[29] format 2：多条轨是互相独立的序列")
    f2 = os.path.join(FIXTURES, "format2.mid")
    rc, out, err = run(exe, f2)
    check(rc == 0, "format 2 文件解析成功", err[:200])
    check("format 2（多个独立序列）：共 2 段，只转换轨 0" in out,
          "说明只转了第 0 段")
    check("16 个音符" in out and "24 个音符" not in out,
          "只保留 SongA 的 16 个音符（不再把两段并成 24 个）")
    check("[SongA]" in out and "[SongB]" not in out,
          "另一段没有混进谱面")

    rc, out2, _ = run(exe, f2, "--track", "1")
    check(rc == 0 and "8 个音符" in out2 and "[SongB]" in out2,
          "--track 1 能取到第二段（SongB）")
    check("只转换轨 1" in out2, "--track 后提示跟着改口")

    fx = os.path.join(tmp, "format2.xml")
    rc, _, err = run(exe, f2, "--musicxml", fx)
    check(rc == 0, "format 2 导出 MusicXML", err[:200])
    fxml = open(fx, encoding="utf-8").read()
    fdiv = int(re.search(r"<divisions>(\d+)</divisions>", fxml).group(1))
    check(fxml.count("<measure ") == 4,
          f"只写 SongA 的 4 小节，实际 {fxml.count(chr(60) + 'measure ')}")
    fbad = []
    for mbody in re.findall(r"<measure [^>]*>(.*?)</measure>", fxml, re.S):
        tot = sum(int(d) for d in re.findall(r"<duration>(\d+)</duration>", mbody)
                  if "<chord/>" not in mbody)
        if tot != 4 * fdiv:
            fbad.append(tot)
    check(not fbad, f"每小节正好 4 拍，实际 {fbad[:3]}")

    print("\n[30] 复音触后 0xA0：3 字节事件不能读错")
    at = os.path.join(FIXTURES, "aftertouch.mid")
    if not os.path.exists(at):
        check(False, "素材 aftertouch.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, at)
        check(rc == 0, "含复音触后的文件能正常转换", err[:200])
        check("未提取到音符" not in out,
              "不再因为事件流错位报「未提取到音符」")
        check("16 个音符" in out, "16 个音符一个不少")
        body = out.split("---")[-1]
        check(body.count("|") == 4, f"16 拍划成 4 小节，实际小节线 {body.count('|')}")
        check("声部2" not in out, "时值没被吃掉，不会拆出假声部")

        ax = os.path.join(tmp, "aftertouch.xml")
        rc, _, err = run(exe, at, "--musicxml", ax)
        check(rc == 0, "含复音触后的文件导出 MusicXML", err[:200])
        axml = open(ax, encoding="utf-8").read()
        adiv = int(re.search(r"<divisions>(\d+)</divisions>", axml).group(1))
        check(axml.count("<measure ") == 4,
              f"MusicXML 四个小节，实际 {axml.count(chr(60) + 'measure ')}")
        abad = []
        for mbody in re.findall(r"<measure [^>]*>(.*?)</measure>", axml, re.S):
            tot = sum(int(d) for d in re.findall(r"<duration>(\d+)</duration>", mbody)
                      if "<chord/>" not in mbody)
            if tot != 4 * adiv:
                abad.append(tot)
        check(not abad, f"每小节正好 4 拍，实际 {abad[:3]}")

    print("\n[31] 同音重触发：一个键没抬手又按下")
    rt = os.path.join(FIXTURES, "retrigger.mid")
    if not os.path.exists(rt):
        check(False, "素材 retrigger.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, rt)
        check(rc == 0, "同音重触发的文件能正常转换", err[:200])
        check("8 个音符" in out and "7 个音符" not in out,
              "前一个音没被覆盖掉（8 个而不是 7 个）")
        body = out.split("---")[-1]
        # 两个 C4 首尾相接，谱面应写成「1 -」而不是只剩一个「1」
        check("1 - 3 4" in body, "重触发的两个 C4 合成延音（1 -），音高不丢",
              body.splitlines()[1] if len(body.splitlines()) > 1 else body)

        rx = os.path.join(tmp, "retrigger.xml")
        rc, _, err = run(exe, rt, "--musicxml", rx)
        check(rc == 0, "同音重触发导出 MusicXML", err[:200])
        rxml = open(rx, encoding="utf-8").read()
        # 重触发是两次发声，MusicXML 里就该是两个四分音符（简谱那一侧把相邻
        # 同音合并成「1 -」是显示上的简化，不代表只按了一次）
        cc4 = rxml.count("<step>C</step><octave>4</octave>")
        check(cc4 == 2, f"两个 C4 都写进了 MusicXML，实际 {cc4} 个")
        rdurs = [sum(int(d) for d in re.findall(r"<duration>(\d+)</duration>", mb))
                 for mb in re.findall(r"<measure [^>]*>(.*?)</measure>", rxml, re.S)]
        check(rdurs == [1920, 1920], f"每小节仍是 4 拍，实际 {rdurs}")
        check(rxml.count("<measure ") == 2,
              f"MusicXML 两个小节，实际 {rxml.count(chr(60) + 'measure ')}")

    print("\n[32] 轨尾悬挂音：只按不抬的音不能丢")
    dg = os.path.join(FIXTURES, "dangling.mid")
    if not os.path.exists(dg):
        check(False, "素材 dangling.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, dg)
        check(rc == 0, "悬挂音的文件能正常转换", err[:200])
        check("16 个音符" in out and "15 个音符" not in out,
              "轨尾悬挂音被补收（16 个而不是 15 个）")
        check("1=C 大调" in out, "少一个音不再把调性带偏成 F / D 小调")
        check("弱起" not in out, "也不再误判出「弱起 3 拍」")
        dx = os.path.join(tmp, "dangling.xml")
        rc, _, err = run(exe, dg, "--musicxml", dx)
        check(rc == 0, "悬挂音导出 MusicXML", err[:200])
        dxml = open(dx, encoding="utf-8").read()
        check(dxml.count("<measure ") == 4,
              f"MusicXML 四个小节，实际 {dxml.count(chr(60) + 'measure ')}")

    print("\n[33] 双歌词轨：只取音节最多的一条")
    tl = os.path.join(FIXTURES, "twolyrics.mid")
    if not os.path.exists(tl):
        check(False, "素材 twolyrics.mid 存在（先跑 make_fixture.py）")
    else:
        rc, out, err = run(exe, tl)
        check(rc == 0, "双歌词轨的文件能正常转换", err[:200])
        check("10 个音节" in out and "20 个音节" not in out,
              "只算被采信那条轨的 10 个音节")
        check("另 1 轨的 10 个音节已忽略" in out, "控制台说明忽略了另一条轨")
        tlh = os.path.join(tmp, "twolyrics.html")
        rc, _, err = run(exe, tl, "--html", tlh)
        check(rc == 0, "双歌词轨导出 HTML", err[:200])
        th = open(tlh, encoding="utf-8").read()
        check("歌词：MIDI 内置（10 音节，另 1 轨的 10 个音节已忽略）" in th,
              "页眉的歌词来源也带上忽略说明")
        words = [w for w in re.findall(r"<span class='lyric'>([^<]*)</span>", th) if w]
        check(words == list("春眠不觉晓处处闻"),
              "只剩第一段词，没有和第二段交错", "".join(words))
        check("夜" not in "".join(words), "第二段词（夜来风雨声…）没混进来")

        tlx = os.path.join(tmp, "twolyrics.xml")
        rc, _, err = run(exe, tl, "--musicxml", tlx)
        check(rc == 0, "双歌词轨导出 MusicXML", err[:200])
        import xml.etree.ElementTree as ET5
        root5 = ET5.fromstring(open(tlx, encoding="utf-8").read())
        texts = [n.findtext("lyric/text") for n in root5.iter("note")
                 if n.find("lyric") is not None and n.findtext("lyric/text")]
        check("".join(texts) == "春眠不觉晓处处闻",
              "MusicXML 歌词同样只剩第一段", str(texts))

    import xml.etree.ElementTree as ETC

    print("\n[34] CC64 延音踏板：抬手不等于消音")
    ped = os.path.join(FIXTURES, "pedal.mid")
    rc, out, err = run(exe, ped)
    check(rc == 0, "踏板文件解析成功", err[:200])
    check("延音踏板：补长 6 个音（2 个踏板事件）" in out,
          "CLI 报出补长 6 个音")
    body = out.split("---")[-1]
    check("1= 3= 5=" not in body and "0 - - -" not in body,
          "不再是一串十六分音符加长休止")
    check("1 - - -" in body, "和弦音补成 4 拍全音符")

    px = os.path.join(tmp, "pedal.xml")
    rc, _, err = run(exe, ped, "--musicxml", px)
    check(rc == 0, "踏板文件导出 MusicXML", err[:200])
    root_p = ETC.fromstring(open(px, encoding="utf-8").read())
    pdiv = int(root_p.findtext(".//divisions"))
    pm = list(root_p.iter("measure"))
    check(len(pm) == 2, f"两个小节，实际 {len(pm)}")
    pbad = []
    for m in pm:
        notes = [n for n in m.iter("note") if n.find("rest") is None]
        main = [n for n in notes if n.find("chord") is None]
        total = sum(int(n.findtext("duration")) for n in main)
        if total != 4 * pdiv or len(notes) != 3:
            pbad.append((total, len(notes)))
    check(not pbad, f"每小节 3 个和弦音共 4 拍（含 2 个 <chord/>），实际 {pbad}")

    ph = os.path.join(tmp, "pedal.html")
    rc, _, err = run(exe, ped, "--html", ph)
    check(rc == 0, "踏板文件导出 HTML", err[:200])
    check("延音踏板：补长 6 个音" in open(ph, encoding="utf-8").read(),
          "HTML 页眉写明踏板补长")

    print("\n[35] CC123 All Notes Off：清音要截在正确 tick")
    ao = os.path.join(FIXTURES, "allnotesoff.mid")
    rc, out, err = run(exe, ao)
    check(rc == 0, "清音文件解析成功", err[:200])
    check("强制清音：截尾 1 个音（1 个事件）" in out, "CLI 报出截尾 1 个音")
    check("共提取 7 个音符" in out, "7 个音符都在")
    check("声部2" not in out, "不再因幽灵长音拆出假声部")

    ax = os.path.join(tmp, "allnotesoff.xml")
    rc, _, err = run(exe, ao, "--musicxml", ax)
    check(rc == 0, "清音文件导出 MusicXML", err[:200])
    root_a = ETC.fromstring(open(ax, encoding="utf-8").read())
    adiv = int(root_a.findtext(".//divisions"))
    adurs = [int(n.findtext("duration")) for n in root_a.iter("note")
             if n.find("rest") is None]
    check(len(adurs) == 7 and all(d == adiv for d in adurs),
          f"7 个音符都是 1 拍，实际 {adurs}")
    # 小节 1 在 tick 480~960 本来就是休止（清音之后空了一拍），
    # 所以要把 rest 的时值一起算进来，只排除 <chord/> 的叠加音。
    abad = []
    for m in root_a.iter("measure"):
        main = [n for n in m.iter("note") if n.find("chord") is None]
        if sum(int(n.findtext("duration")) for n in main) != 4 * adiv:
            abad.append(m.get("number"))
    check(not abad, f"每小节音符 + 休止仍是 4 拍，异常小节 {abad}")

    print("\n[36] CC7 主音量：和 CC11 一样是强弱来源")
    cv = os.path.join(FIXTURES, "cc7volume.mid")
    rc, out, err = run(exe, cv)
    check(rc == 0, "CC7 文件解析成功", err[:200])
    check("CC7 主音量：补入 8 个力度点" in out, "CLI 报出补入 8 个力度点")
    check("演奏表情" in out and "渐强" in out, "识别出渐强与力度档位")
    check("[cresc.]" in out and "[p]" in out and "[mf]" in out,
          "谱面印出 cresc. / p / mf")

    ch = os.path.join(tmp, "cc7volume.html")
    rc, _, err = run(exe, cv, "--html", ch)
    check(rc == 0, "CC7 文件导出 HTML", err[:200])
    check("CC7 主音量：补入 8 个力度点" in open(ch, encoding="utf-8").read(),
          "HTML 页眉写明 CC7 力度点")

    cx = os.path.join(tmp, "cc7volume.xml")
    rc, _, err = run(exe, cv, "--musicxml", cx)
    check(rc == 0, "CC7 文件导出 MusicXML", err[:200])
    cxml = open(cx, encoding="utf-8").read()
    check("<dynamics" in cxml or "<other-dynamics" in cxml or "cresc" in cxml,
          "MusicXML 里也有力度 / 渐强方向")

    print("\n[37] 踏板按通道隔离：没踩的通道不补时值")
    pc = os.path.join(FIXTURES, "pedal_channels.mid")
    # 素材刻意稀疏，自动弱起判定会把小节网格左移；用 --no-pickup 固定网格，
    # 本组只关心四个音各自的时值。
    rc, out, err = run(exe, pc, "--no-pickup")
    check(rc == 0, "双通道踏板文件解析成功", err[:200])
    check("延音踏板：补长 1 个音（2 个踏板事件）" in out,
          "只补长通道 0 的那一个音")

    cpx = os.path.join(tmp, "pedal_channels.xml")
    rc, _, err = run(exe, pc, "--no-pickup", "--musicxml", cpx)
    check(rc == 0, "双通道踏板导出 MusicXML", err[:200])
    root_c = ETC.fromstring(open(cpx, encoding="utf-8").read())
    cdurs = sorted(int(n.findtext("duration")) for n in root_c.iter("note")
                   if n.find("rest") is None)
    check(cdurs == [120, 120, 120, 1920],
          f"时值 1920 / 120 / 120 / 120，实际 {cdurs}")

    def xml_pitches(path):
        root = ETC.fromstring(open(path, encoding="utf-8").read())
        return [(n.findtext("pitch/step"), n.findtext("pitch/octave"))
                for n in root.iter("note")
                if n.find("rest") is None and n.find("chord") is None
                and n.find("pitch") is not None]

    print("\n[38] RPN 弯音范围按通道隔离：不同通道不共用同一个 ±N")
    bc = os.path.join(FIXTURES, "bendrange_channels.mid")
    rc, out, err = run(exe, bc, "--no-pickup")
    check(rc == 0, "双通道弯音文件解析成功", err[:200])
    check("弯音范围：±12 半音，按通道与时间生效（1 处设定，1 条通道）" in out,
          "CLI 写明只有一条通道设过 RPN")
    bcx = os.path.join(tmp, "bendrange_channels.xml")
    rc, _, err = run(exe, bc, "--no-pickup", "--musicxml", bcx)
    check(rc == 0, "双通道弯音导出 MusicXML", err[:200])
    pb = xml_pitches(bcx)
    check(pb == [("C", "5"), ("D", "4")],
          f"通道 0 升到 C5、通道 1 只升到 D4，实际 {pb}")
    bch = os.path.join(tmp, "bendrange_channels.html")
    rc, _, err = run(exe, bc, "--html", bch)
    check(rc == 0, "双通道弯音导出 HTML", err[:200])
    check("弯音范围：±12 半音" in open(bch, encoding="utf-8").read(),
          "HTML 页眉写明弯音范围")

    print("\n[39] RPN 中途改范围：不倒灌到之前的音")
    bt = os.path.join(FIXTURES, "bendrange_timeline.mid")
    rc, out, err = run(exe, bt, "--no-pickup")
    check(rc == 0, "中途改范围文件解析成功", err[:200])
    check("±2 / ±12" in out and "2 处设定" in out, "CLI 列出两种范围与 2 处设定")
    btx = os.path.join(tmp, "bendrange_timeline.xml")
    rc, _, err = run(exe, bt, "--no-pickup", "--musicxml", btx)
    check(rc == 0, "中途改范围导出 MusicXML", err[:200])
    pt = xml_pitches(btx)
    check(pt == [("C", "5"), ("D", "4")],
          f"前一段 ±12（C5）、后一段 ±2（D4），实际 {pt}")

    print("\n[40] RPN 写在另一条轨：解析顺序不能决定时间顺序")
    bx = os.path.join(FIXTURES, "bendrange_crosstrack.mid")
    rc, out, err = run(exe, bx, "--no-pickup")
    check(rc == 0, "跨轨 RPN 文件解析成功", err[:200])
    check("弯音范围：±12 半音" in out, "跨轨 RPN 被识别")
    bxx = os.path.join(tmp, "bendrange_crosstrack.xml")
    rc, _, err = run(exe, bx, "--no-pickup", "--musicxml", bxx)
    check(rc == 0, "跨轨 RPN 导出 MusicXML", err[:200])
    px = xml_pitches(bxx)
    check(px == [("C", "5")], f"setup 轨的 ±12 仍然生效，实际 {px}")

    print("\n[41] 歌词 meta 之后省略状态字节：半首曲子不能凭空消失")
    rm = os.path.join(FIXTURES, "running_meta.mid")
    rc, out, err = run(exe, rm, "--no-pickup")
    check(rc == 0, "省略状态字节的 MIDI 解析成功", err[:200])
    check("补回 1 处" in out, "CLI 页眉写明补回处数")
    rmx = os.path.join(tmp, "running_meta.xml")
    rc, _, err = run(exe, rm, "--no-pickup", "--musicxml", rmx)
    check(rc == 0, "省略状态字节导出 MusicXML", err[:200])
    pm = xml_pitches(rmx)
    check(pm == [("C", "4"), ("D", "4")],
          f"歌词之后的 D4 还在，实际 {pm}")
    rmh = os.path.join(tmp, "running_meta.html")
    rc, _, err = run(exe, rm, "--html", rmh)
    check(rc == 0, "省略状态字节导出 HTML", err[:200])
    check("省略状态字节" in open(rmh, encoding="utf-8").read(),
          "HTML 页眉同步说明")

    print("\n[42] SysEx 取消 / 0xF8 实时消息不取消：两种都要读出两个音")
    for name, label, need_hint in (
        ("running_sysex.mid", "SysEx 后省略状态字节", True),
        ("running_realtime.mid", "0xF8 时钟后省略状态字节", False),
    ):
        p = os.path.join(FIXTURES, name)
        rc, out, err = run(exe, p, "--no-pickup")
        check(rc == 0, f"{label}：解析成功", err[:200])
        x = os.path.join(tmp, name + ".xml")
        rc, _, err = run(exe, p, "--no-pickup", "--musicxml", x)
        check(rc == 0, f"{label}：导出 MusicXML", err[:200])
        check(xml_pitches(x) == [("C", "4"), ("D", "4")],
              f"{label}：C4/D4 都在")
        if need_hint:
            check("补回 1 处" in out, f"{label}：补回提示存在")
        else:
            check("省略状态字节" not in out,
                  f"{label}：实时消息不改变 running status，无需补回")

    print("\n[43] 规范写法零回归：重写状态字节时不触发任何补回")
    rs = os.path.join(FIXTURES, "running_standard.mid")
    rc, out, err = run(exe, rs, "--no-pickup")
    check(rc == 0, "规范写法解析成功", err[:200])
    check("省略状态字节" not in out, "规范写法不输出补回提示")
    rsx = os.path.join(tmp, "running_standard.xml")
    rc, _, err = run(exe, rs, "--no-pickup", "--musicxml", rsx)
    check(rc == 0, "规范写法导出 MusicXML", err[:200])
    check(xml_pitches(rsx) == [("C", "4"), ("D", "4")],
          "规范写法仍是 C4/D4 两个八分音符")

    # ---- MIDI Port / Channel Prefix ----
    def xml_dur_by_pitch(path):
        root = ETC.fromstring(open(path, encoding="utf-8").read())
        out = {}
        for n in root.iter("note"):
            if n.find("rest") is not None or n.find("chord") is not None:
                continue
            p = n.find("pitch")
            if p is None:
                continue
            key = (p.findtext("step"), p.findtext("octave"))
            out[key] = out.get(key, 0) + int(n.findtext("duration") or 0)
        return out

    print("\n[44] MIDI Port：踏板按 (port, channel) 隔离，端口不跨轨继承")
    pp = os.path.join(FIXTURES, "port_pedal.mid")
    rc, out, err = run(exe, pp, "--no-pickup")
    check(rc == 0, "端口踏板素材解析成功", err[:200])
    check("MIDI Port：2 处声明，共 2 个端口（0～1）" in out,
          "CLI 页眉报出端口数与隔离说明")
    check("延音踏板：补长 2 个音" in out,
          "只有 port 0 的两个音被补长（旧版是 3 个）")
    ppx = os.path.join(tmp, "port_pedal.xml")
    rc, _, err = run(exe, pp, "--no-pickup", "--musicxml", ppx)
    check(rc == 0, "端口踏板导出 MusicXML", err[:200])
    pd = xml_dur_by_pitch(ppx)
    check(pd.get(("C", "4")) == 1920 and pd.get(("D", "4")) == 480
          and pd.get(("E", "4")) == 960,
          "C4=1920 / D4=480 / E4=960（D4 没被 port 0 的踏板串到）", str(pd))
    pph = os.path.join(tmp, "port_pedal.html")
    rc, _, err = run(exe, pp, "--no-pickup", "--html", pph)
    check(rc == 0, "端口踏板导出 HTML", err[:200])
    check("同通道号跨端口已隔离" in open(pph, encoding="utf-8").read(),
          "HTML 页眉同样写明端口隔离")

    print("\n[45] MIDI Port：弯音范围按端口独立；FF 20 不重定向通道消息")
    pb = os.path.join(FIXTURES, "port_bend.mid")
    rc, bout, err = run(exe, pb, "--no-pickup")
    check(rc == 0, "端口弯音素材解析成功", err[:200])
    check("弯音范围：±12 半音" in bout, "RPN ±12 只被记了一次")
    pbx = os.path.join(tmp, "port_bend.xml")
    rc, _, err = run(exe, pb, "--no-pickup", "--musicxml", pbx)
    check(rc == 0, "端口弯音导出 MusicXML", err[:200])
    check(xml_pitches(pbx) == [("C", "5"), ("E", "4")],
          "C4→C5（±12）、D4→E4（默认 ±2），不是两个都 ±12")
    pre = os.path.join(FIXTURES, "port_prefix.mid")
    base = os.path.join(FIXTURES, "port_prefix_base.mid")
    _, po, _ = run(exe, pre, "--no-pickup")
    _, bo, _ = run(exe, base, "--no-pickup")
    body = lambda s: s[s.find("1="):]
    check(body(po) == body(bo) and "1=" in po,
          "FF 20 前后谱面正文逐字符一致（没有把 ch0 重定向成 ch2）")
    check("通道前缀：1 处（按规范不影响通道消息）" in po,
          "CLI 页眉如实说明 FF 20 的作用域")
    check("通道前缀" not in bo, "不写 FF 20 的基线没有多余提示")

    print("\n[46] 无端口声明零回归：单通道踏板行为与旧版一致")
    ps = os.path.join(FIXTURES, "port_standard.mid")
    rc, sout, err = run(exe, ps, "--no-pickup")
    check(rc == 0, "无端口基线解析成功", err[:200])
    check("MIDI Port" not in sout and "通道前缀" not in sout,
          "没有 FF 21 / FF 20 时不输出任何端口提示")
    check("延音踏板：补长 2 个音" in sout, "单通道踏板照常补长两个音")
    psx = os.path.join(tmp, "port_standard.xml")
    rc, _, err = run(exe, ps, "--no-pickup", "--musicxml", psx)
    check(rc == 0, "无端口基线导出 MusicXML", err[:200])
    check(xml_dur_by_pitch(psx) == {("C", "4"): 1920, ("D", "4"): 1440},
          "无端口声明的时值与旧版完全相同")


    print("\n[47] SysEx 鼓模式：GS/XG 指定的鼓通道不能转成假旋律")
    for name in ("sysex_gs_drum", "sysex_xg_drum"):
        f = os.path.join(FIXTURES, name + ".mid")
        rc, out, err = run(exe, f, "--no-pickup")
        check(rc == 0, f"{name} 解析成功", err[:200])
        check("共提取 8 个音符" in out and "已跳过 8 个打击乐音符" in out,
              f"{name}：只留 8 个旋律音，8 个鼓件音被跳过")
        check("SysEx 鼓模式：1 处 GS/XG 设定，1 条鼓通道，已跳过 8 个鼓件音" in out,
              f"{name}：CLI 页眉说清鼓模式与跳过数量")
        check("#4,," not in out and "1,," not in out,
              f"{name}：谱面正文里没有鼓件音高转出的低音垃圾")
        x = os.path.join(tmp, name + ".xml")
        rc, _, err = run(exe, f, "--no-pickup", "--musicxml", x)
        check(rc == 0 and len(xml_pitches(x)) == 8,
              f"{name}：MusicXML 也只有 8 个旋律音", err[:200])
    _, gso, _ = run(exe, os.path.join(FIXTURES, "sysex_gs_drum.mid"), "--no-pickup")
    _, xgo, _ = run(exe, os.path.join(FIXTURES, "sysex_xg_drum.mid"), "--no-pickup")
    check(body(gso) == body(xgo) and "1=" in gso,
          "GS 与 XG 两种 SysEx 写法得到完全相同的谱面正文")
    gn = os.path.join(FIXTURES, "sysex_gs_normal.mid")
    _, no, _ = run(exe, gn, "--no-pickup")
    check("共提取 16 个音符" in no and "已跳过" not in no,
          "mode=0 对照：普通乐器一个音都不丢")
    check("1 条恢复普通乐器" in no, "mode=0：CLI 说明通道恢复普通乐器")

    print("\n[48] GM/GS/XG Reset 按时间撤销鼓设定；SysEx 写在末轨也生效")
    for name in ("sysex_gs_reset", "sysex_gm_reset", "sysex_xg_reset"):
        f = os.path.join(FIXTURES, name + ".mid")
        rc, out, err = run(exe, f, "--no-pickup")
        check(rc == 0 and "共提取 12 个音符" in out and "已跳过 4 个打击乐音符" in out,
              f"{name}：前 4 个鼓件音丢、后 4 个恢复成普通乐器", err[:200])
        check("1 处 GM/GS/XG Reset 恢复默认" in out,
              f"{name}：CLI 报出 Reset 恢复默认")
        x = os.path.join(tmp, name + ".xml")
        rc, _, err = run(exe, f, "--no-pickup", "--musicxml", x)
        check(rc == 0 and len(xml_pitches(x)) == 12,
              f"{name}：MusicXML 同样是 12 个音", err[:200])
    cd = os.path.join(FIXTURES, "sysex_conductor.mid")
    _, co, _ = run(exe, cd, "--no-pickup")
    check("共提取 8 个音符" in co and "已跳过 8 个打击乐音符" in co,
          "SysEx 在最后一条轨（tick 0）仍对前面的轨生效")
    cx = os.path.join(tmp, "sysex_conductor.xml")
    run(exe, cd, "--no-pickup", "--musicxml", cx)
    check(xml_pitches(cx) == xml_pitches(os.path.join(tmp, "sysex_gs_drum.xml")),
          "末轨 SysEx 与首轨 SysEx 的 MusicXML 音高序列一致")

    print("\n[49] 通道 10 改回普通乐器；无关 SysEx 零回归")
    ch9 = os.path.join(FIXTURES, "sysex_ch9_normal.mid")
    rc, c9, err = run(exe, ch9, "--no-pickup")
    check(rc == 0 and "共提取 16 个音符" in c9 and "已跳过" not in c9,
          "GS 把通道 10 改回钢琴：ch9 的 8 个旋律音全部留下", err[:200])
    check("1 条恢复普通乐器" in c9, "CLI 说明通道 10 恢复普通乐器")
    c9x = os.path.join(tmp, "sysex_ch9_normal.xml")
    run(exe, ch9, "--no-pickup", "--musicxml", c9x)
    check(len(xml_pitches(c9x)) == 16, "MusicXML 同样有 16 个音")
    cd9 = os.path.join(FIXTURES, "sysex_ch9_drum.mid")
    _, c9d, _ = run(exe, cd9, "--no-pickup")
    check("共提取 8 个音符" in c9d and "已跳过 8 个打击乐音符" in c9d,
          "显式把通道 10 设回鼓：仍按鼓跳过")
    uk = os.path.join(FIXTURES, "sysex_unknown.mid")
    rc, uo, err = run(exe, uk, "--no-pickup")
    check(rc == 0 and "共提取 16 个音符" in uo and "SysEx 鼓模式" not in uo,
          "不相干的 SysEx 不触发任何鼓模式提示，也不丢音", err[:200])
    ux = os.path.join(tmp, "sysex_unknown.xml")
    run(exe, uk, "--no-pickup", "--musicxml", ux)
    gnh = os.path.join(tmp, "sysex_gs_normal.xml")
    run(exe, gn, "--no-pickup", "--musicxml", gnh)
    check(xml_pitches(ux) == xml_pitches(gnh) and len(xml_pitches(ux)) == 17,
          "无关 SysEx 的 MusicXML 音高序列与 mode=0 基线完全一致")

    print("\n[50] SysEx 鼓模式按端口隔离；HTML 页眉同样写明")
    sp = os.path.join(FIXTURES, "sysex_port_drum.mid")
    rc, so, err = run(exe, sp, "--no-pickup")
    check(rc == 0 and "共提取 4 个音符" in so and "已跳过 4 个打击乐音符" in so,
          "port 1 的 ch0 鼓件音被丢，port 0 的 ch0 旋律保留", err[:200])
    check("MIDI Port：1 处声明，共 2 个端口（0～1）" in so and
          "已跳过 4 个鼓件音" in so, "端口与鼓模式两条提示同时出现")
    sph = os.path.join(tmp, "sysex_port_drum.html")
    rc, _, err = run(exe, sp, "--no-pickup", "--html", sph)
    check(rc == 0 and "SysEx 鼓模式" in open(sph, encoding="utf-8").read(),
          "HTML 页眉同样写明 SysEx 鼓模式", err[:200])


    print("\n[51] Bank Select：XG 鼓组通道 / 普通音色库对照")
    bx = os.path.join(FIXTURES, "bank_xg_drum.mid")
    rc, bo, err = run(exe, bx, "--no-pickup")
    check(rc == 0 and "共提取 8 个音符" in bo and "已跳过 8 个打击乐音符" in bo,
          "XG Bank MSB 127 的 ch0 鼓件音被丢，8 个旋律音全留", err[:200])
    check("音色库/音色设定" in bo and "1 条 XG 鼓组通道" in bo and
          "SysEx 鼓模式" not in bo, "CLI 只报 Bank 提示，不误报 SysEx")
    bn = os.path.join(FIXTURES, "bank_normal.mid")
    rc, no, err = run(exe, bn, "--no-pickup")
    check(rc == 0 and "共提取 16 个音符" in no and "已跳过" not in no,
          "CC0=0 的普通音色库：16 个音一个不少", err[:200])
    bxm = os.path.join(tmp, "bank_xg_drum.xml")
    run(exe, bx, "--no-pickup", "--musicxml", bxm)
    check(len(xml_pitches(bxm)) == 8 and "<octave>2</octave>" not in open(bxm, encoding="utf-8").read(),
          "XG 鼓组的 MusicXML 只有 8 个旋律音，没有低八度鼓件")
    bh = os.path.join(tmp, "bank_xg_drum.html")
    run(exe, bx, "--no-pickup", "--html", bh)
    check("音色库/音色设定" in open(bh, encoding="utf-8").read(),
          "HTML 页眉同样写明 Bank Select")

    print("\n[52] Bank 时间轴：切回普通库 / 末尾 setup 轨 / GM Reset")
    bs = os.path.join(FIXTURES, "bank_switch_back.mid")
    rc, so, err = run(exe, bs, "--no-pickup")
    check(rc == 0 and "共提取 12 个音符" in so and "已跳过 4 个打击乐音符" in so,
          "CC0 从 127 改回 0 后，后半段 4 个旋律音恢复", err[:200])
    check("1 条通道中途换音色（乐器名按时间取）" in so,
          "CLI 说明同通道中途换音色已按时间轴取名")
    bc = os.path.join(FIXTURES, "bank_conductor.mid")
    _, co, _ = run(exe, bc, "--no-pickup")
    check("共提取 8 个音符" in co and "已跳过 8 个打击乐音符" in co,
          "Bank 设定写在最后一条 setup 轨（tick 0）仍生效")
    br = os.path.join(FIXTURES, "bank_reset.mid")
    _, ro, _ = run(exe, br, "--no-pickup")
    check("共提取 14 个音符" in ro and "已跳过 2 个打击乐音符" in ro,
          "GM Reset 只撤销它之前的 2 个鼓件音，之后 6 个旋律音全留")

    print("\n[53] Bank 鼓组按端口隔离；两个防御性对照")
    bp = os.path.join(FIXTURES, "bank_port.mid")
    rc, po, err = run(exe, bp, "--no-pickup")
    check(rc == 0 and "共提取 8 个音符" in po and "已跳过 8 个打击乐音符" in po,
          "port 1 的 ch0 鼓件音被丢，port 0 的 ch0 旋律保留", err[:200])
    b9 = os.path.join(FIXTURES, "bank_ch9_plain.mid")
    _, o9, _ = run(exe, b9, "--no-pickup")
    check("共提取 8 个音符" in o9 and "已跳过 4 个打击乐音符" in o9,
          "通道 10 的普通 Bank+Program 不能把它从鼓改回旋律")
    bp0 = os.path.join(FIXTURES, "bank_no_program.mid")
    _, o0, _ = run(exe, bp0, "--no-pickup")
    check("共提取 12 个音符" in o0 and "已跳过" not in o0,
          "只有 CC0=127、没有 Program Change 时不切鼓")

    print("\n[54] Program Change 时间轴：声部乐器名按首音时刻取")
    pt = os.path.join(FIXTURES, "program_timeline.mid")
    rc, to, err = run(exe, pt, "--no-pickup")
    check(rc == 0 and "共提取 16 个音符" in to, "中途换音色不改变音符数", err[:200])
    check("声部：Lead·钢琴" in to and "声部：Lead·小提琴" not in to,
          "前半首钢琴、后半首小提琴：声部标前半首真正的乐器")
    px = os.path.join(tmp, "program_timeline.xml")
    run(exe, pt, "--no-pickup", "--musicxml", px)
    pxtext = open(px, encoding="utf-8").read()
    check("<instrument-name>钢琴</instrument-name>" in pxtext and
          "<instrument-name>小提琴</instrument-name>" not in pxtext,
          "MusicXML 的 score-part 同样标钢琴")
    check("音色库/音色设定" not in to,
          "只有 0xC0、没有 Bank Select 的普通 GM 文件不显示新提示")

    print("\n[55] --track 按轨隔离：别轨歌词 / 力度不许串进选中轨")
    tl = os.path.join(FIXTURES, "track_leak.mid")
    tb = os.path.join(FIXTURES, "track_leak_base.mid")
    rc, lall, err = run(exe, tl, "--no-pickup")
    check(rc == 0 and "MIDI 内置歌词: 16 个音节" in lall and "力度 1" in lall,
          "不带 --track 时两轨的歌词 / 力度都在", err[:200])
    rc, l0, err = run(exe, tl, "--no-pickup", "--track", "0")
    _, base, _ = run(exe, tb, "--no-pickup")
    stripped0 = "".join(x for x in l0.splitlines(True)
                        if not x.startswith("按轨隔离："))
    check(rc == 0 and stripped0 == base,
          "--track 0 去掉新提示行后与单轨基线逐字节一致", err[:200])
    check("MIDI 内置歌词" not in l0 and "力度 1" not in l0,
          "--track 0 不再冒出别轨的内置歌词与力度记号")
    check("按轨隔离：只转换轨 0「Vocal」" in l0 and "另丢弃 8 个音符" in l0,
          "--track 0 的页眉写明隔离范围与丢弃的音符数")
    _, l1, _ = run(exe, tl, "--no-pickup", "--track", "1")
    check("MIDI 内置歌词: 16 个音节" in l1 and "力度 1" in l1 and
          "只转换轨 1「Guitar」" in l1,
          "--track 1 反过来保留该轨自己的歌词 / 力度")

    print("\n[56] 按轨隔离：踏板隔开，全局音源设定照常保留")
    tp = os.path.join(FIXTURES, "track_pedal.mid")
    _, pall, _ = run(exe, tp, "--no-pickup")
    _, p0, _ = run(exe, tp, "--no-pickup", "--track", "0")
    check("延音踏板：补长 8 个音" in pall,
          "全轨时别轨的 CC64 踏板确实补长了 8 个音")
    check("延音踏板" not in p0 and "按轨隔离" in p0,
          "--track 0 完全不受别轨踏板影响（踏板与旋律同通道）")
    tr = os.path.join(FIXTURES, "track_rpn_setup.mid")
    _, r1, _ = run(exe, tr, "--no-pickup", "--track", "1")
    check("弯音范围：±12 半音" in r1,
          "--track 1 仍读到别轨 setup 的 RPN ±12 半音")
    tc = os.path.join(FIXTURES, "track_conductor.mid")
    _, c1, _ = run(exe, tc, "--no-pickup", "--track", "1")
    check("拍号=3/4" in c1 and "1=D 大调" in c1 and "♩=90" in c1 and
          "[Verse]" in c1,
          "--track 1 仍读到别轨 conductor 的速度 / 拍号 / 调号 / Marker")

    print("\n[57] format 2 选段、空轨报错与默认零回归")
    tf = os.path.join(FIXTURES, "track_fmt2.mid")
    _, f1, _ = run(exe, tf, "--track", "1")
    check("共 2 段，只转换轨 1「SongB」" in f1 and "共提取 4 个音符" in f1 and
          "按轨隔离" in f1,
          "format 2 选第 2 段时段数提示不消失、只留该段 4 个音")
    te = os.path.join(FIXTURES, "track_empty.mid")
    rc, _, ee = run(exe, te, "--track", "0")
    check(rc == 4 and "轨道 0 无音符" in ee,
          "--track 指向只有 meta 的空轨时明确报错并返回 4")
    rc, _, e9 = run(exe, tf, "--track", "9")
    check(rc == 4 and "轨道 9 无音符" in e9,
          "--track 越界时明确报错并返回 4")
    _, sm, _ = run(exe, os.path.join(FIXTURES, "sample.mid"))
    check("按轨隔离" not in sm,
          "不带 --track 的普通文件不显示按轨隔离提示（页眉零回归）")

    # ---- [58] --list-tracks：选 --track 前先看清轨道布局 ----
    # 纯诊断命令，不参与转换。验证它列出每条轨的序号/名称/音数/乐器/歌词，
    # 并能把鼓轨标出来；同时必须「只列不转」——不输出「共提取 N 个音符」。
    print("\n[58] --list-tracks 轨道列表（诊断）")
    # meter.mid 含 Conductor / Vocal / Piano / Drums 四条轨，正好覆盖各类。
    rc, out, err = run(exe, os.path.join(FIXTURES, "meter.mid"), "--list-tracks")
    check(rc == 0, "退出码为 0", err)
    check("轨道列表" in out and "format 1" in out, "打印轨道列表头")
    check("--track N 只取第 N 轨" in out, "提示默认转全部、可用 --track 选段")
    check(out.count("#") >= 4, "列出至少 4 条轨", str(out.count("#")))
    check('"Vocal"' in out and '"Piano"' in out, "写出轨名 Vocal / Piano")
    check("鼓轨" in out and "鼓件音" in out, "把鼓轨标出来")
    check("合成人声" in out or "钢琴" in out, "写出轨道的 GM 乐器名")
    check("共提取" not in out, "只列轨道、不参与转换（无「共提取」行）")

    # 空文件保护：--list-tracks 也走正常加载路径，非法文件仍报错
    bad = os.path.join(tmp, "not_a_midi.txt")
    open(bad, "w", encoding="utf-8").write("hello")
    rc2, _, err2 = run(exe, bad, "--list-tracks")
    check(rc2 != 0 and ("错误" in err2 or "无法" in err2),
          "对非法文件给出错误而非崩溃", err2)

    print("\n[59] CC64 踏板迟滞：死区中间值不提前抬起")
    # 连续踏板 127→50→0，50 落在迟滞死区 33..63：踩着不放，音延到真正的
    # 释放点 0（第 2 拍）；旧二值判定会在 50 处当成抬起，只延到 1 拍。
    ph = os.path.join(FIXTURES, "pedal_hysteresis.mid")
    rc, out, err = run(exe, ph)
    check(rc == 0, "迟滞踏板文件解析成功", err[:200])
    check("延音踏板：补长 1 个音（3 个踏板事件）" in out,
          "CLI 报出补长 1 个音 / 3 个踏板事件")
    body = out.split("---")[-1]
    check("1 -" in body,
          f"死区 50 不中断，音延到真正释放点（2 拍），实际: {body.strip()}")
    check("1  |" not in body, "不再是旧行为的 1 拍（在 50 处提前断开）")

    phx = os.path.join(tmp, "pedal_hysteresis.xml")
    rc, _, err = run(exe, ph, "--musicxml", phx)
    check(rc == 0, "迟滞踏板导出 MusicXML", err[:200])
    root_ph = ETC.fromstring(open(phx, encoding="utf-8").read())
    phnotes = [n for n in root_ph.iter("note") if n.find("rest") is None]
    phdurs = [int(n.findtext("duration")) for n in phnotes]
    phtypes = [n.findtext("type") for n in phnotes]
    check(phdurs == [960], f"音符时值 960 tick（2 拍），实际 {phdurs}")
    check(phtypes == ["half"], f"MusicXML 记为 half，实际 {phtypes}")

    print("\n[60] --list-tracks 诊断与谱面标注一致性")
    # 旧实现两处各算一套：乐器名读 channelProgram 末值（谱面读 programAt 首音），
    # 鼓件音只按默认通道 9 统计（GS/XG 切出来的鼓组漏标）。同一首曲子里，
    # 诊断说「小提琴 + 旋律 0 音」，谱面却是「钢琴 + 鼓轨」，选轨会被误导。
    dc = os.path.join(FIXTURES, "diag_consistency.mid")
    rc, lst, err = run(exe, dc, "--list-tracks")
    check(rc == 0, "诊断一致性素材解析成功", err[:200])
    _, score, _ = run(exe, dc)
    # A) 中途换音色：Lead 前半钢琴(0) / 后半小提琴(40)，诊断必须报首音乐器
    check("乐器: 钢琴" in lst and "乐器: 小提琴" not in lst,
          "诊断按首音报「钢琴」，不再读末值报「小提琴」")
    check("Lead·钢琴" in score,
          "谱面声部仍是 Lead·钢琴，诊断与谱面同源")
    # B) XG Part Mode 切出的鼓组：8 个鼓件音按轨补记，标成鼓轨
    drums_line = next((ln for ln in lst.splitlines() if '"Drums"' in ln), "")
    check("鼓轨 (跳过 8 鼓件音)" in drums_line,
          "XG 鼓组轨标成「鼓轨 (跳过 8 鼓件音)」", drums_line)
    check("旋律 0 音" not in drums_line,
          "不再把整条鼓轨显示成「旋律 0 音」", drums_line)
    # C) 纯诊断：不参与转换，也不碰既有谱面
    check("共提取" not in lst, "只列轨道、不参与转换（无「共提取」行）")
    check("Melody" in lst and "Lead" in lst, "旋律轨仍照常列出")

    # 既有素材零回归：meter.mid 的鼓轨与乐器名都不受改动影响
    rc2, lst2, _ = run(exe, os.path.join(FIXTURES, "meter.mid"), "--list-tracks")
    check(rc2 == 0 and "鼓轨 (跳过 6 鼓件音)" in lst2 and
          "乐器: 合成人声" in lst2 and "乐器: 钢琴" in lst2,
          "meter.mid 的鼓轨 / 乐器名逐字不变（零回归）")

    # Bank MSB 127 切出的鼓组同样要标鼓轨（与 SysEx 共用鼓模式时间轴）
    rc3, lst3, _ = run(exe, os.path.join(FIXTURES, "bank_xg_drum.mid"),
                       "--list-tracks")
    check(rc3 == 0 and "鼓轨 (跳过 8 鼓件音)" in lst3,
          "Bank MSB 127 的 XG 鼓组轨也标成鼓轨", lst3)

    print("\n[61] 截断 MIDI：不越过文件末尾读幻影音符与乱码歌词")
    # MTrk 长度字段写着原值、文件却只下载到一半，轨尾只剩半个 note-on。
    # 旧实现从 buffer 末尾继续读堆内存：2 音素材冒出 10 个幻影音符，调性
    # 也被带偏；被截断的 lyric meta 还会冒出 9 个乱码歌词音节。
    tm = os.path.join(FIXTURES, "truncated.mid")
    rc, out, err = run(exe, tm)
    check(rc == 0, "截断文件仍能按可用字节出谱", err[:200])
    check("共提取 2 个音符" in out, "只读出 2 个真音，不再冒出幻影音符", out[:300])
    check("文件数据不完整" in out and "丢弃" in out,
          "CLI 明确提示数据被截断、丢弃了不完整事件")
    check("MIDI 内置歌词" not in out, "截断的 meta 不产生乱码歌词")

    tmx = os.path.join(tmp, "truncated.xml")
    rc, _, err = run(exe, tm, "--musicxml", tmx)
    check(rc == 0, "截断文件导出 MusicXML", err[:200])
    root_t = ETC.fromstring(open(tmx, encoding="utf-8").read())
    pitched_t = [n for n in root_t.iter("note") if n.find("rest") is None]
    check(len(pitched_t) == 2, f"MusicXML 也只有 2 个实音，实际 {len(pitched_t)}")

    tmh = os.path.join(tmp, "truncated.html")
    rc, _, err = run(exe, tm, "--html", tmh)
    check(rc == 0, "截断文件导出 HTML", err[:200])
    html_t = open(tmh, encoding="utf-8").read()
    check("文件数据不完整" in html_t, "HTML 页眉也写明数据不完整")

    _, normal_out, _ = run(exe, os.path.join(FIXTURES, "sample.mid"))
    check("文件数据不完整" not in normal_out, "正常文件不出现截断提示（零回归）")

    print("\n[62] CC66 持音踏板：只保持踩下瞬间正按着的键")
    # 真钢琴中踏板：踩下时正按着的 C/E/G 延到抬起，之后新弹的 D4 不受影响。
    # 旧实现把 0xB0 里的 CC66 当普通两字节跳过，和弦只写 1 拍。
    so = os.path.join(FIXTURES, "sostenuto.mid")
    rc, out, err = run(exe, so)
    check(rc == 0, "持音踏板文件解析成功", err[:200])
    check("持音踏板：保持 3 个音（2 个事件）" in out,
          "CLI 报出保持 3 个音 / 2 个 CC66 事件")
    check("延音踏板" not in out, "纯 CC66 文件不误报 CC64 延音踏板")

    sox = os.path.join(tmp, "sostenuto.xml")
    rc, _, err = run(exe, so, "--musicxml", sox)
    check(rc == 0, "持音踏板导出 MusicXML", err[:200])
    root_so = ETC.fromstring(open(sox, encoding="utf-8").read())
    pitched_so = [n for n in root_so.iter("note") if n.find("rest") is None]
    so_durs = sorted(int(n.findtext("duration")) for n in pitched_so)
    so_types = [n.findtext("type") for n in pitched_so]
    check(so_durs == [480, 1920, 1920, 1920],
          f"3 个和弦音 1920 tick、新弹的音 480 tick，实际 {so_durs}")
    check(so_types.count("whole") == 3 and so_types.count("quarter") == 1,
          f"MusicXML 记为 3 个 whole + 1 个 quarter，实际 {so_types}")

    soh = os.path.join(tmp, "sostenuto.html")
    rc, _, err = run(exe, so, "--html", soh)
    check(rc == 0, "持音踏板导出 HTML", err[:200])
    html_so = open(soh, encoding="utf-8").read()
    check("持音踏板" in html_so, "HTML 页眉写明持音踏板")

    _, ph_out, _ = run(exe, os.path.join(FIXTURES, "pedal_hysteresis.mid"))
    check("持音踏板" not in ph_out, "CC64 素材不误报持音踏板（零回归）")

    print("\n" + "-" * 48)
    if failures:
        print(f"失败 {len(failures)} 项")
        return 1
    print("冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

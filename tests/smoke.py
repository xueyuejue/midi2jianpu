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

    print("\n" + "-" * 48)
    if failures:
        print(f"失败 {len(failures)} 项")
        return 1
    print("冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

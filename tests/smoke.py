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

    print("\n" + "-" * 48)
    if failures:
        print(f"失败 {len(failures)} 项")
        return 1
    print("冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

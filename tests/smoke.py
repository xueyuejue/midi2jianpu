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

    print("\n" + "-" * 48)
    if failures:
        print(f"失败 {len(failures)} 项")
        return 1
    print("冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

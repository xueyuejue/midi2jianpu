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
    check("1 = C 大调" in out, "识别出 C 大调", out[:200])
    check("[Vocal]" in out and "[Piano]" in out, "分离出两个轨道")
    check(re.search(r"5\s+-", out) is not None, "二分音符标出延长线（5 -）")
    check("3(5 5 5" in out, "三连音标记 3(5 5 5)")
    check("1' -" in out, "同音重复合并成延音（1' -）")
    check("[p]" in out and "[ff]" in out, "力度记号 p / ff")

    print("\n[2] 手动调性 --key G（八度基准回归）")
    rc, out, _ = run(exe, mid, "--key", "G")
    check(rc == 0 and "1 = G 大调" in out, "输出 G 大调")
    vocal = [l for l in out.splitlines() if l.startswith("[C]") or l.startswith("[F]")]
    joined = " ".join(vocal)
    check("4'" not in joined, "G 大调里 C5 不再错误地带高八度点", joined)

    print("\n[3] 降号调 --key Bb")
    rc, out, _ = run(exe, mid, "--key", "Bb")
    check(rc == 0 and "1 = Bb 大调" in out, "输出 Bb 大调")
    check("b" in out, "出现降号写法")

    print("\n[4] 非法调性应给出警告而不是静默失败")
    rc, out, err = run(exe, mid, "--key", "H")
    check(rc == 0 and "无法识别" in err, "对非法调性给出警告", err)

    print("\n[5] --track 轨道过滤")
    rc, out, _ = run(exe, mid, "--track", "1")
    check(rc == 0 and "[Vocal]" in out and "[Piano]" not in out, "只输出轨道 1")

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

    print("\n[8] --html + --lyrics 导出")
    html = os.path.join(tmp, "out.html")
    rc, out, _ = run(exe, mid, "--html", html, "--lyrics", lrc)
    check(rc == 0 and os.path.exists(html), "生成 HTML 文件")
    body = open(html, encoding="utf-8").read()
    check("<!DOCTYPE html>" in body, "带 DOCTYPE")
    check("class='note" in body, "含音符样式元素")
    check("歌词已加载" in out, "歌词加载成功")

    print("\n[9] --output 文本落盘")
    txt = os.path.join(tmp, "out.txt")
    rc, out, _ = run(exe, mid, "--output", txt)
    check(rc == 0 and os.path.exists(txt), "生成文本文件")
    check("1 = C 大调" in open(txt, encoding="utf-8").read(), "文件内有调性行")

    print("\n[10] --help / --version")
    rc, out, _ = run(exe, "--help")
    check(rc == 0 and "--musicxml" in out, "帮助里列出了 --musicxml")
    rc, out, _ = run(exe, "--version")
    check(rc == 0 and re.search(r"\d+\.\d+\.\d+", out) is not None, "版本号正常")

    print("\n" + "-" * 48)
    if failures:
        print(f"失败 {len(failures)} 项")
        return 1
    print("冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

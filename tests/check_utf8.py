#!/usr/bin/env python3
"""三端输出必须是合法 UTF-8：文本谱 / HTML / MusicXML 逐个严格解码。

为什么单独立一道闸门：
    MIDI 里的 0x06 段落名、轨名、歌词都是**裸字节**，工具原样转发到三端。
    只要有一处按字节截断或按字节清洗，就可能留下孤立的续字节（0x80..0xBF），
    整份文件从此不是合法 UTF-8。文本谱只是终端显示成乱码，还能忍；HTML 是高亮
    替换字符；MusicXML 则**整份被解析器拒绝**——MuseScore 打开时直接报错，
    用户拿到的不是「少一个段落名」，而是「这份谱打不开」。
    v1.79.0 修的就是 `substr(0, 40)` 把汉字劈成半截，缺陷正是被这里抓到的。

v1.122.0 起这道闸门同时管「控制字符」：轨名 / 段落名 / 歌词都是裸字节，
XML 1.0 禁止 C0（留着整份谱面解析不了）、HTML 遇到它们是解析错误、终端则会
把 ESC 当 ANSI 命令执行（改颜色、清屏、改窗口标题）。所以三端产物与
`--list-tracks` 的终端输出里都不许出现裸 C0 / DEL——这是注入面，不是洁癖。

用法：
    python tests/check_utf8.py [--exe PATH]
        对全部 fixture 生成三端输出，逐个严格 UTF-8 解码 + XML 严格解析，
        并审计控制字符（含 `--list-tracks` 的 stdout）。
    python tests/check_utf8.py --self-test
        纯逻辑自测，不调用可执行文件。

不依赖第三方库；失败返回非 0，可直接用于 CI。
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# 段落名的显示上限，与 src/notation.cpp 的 truncateUtf8Chars(text, 40) 对齐。
# 注意是 40 个**字符**，不是 40 字节：一个汉字 3 字节，按字节切既短得离谱，
# 又必然把最后一个字劈开。
MAX_SECTION_CHARS = 40

SECTION = re.compile(r"<span class='section'>([^<]*)</span>")

# 三端产物与诊断输出里都不许出现的控制字符：C0（除 \t \n \r）与 DEL。
# 这不是编码问题而是**载体**问题：XML 1.0 明文禁止 C0（留着整份谱面解析不了）、
# HTML 遇到它们是解析错误、终端则会把 ESC 当成 ANSI 命令执行（改颜色、清屏、
# 改窗口标题）。轨名 / 段落名 / 歌词都是裸字节，这条闸门盯的是「有没有哪个
# 输出层又把它们原样放出去了」——v1.122.0 之前只有 MusicXML 是对的。
BAD_CTRL = set(range(0x00, 0x20)) - {0x09, 0x0A, 0x0D}
BAD_CTRL.add(0x7F)


def default_exe():
    p = os.path.join(ROOT, "build", "midi2jianpu.exe")
    return p if os.path.exists(p) else os.path.join(ROOT, "build", "midi2jianpu")


def invalid_utf8(blob):
    """返回第一处非法 UTF-8 的说明；合法则返回 None。

    用严格解码而不是 errors='replace'：替换成 U+FFFD 会把问题藏起来，
    而这里的判据必须是「一个字节都不许错」。
    """
    try:
        blob.decode("utf-8")
        return None
    except UnicodeDecodeError as e:
        lo = max(0, e.start - 8)
        ctx = blob[lo:e.start].decode("utf-8", "replace")
        bad = blob[e.start:min(len(blob), e.start + 4)]
        return "%s（偏移 %d，坏字节 %s，前文 …%s）" % (e.reason, e.start,
                                                     bad.hex(), ctx)


def section_names(html):
    return SECTION.findall(html)


def control_chars(blob):
    """返回产物流里出现过的裸控制字符（字节值升序）；空列表表示干净。"""
    return sorted({b for b in blob if b in BAD_CTRL})


def describe_ctrl(codes):
    return "、".join("0x%02x" % b for b in codes)


def check_one(exe, mid, td):
    """返回该素材的问题列表；空列表表示三端都是合法 UTF-8。"""
    name = os.path.basename(mid)
    errs = []

    txt = os.path.join(td, "out.txt")
    html = os.path.join(td, "out.html")
    xml = os.path.join(td, "out.xml")

    r = subprocess.run([exe, mid, "--output", txt], capture_output=True, timeout=120)
    if r.returncode != 0:
        return ["%s: 文本谱导出失败 rc=%d" % (name, r.returncode)]
    r = subprocess.run([exe, mid, "--html", html], capture_output=True, timeout=120)
    if r.returncode != 0:
        return ["%s: HTML 导出失败 rc=%d" % (name, r.returncode)]
    r = subprocess.run([exe, mid, "--musicxml", xml], capture_output=True, timeout=120)
    if r.returncode != 0:
        return ["%s: MusicXML 导出失败 rc=%d" % (name, r.returncode)]

    for path, tag in ((txt, "文本谱"), (html, "HTML"), (xml, "MusicXML")):
        blob = open(path, "rb").read()
        why = invalid_utf8(blob)
        if why:
            errs.append("%s: %s 不是合法 UTF-8 —— %s" % (name, tag, why))
            continue
        if "\ufffd" in blob.decode("utf-8"):
            errs.append("%s: %s 里出现了替换字符 U+FFFD（说明字节被悄悄改写过）"
                        % (name, tag))
        bad = control_chars(blob)
        if bad:
            errs.append("%s: %s 里有裸控制字符 %s（XML 禁止、HTML 是解析错误、"
                        "终端会执行 ESC 序列）" % (name, tag, describe_ctrl(bad)))

    # 诊断输出同样打到终端：--list-tracks 的表格里出现 ESC，就等于让文件内容
    # 执行终端命令。这条不算「输出卫生」而是注入面，必须一起守。
    r = subprocess.run([exe, mid, "--list-tracks"], capture_output=True, timeout=120)
    if r.returncode != 0:
        errs.append("%s: --list-tracks 失败 rc=%d" % (name, r.returncode))
    else:
        bad = control_chars(r.stdout)
        if bad:
            errs.append("%s: --list-tracks 输出里有裸控制字符 %s"
                        % (name, describe_ctrl(bad)))

    # MusicXML 是唯一会被外部解析器严格校验的一端：解码过关还不够，
    # 必须真的能被 XML 解析器读完。
    try:
        ET.parse(xml)
    except ET.ParseError as e:
        errs.append("%s: MusicXML 无法解析 —— %s" % (name, e))

    hb = open(html, "rb").read()
    if not invalid_utf8(hb):
        for s in section_names(hb.decode("utf-8")):
            if len(s) > MAX_SECTION_CHARS:
                errs.append("%s: 段落名 %d 个字符，超过上限 %d（%r）"
                            % (name, len(s), MAX_SECTION_CHARS, s[:20]))
    return errs


def audit_fixtures(exe):
    errs = []
    for p in sorted(glob.glob(os.path.join(FIX, "*.mid"))):
        with tempfile.TemporaryDirectory() as td:
            errs.extend(check_one(exe, p, td))
    return errs


def self_test():
    n = 0
    failed = []

    def ck(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            failed.append(msg)

    good = "副歌第一段".encode("utf-8")
    ck(invalid_utf8(good) is None, "完整汉字串应判为合法 UTF-8")
    ck(invalid_utf8(b"") is None, "空串应判为合法 UTF-8")
    ck(invalid_utf8(b"ABC\x7f") is None, "ASCII 应判为合法 UTF-8")

    # 三个汉字的最后一个被劈成半截：这正是 substr(0, 40) 留下的残骸
    cut = "副歌第".encode("utf-8")[:-1]
    why = invalid_utf8(cut)
    ck(why is not None, "半截汉字必须判为非法")
    ck(why and "偏移 6" in why, "非法位置应指向第 7 字节，实际: %r" % why)
    ck(invalid_utf8(b"\xe5\x89") is not None, "孤立的首字节必须判为非法")
    ck(invalid_utf8(b"\x80") is not None, "孤立的续字节必须判为非法")
    ck(invalid_utf8(b"\xff\xfe") is not None, "UTF-16 BOM 不是合法 UTF-8")

    # 四字节序列（emoji / 生僻字）整体完整才合法
    ck(invalid_utf8("\U0001F3B5".encode("utf-8")) is None, "四字节序列应合法")
    ck(invalid_utf8("\U0001F3B5".encode("utf-8")[:3]) is not None,
       "四字节序列被切掉一字节必须判为非法")

    # 段落名扫描只认 <span class='section'>，不能被 <span class='chord'> 干扰
    h = ("<span class='chord'>[C]</span>"
         "<span class='section'>前奏</span>"
         "<span class='section'>副歌</span>")
    ck(section_names(h) == ["前奏", "副歌"], "段落名提取: %r" % section_names(h))
    ck(section_names("<span class='chord'>C</span>") == [], "无段落名时为空")

    # v1.122.0：控制字符闸门本身要能把「干净」和「脏」分开，否则它只是个
    # 永远返回空列表的摆设。
    ck(control_chars(b"abc\n\t\r") == [], "换行 / tab / CR 是合法空白，不算脏")
    ck(control_chars(b"") == [], "空文件干净")
    ck(control_chars(b"A\x1b[31mB") == [0x1B], "ESC 必须被抓出来")
    ck(control_chars(b"A\x01B\x00C") == [0x00, 0x01], "多个控制字符按字节值排序")
    ck(control_chars(b"A\x7fB") == [0x7F], "DEL 也算控制字符")
    ck(control_chars("中文".encode("utf-8")) == [],
       "UTF-8 多字节序列不会被误判（0x80..0xBF 不是 C0）")
    ck(describe_ctrl([0x01, 0x1B]) == "0x01、0x1b", "错误信息里的写法")


    # v1.80.0：源头就是旧编码时，裸字节同样绝不能流到三端。
    # 这条闸门只看产物，所以它同时守住「工具切坏」与「文件本来就不是 UTF-8」。
    ck(invalid_utf8("副歌段落".encode("gbk")) is not None,
       "GBK 裸字节必须判为非法")
    ck(invalid_utf8("サビ段落".encode("shift_jis")) is not None,
       "Shift-JIS 裸字节必须判为非法")
    ck(invalid_utf8(b"it\x92s Piano") is not None,
       "Windows-1252 弯引号字节必须判为非法（折叠前）")

    print("check_utf8 self-test: %d 项，%d 失败" % (n, len(failed)))
    for f in failed:
        print("  [FAIL]", f)
    return 0 if not failed else 1


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    exe = default_exe()
    if "--exe" in args:
        exe = args[args.index("--exe") + 1]
    if not os.path.exists(exe):
        print("找不到 CLI：", exe)
        return 2
    errs = audit_fixtures(exe)
    if errs:
        print("三端 UTF-8 校验失败：%d 处" % len(errs))
        for e in errs[:20]:
            print("  [FAIL]", e)
        return 1
    print("三端 UTF-8 校验：全部 fixture 的文本谱 / HTML / MusicXML 均合法，"
          "MusicXML 全部可解析")
    return 0


if __name__ == "__main__":
    sys.exit(main())

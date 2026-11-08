#!/usr/bin/env python3
"""WASM 产物同步守护：改了会进 wasm 的源码，必须一起提交重编后的产物。

背景：`tests/wasm_smoke.mjs` 第 34 组只比对 `include/midi2jianpu/version.hpp`
与 wasm 页脚里的版本号。若有人改了 `src/` 却**不升版本、也不重编**
`docs/wasm/`，页脚版本仍然与源码一致，那道守护会静默放过——v1.35~v1.37
连续三版「CLI 发版了、网页产物还停在 1.34.0」就是这么发生的。本脚本退到更
下面一层，直接看「这次提交到底动了哪些文件」，不依赖版本号，也不编译 wasm。

判定规则（宁可多重编一次，也不要网页端静默滞后）：
  * 正向：触发集（`src/**`、`include/**`、`cmake/**`、`CMakeLists.txt`、
    `build/build_wasm.cmd`，即任何会改变 wasm 二进制的输入）里有变更 ->
    `docs/wasm/midi2jianpu.wasm` 必须也在本次变更里。
  * 反向：`docs/wasm/midi2jianpu.wasm` 变了（说明确实重编过），
    `include/midi2jianpu/version.hpp` 也必须一起变。否则源码改了、产物也
    重编了，却没升版本：第 34 组看到的页脚与源码版本仍然「一致」，
    线上用户和 CDN 缓存无法区分新旧二进制，这次发版等于没有版本号。
  * 胶水层 `docs/wasm/midi2jianpu.js` 变了 -> `.wasm` 也必须一起变。
  * 只改测试 / 文档 / CI 配置 / `docs/index.html` -> 不要求重编。
  * 极少数「只重编产物、确实不升版本」的补提交，用
    `--allow-artifact-only` 显式放行，别悄悄改规则。

用法：
    python tests/check_wasm_sync.py                          # 自动推 base..head
    python tests/check_wasm_sync.py --base HEAD~3 --head HEAD
    python tests/check_wasm_sync.py --changed src/midifile.cpp README.md
    python tests/check_wasm_sync.py --changed docs/wasm/midi2jianpu.wasm \
        --allow-artifact-only                 # 显式放行的纯产物补提交
    python tests/check_wasm_sync.py --self-test             # 纯逻辑自测，不碰 git

CI 里 `actions/checkout` 必须带 `fetch-depth: 0`，否则 base commit 不在本地。
退出码非 0 即失败，可直接用于 GitHub Actions。
"""
import argparse
import json
import os
import subprocess
import sys

# ---- 会改变 wasm 二进制的输入（路径用 / 分隔，相对仓库根）----
INPUT_PREFIXES = ("src/", "include/", "cmake/")
INPUT_FILES = ("CMakeLists.txt", "build/build_wasm.cmd")
WASM_BINARY = "docs/wasm/midi2jianpu.wasm"
WASM_GLUE = "docs/wasm/midi2jianpu.js"
VERSION_HEADER = "include/midi2jianpu/version.hpp"


def normalize(path):
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def is_wasm_input(path):
    return path in INPUT_FILES or path.startswith(INPUT_PREFIXES)


def evaluate(changed, allow_artifact_only=False):
    """给定变更文件列表，返回问题描述列表（空列表 = 通过）。"""
    files = {normalize(p) for p in changed if p and p.strip()}
    inputs = sorted(p for p in files if is_wasm_input(p))
    problems = []
    if inputs and WASM_BINARY not in files:
        problems.append(
            "本次变更改了会进 WASM 的源码，却没有提交重编后的 %s：\n"
            "    %s\n"
            "  请运行 build/build_wasm.cmd（便携 emsdk 在 ../tools/emsdk），\n"
            "  把 build-wasm/ 里的 .wasm / .js 拷进 docs/wasm/ 后一起提交。"
            % (WASM_BINARY, "\n    ".join(inputs)))
    if WASM_GLUE in files and WASM_BINARY not in files:
        problems.append(
            "胶水层 %s 变了，%s 却没变；emcc 总是成对输出这两个文件。"
            % (WASM_GLUE, WASM_BINARY))
    # 反向：产物重编过，版本号必须跟着升。否则 wasm_smoke 第 34 组看到的
    # 「页脚版本 == version.hpp」仍成立，却掩盖了一次没有版本号的发版。
    if (WASM_BINARY in files and VERSION_HEADER not in files
            and not allow_artifact_only):
        problems.append(
            "%s 变了（说明重编过），%s 却没变：\n"
            "    请升 M2J_VERSION 后重编并一起提交；否则页脚版本虽然\n"
            "    与源码一致，线上仍无法区分新旧二进制。\n"
            "    确属纯产物补提交时用 --allow-artifact-only 显式放行。"
            % (WASM_BINARY, VERSION_HEADER))
    return problems


def git(*args):
    p = subprocess.run(["git", *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.returncode, (p.stdout or "").strip(), (p.stderr or "").strip()


def reachable(rev):
    return git("cat-file", "-e", rev + "^{commit}")[0] == 0


def event_json():
    path = os.environ.get("GITHUB_EVENT_PATH", "")
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def resolve_range():
    """返回 (base, head)；无法确定时返回 (None, None) 表示跳过。"""
    event = os.environ.get("GITHUB_EVENT_NAME", "")
    info = event_json()
    head = os.environ.get("GITHUB_SHA") or "HEAD"
    base = None

    if event == "pull_request":
        base = (info.get("pull_request", {}).get("base", {}).get("sha") or "")
    elif event == "push":
        before = info.get("before", "")
        # 新分支 / force push 时 before 是全 0，或指向一个浅克隆里没有的提交。
        if before and set(before) != {"0"} and reachable(before):
            base = before

    if not base:
        base = "HEAD~1"
    if not reachable(head):
        head = "HEAD"
    if not reachable(base):
        # 只有首次提交（或浅克隆）才走到这里。浅克隆是配置问题，必须报出来，
        # 不能假装通过——那样守护就形同虚设。
        if not reachable("HEAD~1"):
            print("[skip] 仓库没有可比较的父提交（首次提交）")
            return None, None
        print("[warn] base %s 不可达；请给 actions/checkout 加 fetch-depth: 0" % base,
              file=sys.stderr)
        return "SKIP", head
    return base, head


def self_test():
    cases = [
        ([], 0, "空变更"),
        (["README.md", "BUILD.md", ".github/workflows/ci.yml"], 0, "只改文档 / CI"),
        (["tests/smoke.py", "tests/fixtures/sample.mid"], 0, "只改测试与素材"),
        (["docs/index.html"], 0, "只改网页，不碰 wasm 产物"),
        (["src/midifile.cpp"], 1, "只改源码必须失败"),
        (["include/midi2jianpu/version.hpp"], 1, "只升版本必须失败"),
        (["CMakeLists.txt"], 1, "只改构建清单必须失败"),
        (["build/build_wasm.cmd"], 1, "只改 WASM 编译脚本必须失败"),
        (["src/midifile.cpp", WASM_BINARY, VERSION_HEADER], 0, "源码 + 产物 + 版本号通过"),
        (["src/midifile.cpp", "include/midi2jianpu/version.hpp",
          WASM_BINARY, WASM_GLUE], 0, "典型发版提交通过"),
        ([WASM_GLUE], 1, "胶水层单独变更必须失败"),
        ([WASM_GLUE, WASM_BINARY, VERSION_HEADER], 0, "胶水层 + 二进制 + 版本号通过"),
    ]
    extra = [
        ([WASM_BINARY], 1, "只重编产物却没升版本，反向守护必须失败"),
        ([WASM_BINARY, VERSION_HEADER], 0, "产物 + 版本号通过"),
    ]
    bad = 0
    for changed, expected, label in cases + extra:
        problems = evaluate(changed)
        ok = (len(problems) == expected)
        print("  [%s] %s" % ("PASS" if ok else "FAIL", label))
        if not ok:
            print("        期望 %d 个问题，实际 %d：%s"
                  % (expected, len(problems), problems))
            bad += 1
    # 显式放行开关只影响反向规则，正向规则仍然生效。
    for changed, expected, label in [
        ([WASM_BINARY], 0, "--allow-artifact-only 放行纯产物补提交"),
        (["src/midifile.cpp"], 1, "--allow-artifact-only 不豁免漏重编"),
    ]:
        problems = evaluate(changed, allow_artifact_only=True)
        ok = (len(problems) == expected)
        print("  [%s] %s" % ("PASS" if ok else "FAIL", label))
        if not ok:
            print("        期望 %d 个问题，实际 %d：%s"
                  % (expected, len(problems), problems))
            bad += 1
    total = len(cases) + len(extra) + 2
    print("自测 %d 组，失败 %d 组" % (total, bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="WASM 产物同步守护")
    ap.add_argument("--base", help="比较起点（commit-ish）")
    ap.add_argument("--head", help="比较终点（commit-ish）")
    ap.add_argument("--changed", nargs="*", metavar="FILE",
                    help="直接给定变更文件列表，跳过 git（自测 / 本地演示）")
    ap.add_argument("--self-test", action="store_true", help="只跑纯逻辑自测")
    ap.add_argument("--allow-artifact-only", action="store_true",
                    help="显式放行「只重编 wasm 产物、不升版本」的补提交")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if args.changed is not None:
        changed = args.changed
        print("变更清单（--changed）：%d 个文件" % len(changed))
    else:
        base = args.base
        head = args.head or "HEAD"
        if not base:
            base, auto_head = resolve_range()
            if base is None:
                return 0
            if base == "SKIP":
                return 2
            if not args.head:
                head = auto_head
        rc, out, err = git("diff", "--name-only", base, head)
        if rc != 0:
            print("[fail] git diff %s %s 失败：%s" % (base, head, err),
                  file=sys.stderr)
            return 2
        changed = out.splitlines()
        print("比较 %s..%s：%d 个变更文件" % (base, head, len(changed)))

    problems = evaluate(changed, allow_artifact_only=args.allow_artifact_only)
    if not problems:
        print("[ok] WASM 产物与源码同步（%d 个变更文件）" % len(changed))
        return 0
    for p in problems:
        print("[fail] " + p, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

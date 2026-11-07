#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「署名守护」装进当前仓库的本地配置。

用法（在仓库任意子目录运行都可以）::

    python tools/setup_repo_guard.py          # Windows 也可双击 setup_repo_guard.cmd

它只写 .git/config —— 随整个任务目录绿色复制，不碰全局 git 配置、
不需要管理员权限、不改任何已发布的历史，可重复运行。换一台电脑后跑一次，
把「本机全局 user.email 不是本人」这类隐患挡在提交之前。
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys

AUTHOR_NAME = "xueyuejue"
AUTHOR_EMAIL = "xueyuejue2026@tutamail.com"


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(args, text=True, encoding="utf-8",
                          errors="replace", capture_output=True, check=check)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = run("git", "rev-parse", "--show-toplevel", check=False)
    if root.returncode != 0:
        print("[安装] 当前目录不在 git 仓库里：%s" % root.stderr.strip(), file=sys.stderr)
        return 2
    repo = root.stdout.strip()

    hook = os.path.join(repo, ".githooks", "commit-msg")
    checker = os.path.join(repo, "tools", "check_commit_attribution.py")
    for path in (hook, checker):
        if not os.path.isfile(path):
            print("[安装] 缺少 %s；绿色复制时请务必带上 .git/、.githooks/、tools/"
                  % path, file=sys.stderr)
            return 2

    # 1) 作者身份锁死在仓库本地：不同电脑的全局配置不会把提交算到别人头上。
    run("git", "config", "--local", "user.name", AUTHOR_NAME)
    run("git", "config", "--local", "user.email", AUTHOR_EMAIL)

    # 2) 钩子路径也是仓库内的相对路径，目录整体搬家后依旧有效。
    run("git", "config", "--local", "core.hooksPath", ".githooks")

    # 3) 尽量补上可执行位（Windows 的 core.filemode=false 下 git 会忽略，
    #    Git for Windows 仍能用 sh 直接跑这个钩子）。
    try:
        mode = os.stat(hook).st_mode
        os.chmod(hook, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass

    print("[安装] 仓库根目录：%s" % repo)
    for key in ("user.name", "user.email", "core.hooksPath"):
        value = run("git", "config", "--local", "--get", key).stdout.strip()
        print("[安装] %-16s = %s" % (key, value))

    # 4) 顺手自查一遍历史，确认这台机器拿到的副本是干净的。
    # 用当前解释器而不是硬编码 python / python3：另一台机器可能只装了 py，
    # 或者把解释器放在非标准路径，脚本照样能跑。
    result = run(sys.executable, checker, "--history", check=False)
    print(result.stdout.strip() or result.stderr.strip())
    print("[安装] 完成。之后每次 git commit 都会自动校验提交信息。")
    if result.returncode != 0:
        print("[安装] 注意：历史自查未通过，请按上面的提示处理。", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())

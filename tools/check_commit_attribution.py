#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提交署名守护：保证这个仓库永远只有一位作者、没有 AI 署名行。

为什么要做这件事
----------------
GitHub 仓库首页的 Contributors 不只是按 commit author 统计，还会解析提交信息里的
`Co-Authored-By: Name <email>`，把邮箱对应的账号算成第二个贡献者。本项目曾因为
一行自动追加的署名，被迫重写 39 个提交、重建 21 个 tag、再切换默认分支才能把
GitHub 服务端那份 append-only 统计缓存刷新掉。

这个脚本在两处运行，任何一台电脑、任何办公环境都拦得住：

  * `.githooks/commit-msg`：提交当场拦截（由 `core.hooksPath` 启用）；
  * `.github/workflows/commit-guard.yml`：push 之后服务端复查，防止有人
    用 `--no-verify` 绕过钩子，也防止换机器后忘了跑 setup 脚本。

用法
----
  python tools/check_commit_attribution.py --file .git/COMMIT_EDITMSG
  python tools/check_commit_attribution.py --history

`--history` 检查 HEAD 可达的全部提交（不检查 `--all` 里的本地备份分支），
既能在 CI 里兜底，也能在日常自查时确认历史没被重新污染。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

AUTHOR_EMAIL = "xueyuejue2026@tutamail.com"
# 最早两个通过 GitHub 网页创建的提交，committer 是 GitHub 自己；作者仍是一人。
ALLOWED_COMMITTER_EMAILS = {AUTHOR_EMAIL, "noreply@github.com"}

# 只拦「会被 GitHub 或常见 AI 工具解析成署名」的行，不拦正文里普通的技术讨论。
FORBIDDEN_LINES = [
    (re.compile(r"^\s*co[-\s]?authored[-\s]?by\s*:", re.I),
     "Co-Authored-By 署名行（GitHub 会据此增加贡献者）"),
    (re.compile(r"^\s*(co[-\s]?trailered[-\s]?by|assisted[-\s]?by|ai[-\s]?generated[-\s]?by)\s*:", re.I),
     "AI 辅助署名行"),
    (re.compile(r"@(?:anthropic|openai)\.com\b", re.I),
     "AI 厂商邮箱"),
    (re.compile(r"^\s*generated\s+with\b", re.I),
     "AI 工具生成声明行"),
    (re.compile(r"^\s*signed[-\s]?off[-\s]?by\s*:.*\b(claude|gpt|copilot|gemini)\b", re.I),
     "AI 工具的 sign-off 行"),
]


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git %s 失败：%s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def visible_lines(raw: str) -> list[str]:
    """去掉 git 模板注释与 scissors 之后的内容，只检查真正会进 commit 的行。"""
    out = []
    for line in raw.splitlines():
        if line.startswith("# ------------------------ >8 ------------------------"):
            break
        if line.startswith("#"):
            continue
        out.append(line)
    return out


def scan_message(raw: str, label: str) -> list[str]:
    problems = []
    for lineno, line in enumerate(visible_lines(raw), 1):
        for pattern, why in FORBIDDEN_LINES:
            if pattern.search(line):
                problems.append(
                    "%s 第 %d 行：%s\n      原文：%s" % (label, lineno, why, line.strip()[:160]))
                break
    return problems


def ident_email(kind: str) -> str:
    """kind 取 GIT_AUTHOR_IDENT / GIT_COMMITTER_IDENT。"""
    try:
        text = git("var", kind)
    except RuntimeError:
        return ""
    if "<" not in text or ">" not in text:
        return ""
    return text.rsplit("<", 1)[1].split(">", 1)[0].strip().lower()


def check_hook(msg_path: str) -> list[str]:
    try:
        with open(msg_path, "r", encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except OSError as e:
        return ["无法读取提交信息文件 %s：%s" % (msg_path, e)]

    problems = scan_message(raw, "本次提交")
    author = ident_email("GIT_AUTHOR_IDENT")
    committer = ident_email("GIT_COMMITTER_IDENT")
    if author and author != AUTHOR_EMAIL:
        problems.append("本次提交的 author 邮箱是 %s，应为 %s（换电脑后请运行 "
                        "tools/setup_repo_guard.py）" % (author, AUTHOR_EMAIL))
    if committer and committer not in ALLOWED_COMMITTER_EMAILS:
        problems.append("本次提交的 committer 邮箱是 %s，应为 %s" % (committer, AUTHOR_EMAIL))
    return problems


def check_history() -> list[str]:
    hashes = git("log", "--format=%h", "HEAD").splitlines()
    authors = git("log", "--format=%ae", "HEAD").splitlines()
    committers = git("log", "--format=%ce", "HEAD").splitlines()
    # %B%x00 把每条 message 用 NUL 隔开；git 还会在每条记录后补一个换行，
    # 切出来的第 i 块前面可能带 '\n'，不影响逐行扫描。
    blocks = git("log", "--format=%B%x00", "HEAD").split("\x00")

    problems = []
    for i, h in enumerate(hashes):
        label = "提交 %s" % h
        if i < len(blocks):
            problems.extend(scan_message(blocks[i], label))
        if i < len(authors) and authors[i].strip().lower() != AUTHOR_EMAIL:
            problems.append("%s 的 author 邮箱是 %s，应为 %s" %
                            (label, authors[i].strip(), AUTHOR_EMAIL))
        if i < len(committers) and committers[i].strip().lower() not in ALLOWED_COMMITTER_EMAILS:
            problems.append("%s 的 committer 邮箱是 %s，不在白名单里" %
                            (label, committers[i].strip()))
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="检查提交信息署名与作者身份")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--file", metavar="PATH", help="检查一个 commit message 文件（钩子用）")
    g.add_argument("--history", action="store_true", help="检查 HEAD 可达的全部提交（CI 用）")
    args = ap.parse_args()

    try:
        problems = check_hook(args.file) if args.file else check_history()
    except (RuntimeError, OSError) as e:
        print("[署名守护] 检查无法完成：%s" % e, file=sys.stderr)
        return 2

    if not problems:
        scope = args.file if args.file else "HEAD 历史"
        print("[署名守护] %s 检查通过：作者唯一、无 AI 署名行。" % scope)
        return 0

    print("[署名守护] 发现 %d 处问题：\n" % len(problems), file=sys.stderr)
    for p in problems:
        print("  - " + p, file=sys.stderr)
    print("\n提交信息里不要出现自动生成的署名行；也不要用 --no-verify 跳过钩子。",
          file=sys.stderr)
    print("换电脑 / 绿色复制目录后，先运行一次 tools/setup_repo_guard.py。",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

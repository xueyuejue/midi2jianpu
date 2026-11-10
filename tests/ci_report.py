#!/usr/bin/env python3
"""CI 失败报告：把冒烟失败项写成 GitHub Actions 注解与 step summary。

背景：CLI 冒烟 step 失败时，GitHub 只自动生成一条
"Process completed with exit code 1." 注解。仓库没有管理员权限时，下载
job 日志的接口会返回 403，于是「哪一组、哪一条断言挂了」只能靠翻网页日志；
v1.56.0 的 tag run 红了却定位不了，就是卡在这里。

本模块只做纯文本格式化与写文件，不依赖 exe、不依赖第三方库：
  * `annotation_lines()`：生成 ::error 工作流命令。这些行会被 GitHub 解析成
    check-run annotations，普通的 annotations API 就能读到，不需要日志下载权限。
  * `summary_markdown()`：生成 $GITHUB_STEP_SUMMARY 里的 Markdown 全量清单。
    注解每个 step 有条数上限，完整信息放在摘要里。
  * `report_failures()` / `report_exception()`：失败项与未捕获异常的上报入口。
    非 CI 环境一律不写注解、不写摘要，本地手工跑测试的输出与旧版一致。
  * 两者都接受 `emit` 参数，便于 `smoke.py --self-test` 在不产生真实注解的
    前提下验证格式化结果。

工作流命令的转义规则见 GitHub 文档：data 部分要转义 % CR LF；property
部分还要额外转义 : 和 ,。
"""
import os

# 每个 step 的注解条数有上限（GitHub 当前是 10），留一条给系统自动生成的
# "Process completed with exit code 1."，再留一条给「其余见摘要」汇总。
MAX_ANNOTATIONS = 8


def escape_data(text):
    """工作流命令 ::error:: 之后 data 部分的转义。"""
    return (str(text).replace("%", "%25")
                     .replace("\r", "%0D")
                     .replace("\n", "%0A"))


def escape_property(text):
    """工作流命令 title=... 属性值的转义。"""
    return (str(text).replace("%", "%25")
                     .replace("\r", "%0D")
                     .replace("\n", "%0A")
                     .replace(":", "%3A")
                     .replace(",", "%2C"))


def annotation_lines(failures, limit=MAX_ANNOTATIONS):
    """把 [(组号, 断言, 附加信息)] 转成 ::error 行列表。"""
    lines = []
    for section, msg, extra in failures[:limit]:
        title = escape_property("CLI 冒烟 %s" % (section or "未分组"))
        line = "::error title=%s::%s" % (title, escape_data(msg))
        if extra:
            line += " | " + escape_data(str(extra)[:400])
        lines.append(line)
    rest = len(failures) - limit
    if rest > 0:
        lines.append("::error title=%s::另有 %d 项失败，完整清单见本次 step summary。"
                     % (escape_property("CLI 冒烟汇总"), rest))
    return lines


def _cell(text):
    """Markdown 表格单元格：竖线转义、换行压平，避免撑破表格。"""
    return (str(text).replace("|", "\\|")
                     .replace("\r", " ")
                     .replace("\n", " "))


def summary_markdown(failures, context=None):
    """生成写入 $GITHUB_STEP_SUMMARY 的全量失败清单。"""
    context = context or {}
    out = ["## CLI 冒烟失败 %d 项" % len(failures), ""]
    for key in ("ref", "sha", "exe", "fixtures"):
        if context.get(key):
            out.append("- %s: `%s`" % (key, _cell(context[key])))
    out += ["", "| 组 | 断言 | 附加信息 |", "| --- | --- | --- |"]
    for section, msg, extra in failures:
        out.append("| %s | %s | %s |" % (
            _cell(section or "-"), _cell(msg), _cell((extra or "")[:400])))
    return "\n".join(out) + "\n"


def in_ci():
    return os.environ.get("GITHUB_ACTIONS", "").lower() == "true"


def _emit(lines):
    for line in lines:
        print(line, flush=True)


def _write_summary(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return False
    with open(path, "a", encoding="utf-8") as fp:
        fp.write(text)
    return True


def report_failures(failures, context=None, emit=None):
    """断言失败上报。返回是否真的输出了注解。"""
    if not failures or not in_ci():
        return False
    (emit or _emit)(annotation_lines(failures))
    _write_summary(summary_markdown(failures, context))
    return True


def report_exception(traceback_text, emit=None):
    """未捕获异常上报：注解给最后一行，摘要给完整 traceback。"""
    if not in_ci():
        return False
    text = traceback_text or ""
    tail = text.strip().splitlines()[-1] if text.strip() else "unknown error"
    (emit or _emit)(["::error title=%s::%s"
                     % (escape_property("CLI 冒烟异常中止"), escape_data(tail))])
    _write_summary("## CLI 冒烟异常中止\n\n```text\n%s\n```\n" % text.rstrip())
    return True

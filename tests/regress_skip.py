#!/usr/bin/env python3
"""零回归比对（--regress）的排除项：本轮新增的素材。

`--regress X` 的语义是「同一份素材，新版与旧版输出应当一致（或只差本轮
有意改动的那一处）」。但**本轮新增**的素材在旧版里根本不存在：拿冻结的旧
exe 去跑它，产出的是「旧版对新文件的第一次渲染」，不是回归，逐字节比对必然
全部报红，把真正要看的那几行淹掉。

所以新增素材要在这里登记一次，各个 `--regress` 比对会跳过它，并在输出里留
一条 NOTE 说明「跳过 N 份本轮新增素材」，不静默放过。

注意：登记只影响 `--regress` 这一条路径。非回归的常规审计（`--self-test`
与全量 fixture 审计）仍然会跑这些素材，它们本来就应该被守护。
"""
import os

# 与其在 8 个脚本里各写一份，不如集中一处；加新素材时只改这一行。
# v1.87.0 新增 stray_pedal.mid（解析期踏板过闸）与 stray_diag.mid
# （解析期诊断计数过闸）：旧版渲染它们是「第一次见」，不参与逐字节零回归，
# 但要被常规审计守护。v1.86.0 的 stray_events.mid 对本轮基线已是「旧版见过」，
# 移出跳过清单，让它重新参与逐字节回归——它的页眉丢弃计数由 10 修正为 12
# （v1.86 漏数了 2 个解析期消费的事件），由 .probe/regress_v187.py 精确豁免。
NEW_FIXTURES = {"stray_pedal.mid", "stray_diag.mid"}


def skip(path):
    """该素材是否跳过零回归比对（本轮新增）。"""
    return os.path.basename(str(path)) in NEW_FIXTURES


def note():
    """统一格式的跳过提示；没有登记新增素材时返回 None（不打印）。"""
    if not NEW_FIXTURES:
        return None
    return ("[NOTE] 跳过 %d 份本轮新增素材（旧版没有它们，零回归不比对）：%s"
            % (len(NEW_FIXTURES), " / ".join(sorted(NEW_FIXTURES))))

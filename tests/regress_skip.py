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
# v1.92.0 只新增一道参数矩阵守卫（tests/check_cli_matrix.py），未加任何
# 素材；基线 _baseline_v1911.exe 已含 v1.90 的 html_meta_inject.mid，故本轮
# 无需跳过任何素材——它连同其余 105 份一起参与逐字节零回归比对。
# v1.93.0 只把 fuzz 的突变算子从 7 个扩到 12 个（含多种子），同样未加素材；
# 基线 _baseline_v1920.exe 已含全部 105 份，故仍无需跳过。
NEW_FIXTURES = set()


def skip(path):
    """该素材是否跳过零回归比对（本轮新增）。"""
    return os.path.basename(str(path)) in NEW_FIXTURES


def note():
    """统一格式的跳过提示；没有登记新增素材时返回 None（不打印）。"""
    if not NEW_FIXTURES:
        return None
    return ("[NOTE] 跳过 %d 份本轮新增素材（旧版没有它们，零回归不比对）：%s"
            % (len(NEW_FIXTURES), " / ".join(sorted(NEW_FIXTURES))))

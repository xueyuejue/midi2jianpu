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
# v1.134.0 新增 midbar_meter.mid（0x58 落在小节中途）：旧版对它导出的
# MusicXML 里，被截短的小节没标 implicit，正是本轮要改掉的那一处。它已从
# v1.140.0 起撤出本表（v1.139 基线已含它，应照常参与零回归）。
# v1.140.0 的 smpte_tempo0.mid 从这一版起撤出本表（v1.140 基线已含它）。
# v1.141.0 新增 pitch0.mid（四个合法 MIDI 音高 0~3）：旧版 HTML 把 pitch 0
# 当休止符吞掉第一个字，与修复后的谱面必然不同，登记为跳过。它从 v1.149.0
# 起撤出本表——基线已含它的修复后输出，应照常参与零回归。
#
# v1.122.0 的 hostile_name.mid 从这一版起撤出本表：它已经进了 v1.122.0 之后
# 的每一份基线，旧版对它的三端输出与新版一致，本就该参与逐字节比对。
NEW_FIXTURES = {"octave_extremes.mid"}

# v1.144.0 曾经有一张 HTML_CHANGED = {"parts.mid", "overfull.mid"}：那一版把
# 倚音前面的空拍补成休止列，`--regress` 对这两份既有素材放行了「HTML 有意
# 变化」。v1.145.0 的基线已经包含 v1.144 的输出，它们不再特殊——该机制连同
# 两个守卫里的分支一并删除，恢复逐字节比对。以后若再有「本轮就是要改 HTML」
# 的修复，按同样办法临时开一张表，并在下一轮基线里清掉。

# v1.146.0 曾经有一张 XML_PADDED = {"sample.mid", "pedal_channels.mid",
# "sysex_gs_normal.mid", "sysex_unknown.mid"}：那一版给提前没音的 part 补齐
# 尾节，`--regress` 对这几份既有素材放行了「MusicXML 有意变大」。v1.147.0 的
# 基线已经包含 v1.146.0 的输出，它们不再特殊——该表连同 7 个守卫里的放行
# 分支一并删除，恢复逐字节比对。以后若再有「本轮就是要改某端输出」的修复，
# 按同样办法临时开一张表，并在下一轮基线里清掉。

# v1.149.0：八度点容器在 3 个点起带内联高度（`style='height:NNpx'`）。这是
# 纯排版修复——同一行里每一列的容器高度都一样、数字仍在同一条基线上，只有
# 原来被 flex 压成亚像素薄片的点恢复成看得见的大小。但它会改动 HTML 的字节，
# 而这两份既有素材本来就有 3 个以上的点（pitch0.mid 是 5 个低音点、
# grand_staff_wedge.mid 是 3 个），所以 --regress 要放行「HTML 有意变化」。
# 下一轮基线包含 v1.149.0 的输出后，本表清空。
HTML_CHANGED = {"pitch0.mid", "grand_staff_wedge.mid"}


def html_changed(path):
    """该素材的 HTML 本轮有意变化（仅限八度点容器高度）。"""
    return os.path.basename(str(path)) in HTML_CHANGED


def html_note():
    """有放行项时给一行提示，免得「HTML 变了」被误当成零回归通过。"""
    if not HTML_CHANGED:
        return None
    return ("[NOTE] 本轮 HTML 有意变化 %d 份（八度点容器高度）：%s"
            % (len(HTML_CHANGED), " / ".join(sorted(HTML_CHANGED))))


def skip(path):
    """该素材是否跳过零回归比对（本轮新增）。"""
    return os.path.basename(str(path)) in NEW_FIXTURES


def note():
    """统一格式的跳过提示；没有登记新增素材时返回 None（不打印）。"""
    if not NEW_FIXTURES:
        return None
    return ("[NOTE] 跳过 %d 份本轮新增素材（旧版没有它们，零回归不比对）：%s"
            % (len(NEW_FIXTURES), " / ".join(sorted(NEW_FIXTURES))))

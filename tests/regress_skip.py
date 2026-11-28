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
# v1.149.0 新增 octave_extremes.mid，已在 v1.149 基线里，本轮起照常参与比对。
#
# v1.151.0 新增 beam_octave.mid（高八度点 + 连减时线）：已在 v1.151 基线里，
# v1.152.0 起撤出本表，照常参与零回归。
#
# v1.152.0 新增 longbar.mid（12/4、8/4、16/4 整小节不切分长音）：已在 v1.152
# 基线里，v1.153.0 起撤出本表，照常参与零回归。
#
# v1.153.0 新增 section_struct.mid（段落名含 | ] [ 换行）：已在 v1.153 基线里，
# v1.154.0 起撤出本表，照常参与零回归。
#
# v1.154.0 新增 midi_lyric_grace.mid（倚音 + MIDI 内置歌词）：旧版基线里没有它，
# 且旧版把第一个字挂到不占列的倚音上（HTML 里那格是空的），逐字节比对必然报红，
# 登记为跳过。它已在 v1.155.0 基线里，本轮起撤出本表，照常参与零回归。
#
# v1.155.0 新增 odd_meter.mid（5/16、7/16、9/8 整小节单音 + 4/4 切分音）：
# 旧版把这些时值吸成 1 / 1.5 / 4 拍、缺的拍数在 MusicXML 里补成休止，三端
# 输出都与修复后不同，登记为跳过。它已在 v1.155.0 基线里，本轮起撤出本表，
# 照常参与零回归。
#
# v1.157.0 新增两份素材：triplet_quarter.mid（四分三连音曾被记成附点八分，
# 2/3 拍落不进 1/3 体系）与 meter_offset.mid（0x58 偏 1 tick，第 2 小节被截短、
# 跨边界的音拆出 40 tick 残片）。旧版三端输出都与修复后不同，登记为跳过。
# 它们已进入 v1.157.0 基线，本轮起撤出本表，照常参与零回归。
#
# v1.158.0 新增 offgrid_rest.mid（第 2 小节 3.708 拍才进声部）：旧版把那 1780
# tick 的休止按最近档印成 `0 - - -`（4 拍），文本谱与 HTML 都读出一小节
# 4.25 拍；新版拆成 3.625 拍 + 一个不足 1/8 拍的洞。三端输出必然不同，登记
# 为跳过；它已进入 v1.158.0 基线，本轮起撤出本表，照常参与零回归。
# v1.159.0 新增 rest_hole_nextbar.mid（洞记本小节的锁定素材）：旧版把洞漏给下一
# 小节吸收，末小节多一列 `-%%`。旧基线没有这份素材，登记为跳过；它进入
# v1.159.0 基线后，下一轮起撤出本表。
#
# v1.160.0 新增两份素材，旧基线里都没有：
# ① triplet_crossbar.mid（连续八分三连音跨 3 小节写在同一行）：旧版 writer 把
#    12 组共用一个巨型 `3(...)`，中间行丢掉左括号后被读成 12 拍；新版每组
#    3 音独立开合。② grace_hole_dev.mid（30 tick 倚音 + 30 tick 空隙 + 420 tick
#    主音）：旧版 verifier 不给 grace 列记 c.dur，把 4 拍读成 3 拍；新版补
#    addDev。两份的三端输出与 `--verify` 结论都变了，登记为跳过。
# 上一轮的 rest_hole_nextbar.mid 已进入 v1.159.0 基线，本轮起撤出本表，
# 照常参与零回归比对。
# v1.186.0 的 lyric_chord.mid 已进入 v1.186.0 基线，本轮起撤出本表。
# v1.187.0 新增 quant_lyric.mid（量化把两个差 6 tick 的音头并成一格，歌词
# 体检必须跟着降）；旧基线没有修复后的提示栏口径，登记跳过。
NEW_FIXTURES = {"quant_lyric.mid"}

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

# v1.149.0 的 HTML_CHANGED = {"pitch0.mid", "grand_staff_wedge.mid"}（单音列
# 3 个点起撑高）在 v1.150.0 基线里已包含，撤出本表，恢复逐字节比对。
#
# v1.150.0 的 HTML_CHANGED = {chords, harmony, modulation, pentatonic, pickup}
# （和弦 tone 八度点撑高）在 v1.151.0 基线里已包含，撤出本表，恢复逐字节比对。
#
# v1.151.0 没有对既有素材的 HTML 做任何改动：连减时线的 --oct-h 只在高八度点
# + 连线同时出现时写，全 112 份既有素材都不踩这个组合，新素材已跳过。
#
# v1.152.0 同样没有对既有素材的 HTML / XML 做任何改动：超长音只在拍数 > 4.5
# 才走新分支，全 113 份既有素材的 HTML --n 最大只到 3（=4 拍档）、XML 也没有
# 任何单段 > 4 拍的 whole（见 .probe/v1152/ 的取证），踩到本缺陷的只有新素材
# longbar.mid，它已在 NEW_FIXTURES 里跳过。故本表为空。
#
# v1.153.0 只改文本谱的段落名写入点（折叠 | ] [ 换行成全角）；全 113 份既有
# 素材的 0x06 marker 都不含这些结构字符，文本谱一字未动，HTML / XML 从头到尾
# 没碰。踩到本缺陷的只有新素材 section_struct.mid，它已在 NEW_FIXTURES 里跳过。
#
# v1.154.0 同样没有对既有素材做任何改动：① isTrackHeader 只认行尾的
# 「(N 音符)」（既有素材的轨名都不含这串字）；② 0 小节时的报告文案；
# ③ 失败时只印没过的那一侧；④ assignMidiLyrics 跳过倚音（既有素材里带倚音
# 的都没有内置歌词）；⑤ MusicXML 里拼不出标准时值的间隙不再写 <type>——
# 113 份素材的休止全都能精确拆分（check_xml_durations 全绿即为证），走不到
# 那个兜底分支。故本表仍为空。
#
# v1.156.0 只改 HTML 页眉里「调号是谁定的」这句话，且只在调用方明确给出
# 来源时才改（`--key` / `--gong` / `--transpose`）；`--regress` 跑的是不带
# 参数的三端默认输出，`keySourceNote` 为空、沿用旧判定，页眉一个字节不动。
# 故本表仍为空。
#
# v1.182.0 曾把两份 sysex 素材的 HTML 记为「跨小节非整拍续段档位有意变化」；
# v1.183.0 的基线已含修复后的输出，本表清空，恢复全量逐字节零回归。
HTML_CHANGED = set()

# v1.182.0 的 HTML_SKIP 已随基线清零；再次需要整份放行时按本轮有意变化登记，
# 并在下一轮基线包含修复输出后立即清空，不能长期挂着。
HTML_SKIP = set()


def html_skip(path):
    """该素材的 HTML 本轮整份有意变化（文本谱 / MusicXML 仍逐字节比）。"""
    return os.path.basename(str(path)) in HTML_SKIP


def html_changed(path):
    """该素材的 HTML 本轮有意变化（仅限八度点容器高度）。"""
    return os.path.basename(str(path)) in HTML_CHANGED


def html_note():
    """有放行项时给一行提示，免得「HTML 变了」被误当成零回归通过。"""
    names = sorted(HTML_CHANGED | HTML_SKIP)
    if not names:
        return None
    return ("[NOTE] 本轮 HTML 有意变化 %d 份：%s"
            % (len(names), " / ".join(names)))


def skip(path):
    """该素材是否跳过零回归比对（本轮新增）。"""
    return os.path.basename(str(path)) in NEW_FIXTURES


def note():
    """统一格式的跳过提示；没有登记新增素材时返回 None（不打印）。"""
    if not NEW_FIXTURES:
        return None
    return ("[NOTE] 跳过 %d 份本轮新增素材（旧版没有它们，零回归不比对）：%s"
            % (len(NEW_FIXTURES), " / ".join(sorted(NEW_FIXTURES))))

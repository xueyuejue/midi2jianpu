// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/version.hpp"
#include "cli_args.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// v1.96.0: 移调合理上限（半音）。--transpose 此前无任何范围校验，
// --transpose 999 会被静默接受并产出八度远超 MIDI/乐理范围的退化谱面
// （音符被抬到约 80+ 个八度）。±60 半音（±5 个八度）已覆盖一切真实移调需求，
// 与 --bars(1~8)/--pickup(短于小节)/--soft-pedal-scale(0~1) 的「语义越界即拒」口径一致。
static const int kMaxTransposeSemis = 60;

// 三端写文件共用：打开 / 写入 / flush / close 每一步都要检查。
// 旧版只管 << 完就打印「已写入」：目录不存在、路径是目录、磁盘满时
// 用户拿到的是 rc=0 加一句假成功，回头才发现谱子根本没生成。
// 这里刻意不开 binary：Windows 文本模式把 \n 写成 \r\n 是既有口径，
// 改了会让所有既有素材的输出逐字节变化。
static bool writeOutputFile(const std::string& path, const std::string& content,
                            const char* label) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "错误: 无法打开输出文件 " << path << "（" << label
                  << "）：检查目录是否存在、是否有写权限，或路径是不是目录\n";
        return false;
    }
    f << content;
    f.flush();
    if (!f) {
        std::cerr << "错误: 写入输出文件失败 " << path << "（" << label
                  << "），可能磁盘已满\n";
        return false;
    }
    f.close();
    if (f.fail()) {
        std::cerr << "错误: 关闭输出文件失败 " << path << "（" << label
                  << "），可能磁盘已满\n";
        return false;
    }
    return true;
}
static void printHelp() {
    std::cout <<
        "midi2jianpu " M2J_VERSION " - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>                 打印文本简谱\n"
        "  midi2jianpu <input.mid> --html out.html 导出 HTML 谱面\n"
        "  midi2jianpu <input.mid> --musicxml out.xml  导出 MusicXML(可用 MuseScore 打开)\n"
        "  midi2jianpu <input.mid> --key Am         手动指定调性(如 Am=小调, C=大调)\n"
        "  midi2jianpu <input.mid> --output out.txt  文本简谱输出到文件\n"
        "  midi2jianpu <input.mid> --track 0         只输出指定轨道（轨号从 0 起）\n"
        "  midi2jianpu <input.mid> --list-tracks     列出所有轨道（选 --track 前先看清）\n"
        "  midi2jianpu <input.mid> --transpose +2     移调(半音, 正负都可, ±60 内)\n"
        "  midi2jianpu <input.mid> --lyrics lyrics.lrc  加载歌词(优先于 MIDI 内置歌词)\n"
        "  midi2jianpu <input.mid> --no-midi-lyrics  忽略 MIDI 文件自带的歌词\n"
        "  midi2jianpu <input.mid> --pickup 1       手动指定弱起小节长度(拍, 须短于小节)\n"
        "  midi2jianpu <input.mid> --no-pickup      关闭弱起自动判定\n"
        "  midi2jianpu <input.mid> --html o.html --player  谱面内嵌试听播放器\n"
        "  midi2jianpu <input.mid> --html o.html --bars 4  每行固定 4 小节\n"
        "  midi2jianpu <input.mid> --bar-numbers          文本谱每行标出小节号\n"
        "  midi2jianpu <input.mid> --no-key-changes 忽略中途转调(全曲按一个调记谱)\n"
        "  midi2jianpu <input.mid> --gong C        手动指定五声调式的宫音(简谱 1)\n"
        "  midi2jianpu <input.mid> --no-pentatonic  关闭五声调式宫音自动校正\n"
        "  midi2jianpu <input.mid> --no-dynamics   不写力度/断音/滑音/段落等演奏记号\n"
        "  midi2jianpu <input.mid> --soft-pedal-scale 0.5  软踏板力度衰减系数(0~1, 默认 0.7)\n"
        "  midi2jianpu <input.mid> --no-soft-pedal   关闭 CC67 软踏板(还原被强制压暗的弱奏曲)\n"
        "  midi2jianpu <input.mid> --no-quantize    关闭时值量化(保留 MIDI 原始时长)\n"
        "  midi2jianpu --version\n"
        "  midi2jianpu --help\n";
}

// 解析 --key 参数: "C"=C大调, "Am"=a小调, "F#"=F#大调, "Bb"=降B大调
static bool parseKey(const std::string& s, m2j::KeyEstimate& out) {
    m2j::cli::KeySpec spec;
    if (!m2j::cli::parseKeySpec(s, spec)) return false;
    out = m2j::KeyEstimate{};          // 先清空，避免复用对象时残留上一次的 hint
    out.tonicPitchClass = spec.pitchClass;
    out.isMajor = spec.isMajor;
    // 用户写了 Gb 就该印 Gb：不记住这一侧，音级 6 会查表成六个升号（F#）。
    if (spec.flatSpelling)
        out.fifthsHint = m2j::keyFifthsOfFlatSpelling(spec.pitchClass);
    return true;
}

// 安全的整数解析：非法输入不再抛异常中断程序
static bool parseInt(const char* s, int& out) {
    try {
        size_t used = 0;
        std::string str(s);
        int v = std::stoi(str, &used);
        if (used != str.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// 拍数解析：弱起长度可以带小数（1.5 拍这种半拍弱起在歌本里并不罕见）
static bool parseBeats(const char* s, double& out) {
    double v;
    if (!m2j::cli::parseFiniteDouble(s, v) || v < 0) return false;
    out = v;
    return true;
}

// 浮点系数解析：软踏板衰减系数 0~1（含端点）。非法输入不再抛异常中断程序。
static bool parseDouble(const char* s, double& out) {
    return m2j::cli::parseFiniteDouble(s, out);
}

// --list-tracks：把文件里的每条轨列出来（序号 / 名称 / 旋律音数 / 乐器 /
// 歌词字数 / 是否鼓轨 / 用到的通道），让用户在 `--track N` 之前先看清该选哪条。
// 纯诊断，不参与转换，因此不改动任何既有转换输出，零回归。
// 统计口径收在 notation.cpp 的 trackSummaries()：网页的轨道下拉框读的是
// 同一份数据，两处各算一套的话，诊断说「4 轨」而网页只给 2 个选项。
//
// 这张表说的是**整份文件**，不是「当前选中轨」：按轨过滤后别的轨音符已经不在
// MidiData 里，列出来会全是「旋律 0 音」，而同一行的鼓件数却仍是全曲口径。
// 所以 main() 会在带 --track 时按全曲重解析一次再调这里（见下面的调用点）。
static void printTrackList(const m2j::MidiData& data, int selected = -1) {
    const std::vector<m2j::TrackSummary> tracks = m2j::trackSummaries(data);
    std::cout << "轨道列表 (format " << data.format << ", 共 " << tracks.size() << " 轨):\n";
    // 头部那个 16 位轨数是不可信的：导出器合并轨后忘了改、写溢出成 0 或
    // 0xFFFF、下载被截断……不解释一句的话，用户会以为下面漏列了轨道，
    // 或者奇怪为什么「共 0 轨」还能 --track 3。
    if (data.headerTrackCount != data.trackCount)
        std::cout << "注意: 文件头部写着 " << data.headerTrackCount << " 轨，实际解析到 "
                  << data.trackCount << " 个 MTrk 数据块（以实际为准）\n";
    bool anyMultiChannel = false;
    for (const auto& s : tracks) {
        std::cout << "  #" << s.index;
        // 轨名是文件内容：换行会把这张「一行一轨」的表撕成两行，ESC[31m
        // 这类 ANSI 序列会被终端当命令执行。这里是诊断而不是谱面，转成
        // 看得见的写法（\n / \" / \x1b）比丢掉更有用。40 字符上限与段落名
        // 一致，也保证一个几百字符的名字撑不爆整张表。
        if (!s.name.empty())
            std::cout << "  \"" << m2j::escapeForTerminal(s.name, 40) << "\"";
        if (s.drumNotes > 0 && s.notes == 0)
            std::cout << "   鼓轨 (跳过 " << s.drumNotes << " 鼓件音)";
        else if (s.drumNotes > 0)
            std::cout << "   旋律 " << s.notes << " 音, 鼓件音 " << s.drumNotes;
        else
            std::cout << "   旋律 " << s.notes << " 音";
        if (!s.instrument.empty()) std::cout << "   乐器: " << s.instrument;
        if (s.lyrics > 0) std::cout << "   歌词 " << s.lyrics << " 字";
        // 只写「跳过 192 鼓件音」等于什么都没交代：这段到底是底鼓加军鼓，
        // 还是一整片镲片，用户没法判断是不是导入错了轨。鼓名由诊断层统一
        // 映射，这里只管排版；原句保持原样，新内容另起一段，老输出不挪位。
        if (s.drumNotes > 0) {
            // 这张表是**完整诊断**，鼓件有多少种就列多少种：默认 4 项的
            // 上限是给空轨提示那种「顺带一提」用的，在这里会把第 5 种鼓件
            // 吞成一个「等」字，用户仍不知道丢的是什么。WASM track_list
            // 同样传全量，两端详略一致（sysex_ch9_normal.mid 有 5 种鼓件）。
            const std::string detail =
                m2j::drumInstrumentList(s, s.drumInstruments.size());
            if (!detail.empty()) std::cout << "   鼓件: " << detail;
        }
        // 一条轨混多个通道（format 0 的常态、DAW 的「主旋律+伴奏一条轨」）：
        // 转换层按通道拆声部，--track 却只能整轨取。不说这句的话，用户看到
        // 「乐器: 钢琴」与谱面上的两件乐器对不上，也没有任何线索去查。
        if (s.channels.size() > 1) {
            anyMultiChannel = true;
            std::cout << "   通道 ";
            for (size_t i = 0; i < s.channels.size(); i++) {
                if (i) std::cout << ",";
                std::cout << s.channels[i].first;
            }
            std::cout << "（" << s.channels.size() << " 个通道混在同一轨";
            // 只给通道号还没说完：同一轨的通道 0 是钢琴、通道 1 是小提琴时，
            // 「乐器: 钢琴」那一行只代表主导通道，得把其余乐器的名字交代出来。
            if (!s.channelInstruments.empty()) {
                std::cout << "；各通道乐器: ";
                for (size_t j = 0; j < s.channelInstruments.size(); j++) {
                    if (j) std::cout << ", ";
                    std::cout << "通道 " << s.channelInstruments[j].first << " "
                              << s.channelInstruments[j].second;
                }
            }
            std::cout << "）";
        }
        if (s.index == selected) std::cout << "   ← 当前 --track";
        std::cout << "\n";
    }
    // 选中的轨没有旋律时，列表本身才是用户最需要的东西（旧版这里直接
    // 报错退出、一行都不列），顺手把「为什么转不出来、旋律在哪」说完。
    if (selected >= 0) {
        if (selected >= (int)tracks.size()) {
            std::cout << "注意: --track " << selected << " 不存在：本文件实际只有 "
                      << tracks.size() << " 轨（0~" << (int)tracks.size() - 1 << "）\n";
        } else if (tracks[(size_t)selected].notes == 0) {
            const std::string hint = m2j::emptyTrackHint(data, selected);
            std::cout << "注意: --track " << selected << " 没有旋律音符";
            if (!hint.empty()) std::cout << "：" << hint;
            std::cout << "，直接转换会以退出码 4 结束\n";
        }
    }
    std::cout << "默认: 转换全部轨道（多轨各自成行）；--track N 只取第 N 轨（从 0 起）\n";
    if (data.format == 2)
        std::cout << "注意: format 2 各轨是独立序列，默认只转第 0 轨；--track N 可指定别的段\n";
    // format 0 把整首曲子塞进一条轨：轨号只有一个，通道才是真正的分层。
    // 这跟 format 2 的提示同一个位置、同一个目的——解释「为什么切不开」。
    if (data.format == 0 && anyMultiChannel)
        std::cout << "注意: format 0 把多个通道写进同一条轨，--track 只能整轨转换，"
                     "无法按通道拆分\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { printHelp(); return 1; }

    std::string input, htmlOut, keyArg, gongArg, txtOut, xmlOut, lyricsPath;
    int trackFilter = -1;
    bool listTracks = false;
    int transpose = 0;
    bool quantize = true;
    bool noMidiLyrics = false;
    bool noKeyChanges = false;
    bool noPentatonic = false;
    bool noDynamics = false;
    double softPedalScale = -1.0;  // <0 表示未指定（用默认 0.7）
    bool noSoftPedal = false;
    double pickupBeats = -1;   // <0 表示没指定（走自动判定）
    bool noPickup = false;
    bool player = false;       // --player：HTML 内嵌试听播放器
    int barsPerRow = 0;        // --bars N：每行固定 N 小节（0 = 自动折算）
    bool barNumbers = false;   // --bar-numbers：文本谱每行标出小节号
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        // ---- 带值选项必须有取值 ----
        // 旧版 `--output --html o.html` 会把 --html 当成输出文件名，谱子被静默
        // 写进一个叫 "--html" 的文件；`--html` 放在命令行末尾则只打一行「未知参数」
        // 然后照常输出文本谱——用户以为指定了，其实什么都没发生。
        // 负数合法取值（--transpose -2 / --pickup -1）不算选项，见 isOptionToken()。
        if (m2j::cli::optionNeedsValue(a)) {
            if (i + 1 >= argc || m2j::cli::isOptionToken(argv[i + 1])
                || argv[i + 1][0] == '\0') {
                std::cerr << "错误: " << a << " 需要一个值（用 --help 查看用法）\n";
                return 1;
            }
        }
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--version" || a == "-v") {
            std::cout << "midi2jianpu " M2J_VERSION "\n";
            return 0;
        }
        else if (a == "--html" && i + 1 < argc) { htmlOut = argv[++i]; }
        else if (a == "--key" && i + 1 < argc) {
            keyArg = argv[++i];
            m2j::cli::KeySpec ks;
            if (!m2j::cli::parseKeySpec(keyArg, ks)) {
                std::cerr << "错误: --key 需要形如 C / Am / F# / Bb 的调名，收到 \""
                          << keyArg << "\"\n";
                return 1;
            }
        }
        else if (a == "--gong" && i + 1 < argc) {
            gongArg = argv[++i];
            m2j::cli::KeySpec ks;
            if (!m2j::cli::parseKeySpec(gongArg, ks)) {
                std::cerr << "错误: --gong 需要形如 C / F / Bb 的宫音，收到 \""
                          << gongArg << "\"\n";
                return 1;
            }
        }
        else if (a == "--no-pentatonic") { noPentatonic = true; }
        else if (a == "--no-dynamics") { noDynamics = true; }
        else if (a == "--no-soft-pedal") { noSoftPedal = true; }
        else if (a == "--soft-pedal-scale" && i + 1 < argc) {
            double f;
            if (!parseDouble(argv[++i], f) || f < 0.0 || f > 1.0) {
                std::cerr << "错误: --soft-pedal-scale 需要 0~1 的系数（如 0.5）\n"; return 1;
            }
            softPedalScale = f;
        }
        else if (a == "--output" && i + 1 < argc) { txtOut = argv[++i]; }
        else if (a == "--musicxml" && i + 1 < argc) { xmlOut = argv[++i]; }
        else if (a == "--lyrics" && i + 1 < argc) { lyricsPath = argv[++i]; }
        else if (a == "--no-quantize") { quantize = false; }
        else if (a == "--no-midi-lyrics") { noMidiLyrics = true; }
        else if (a == "--no-pickup") { noPickup = true; }
        else if (a == "--player") { player = true; }
        else if (a == "--bar-numbers") { barNumbers = true; }
        else if (a == "--bars" && i + 1 < argc) {
            int n = 0;
            if (!parseInt(argv[++i], n) || n < 1 || n > 8) {
                std::cerr << "错误: --bars 需要 1~8 的整数（每行小节数）\n"; return 1;
            }
            barsPerRow = n;
        }
        else if (a == "--no-key-changes") { noKeyChanges = true; }
        else if (a == "--pickup" && i + 1 < argc) {
            if (!parseBeats(argv[++i], pickupBeats)) {
                std::cerr << "错误: --pickup 需要正数（单位：拍，如 1 / 1.5）\n"; return 1;
            }
        }
        else if (a == "--track" && i + 1 < argc) {
            if (!parseInt(argv[++i], trackFilter)) {
                std::cerr << "错误: --track 需要整数\n"; return 1;
            }
            // 轨号从 0 起；负数是内部「不过滤」的哨兵值，用户写 --track -1
            // 本意绝不会是「把全曲都输出」，静默照做等于骗人。
            if (trackFilter < 0) {
                std::cerr << "错误: --track 需要非负整数（轨号从 0 起，"
                             "用 --list-tracks 查看）\n"; return 1;
            }
        }
        else if (a == "--list-tracks") { listTracks = true; }
        else if (a == "--transpose" && i + 1 < argc) {
            if (!parseInt(argv[++i], transpose)) {
                std::cerr << "错误: --transpose 需要整数（半音数，可为负）\n"; return 1;
            }
            // v1.96.0: 拒绝明显超出合理范围的移调值——此前无上限，
            // --transpose 999 会被静默接受并产出退化谱面。±60 半音（±5 个八度）
            // 已覆盖一切真实移调需求，±12/±24（check_cli_matrix 不变量）仍合法。
            if (transpose < -kMaxTransposeSemis || transpose > kMaxTransposeSemis) {
                std::cerr << "错误: --transpose 超出合理范围（须在 -60~+60 半音，即 ±5 个八度内）\n";
                return 1;
            }
        }
        else if (a.substr(0, 2) == "--") {
            // 未知长参数：保留 v1.77 的「警告但不吞后续合法参数」口径，
            // 只多给一条拼写建议——打错一个字母不该全靠用户自己瞪眼找。
            std::cerr << "警告: 未知参数 " << a;
            const std::string hint = m2j::cli::suggestOption(a);
            if (!hint.empty()) std::cerr << "（是否想用 " << hint << "）";
            std::cerr << "（用 --help 查看用法）\n";
            // 只跳过「看起来像取值」的下一个词。早先无条件 ++i，于是
            // `--foo --html o.html` 会连 --html 一起吃掉，HTML 静默不生成——
            // 打错一个参数名就丢掉一项输出，比不警告还糟。
            if (i + 1 < argc && argv[i + 1][0] != '-') ++i;
        }
        else if (a.size() > 1 && a[0] == '-') {
            // 未知短参数（-x / -q）：旧版落到「位置参数」分支被当成输入文件名，
            // 于是报「无法读取或解析 MIDI 文件: -x」，把参数拼错伪装成文件缺失；
            // 若前面已经给了合法文件，它还会把那个文件静默覆盖掉。
            std::cerr << "错误: 未知参数 " << a << "（用 --help 查看用法）\n";
            return 1;
        }
        else {
            // 位置参数只许有一个。旧版后面那个会静默覆盖前面那个，
            // `a.mid b.mid` 看起来像「两个都转」，其实只转了 b.mid。
            if (!input.empty()) {
                std::cerr << "错误: 只能指定一个输入文件：已有 " << input
                          << "，又给了 " << a << "\n";
                return 1;
            }
            input = a;
        }
    }
    if (input.empty()) { printHelp(); return 1; }
    // 同一带值选项重复给：旧版 last-wins 静默覆盖（--html a --html b 只写 b、
    // --key C --key D 把调性改成 D），全程 rc=0。位置参数早在 v1.81 就只允许
    // 一个，这里把 11 个带值选项补齐同一口径；开关类重复幂等，不在其列。
    // 放在解析循环之后，--help / --version 仍可提前返回。
    {
        const std::vector<std::string> rawArgs(argv + 1, argv + argc);
        const std::string de = m2j::cli::validateDuplicateValueOptions(rawArgs);
        if (!de.empty()) {
            std::cerr << "错误: " << de << "\n";
            return 1;
        }
    }
    // --key 与 --gong 都是「指定原调」的入口，同时给时旧版静默丢掉 --key，
    // 用户以为调性按自己写的来，谱面却是宫音推出来的另一套。
    const std::string ke = m2j::cli::validateKeyOptions(keyArg, gongArg);
    if (!ke.empty()) {
        std::cerr << "错误: " << ke << "\n";
        return 1;
    }
    // --pickup N 与 --no-pickup 语义冲突：旧版静默忽略 --pickup（用户以为
    // 手动指定了弱起，谱面却完全没有），还顺带绕过「弱起须短于小节」的
    // 越界校验。与 --key/--gong 同口径，解析后、读 MIDI 前就明确拒绝。
    const std::string pue = m2j::cli::validatePickupOptions(noPickup, pickupBeats);
    if (!pue.empty()) {
        std::cerr << "错误: " << pue << "\n";
        return 1;
    }
    // 输出路径预检：与输入 .mid 同路径会毁掉源文件，三端互相撞路径
    // 会静默覆盖先写的那份。放在读 MIDI 之前，越界一律不干活。
    {
        const std::vector<m2j::cli::OutputPath> outs = {
            {"--output", txtOut}, {"--html", htmlOut},
            {"--musicxml", xmlOut}
        };
        // --lyrics 指定的外部歌词也是「读进来的源文件」：三端输出写在它上面
        // 会先读后写、把歌词盖掉（v1.82 只保护了输入 MIDI）。
        const std::vector<m2j::cli::OutputPath> sources = {
            {"--lyrics", lyricsPath}
        };
        const std::string oe = m2j::cli::validateOutputPaths(input, outs, sources);
        if (!oe.empty()) {
            std::cerr << "错误: " << oe << "\n";
            return 1;
        }
    }
    // --player 是 HTML 专属：纯文本谱 / MusicXML 没有承载播放器的地方，
    // 静默忽略会让用户以为导出的文件能试听。
    if (barsPerRow > 0 && htmlOut.empty())
        std::cerr << "警告: --bars 只作用于 --html 输出，其它输出会忽略它\n";
    if (player && htmlOut.empty())
        std::cerr << "警告: --player 只作用于 --html 输出，其它输出会忽略它\n";

    m2j::MidiData data;
    // 软踏板系数必须在解析之前落到 MidiData 上：CC67 的力度衰减发生在解析阶段
    // （applyControllerSemantics 直接改 notes.velocity），晚于 load 就来不及了。
    // 与 --track 一样，解析层要拿着这个值去算，不能在末尾再补救。
    if (noSoftPedal) data.softPedalScale = 0.0;
    else if (softPedalScale >= 0.0) data.softPedalScale = softPedalScale;
    // --track 要在 format 2 里挑段落，此时不能提前丢掉别的轨。轨号一路
    // 传进解析层：歌词 / 力度 / 踏板 / 清音 / 弯音事件都要按轨隔离，
    // 只在末尾丢音符的话，谱面时值早被别轨的控制器改过了。
    if (!m2j::loadMidiFile(input, data, trackFilter >= 0, trackFilter)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << input << "\n";
        return 2;
    }
    // --list-tracks 是**整份文件**的诊断，必须排在「--track 选中空轨就报错」
    // 之前：
    // ① 按轨过滤之后别的轨音符已经不在 data 里，列表会把它们全写成「旋律 0 音」
    //    （`--track 2 --list-tracks` 时 Vocal 那条真有的 8 个音显示成 0，同一条
    //    列表里鼓件数却仍是全曲口径，自相矛盾）；
    // ② 选中的轨恰好没有旋律时，旧版直接 exit 4、一行都不列——而那正是用户
    //    最需要看轨道列表的时刻。
    if (listTracks) {
        // 需要「整份文件」的场合：带 --track（其余轨的演奏事件已被丢弃）、
        // 或者 format 2（默认只保留第 0 段，第 1 段连轨名都读不到）。
        if (trackFilter >= 0 || data.format == 2) {
            // keepAllFormat2 = true：format 2 的其它段也要一并列出。
            m2j::MidiData full;
            if (m2j::loadMidiFile(input, full, true, -1)) {
                printTrackList(full, trackFilter);
                return 0;
            }
        }
        printTrackList(data, trackFilter);
        return 0;
    }
    if (trackFilter >= 0 && data.notes.empty()) {
        // 「无音符」和「没有这条轨」是两回事：头部轨数写错的文件里，
        // 用户照着 --list-tracks 的旧数字敲 --track 5，只会得到一句
        // 「轨道 5 无音符」，看不出这条轨根本不存在。
        if (trackFilter >= data.trackCount) {
            std::cerr << "轨道 " << trackFilter << " 不存在：本文件实际只有 "
                      << data.trackCount << " 轨（0~" << (data.trackCount - 1)
                      << "），用 --list-tracks 可查看\n";
        } else {
            // 这条轨确实存在，但没有旋律音符——它里面可能有鼓件音或歌词，
            // 旋律在别的轨上。按轨过滤后别的轨已经不在 data 里，这里重新按
            // 全曲读一次（只走这条出错路径），否则只能说一句干巴巴的「无音符」。
            m2j::MidiData full;
            // keepAllFormat2 = true：format 2 里旋律可能在别的段落，只留第一段
            // 的话这条提示就永远找不到「旋律在轨几」。
            const std::string hint = m2j::loadMidiFile(input, full, true, -1)
                                         ? m2j::emptyTrackHint(full, trackFilter)
                                         : std::string();
            if (hint.empty())
                std::cerr << "轨道 " << trackFilter << " 无音符。\n";
            else
                std::cerr << "轨道 " << trackFilter << " 无音符：" << hint << "。\n";
        }
        return 4;
    }
    // 中途转调（0x59 map）默认生效。关掉时保留事件表本身：曲首调号还要
    // 用来判调性，清空数据会把「忽略转调」和「没有调号」混为一谈。
    if (noKeyChanges) data.applyKeyChanges = false;
    // 五声校正只在没有 0x59 调号、走自动猜调时起作用；关掉后完全回到 KS。
    if (noPentatonic) data.usePentatonic = false;
    // 演奏表情只影响输出层：事件表保留，调性/旋律轨判定不受影响。
    if (noDynamics) data.useDynamics = false;

    // 鼓轨的提示写在谱面头部（formatJianpu 里），这里只管「只剩鼓」这种致命情况
    if (data.notes.empty()) {
        if (data.drumNotesSkipped > 0) {
            std::cerr << "未提取到旋律音符：文件里只有打击乐（简谱不支持鼓谱）。\n";
        } else {
            std::cerr << "未提取到音符。\n";
        }
        return 3;
    }

    // MIDI 内置歌词（0x05 lyric meta）：默认自动加载，--no-midi-lyrics 忽略。
    // 这里直接清空事件表而不是加一个「是否启用」的标志位：歌词事件同时还是
    // 旋律轨判定的证据，关掉就该连证据一起消失，否则会出现「谱面无词、旋律
    // 轨却被自带歌词带偏」这种半开半关的状态。
    const size_t builtinLyricEvents = data.midiLyrics.size();
    if (noMidiLyrics) {
        data.midiLyrics.clear();
        if (builtinLyricEvents > 0)
            std::cout << "已忽略 MIDI 内置歌词: " << builtinLyricEvents << " 个事件\n";
    } else if (builtinLyricEvents > 0) {
        const auto syls = m2j::normalizeMidiLyrics(data);
        const m2j::LyricTrackPick ly = m2j::pickMidiLyricTrack(data);
        if (!syls.empty())
            std::cout << "MIDI 内置歌词: " << syls.size() << " 个音节（轨 " << ly.track << "）"
                      << m2j::droppedLyricNote(data) << "（自动加载，"
                      << "--no-midi-lyrics 可关闭）\n";
        else
            // 有 0x05 事件但一个可唱音节都没有（全是 @KMIDI 这类元数据）：
            // 一声不吭会让用户以为「文件里的词被吞了」。
            std::cout << "MIDI 内置歌词: " << builtinLyricEvents
                      << " 条事件里没有可唱音节（@KMIDI 等元数据与延续符不计）\n";
    }

    // --track 的轨道隔离已在解析层完成（含 format 2 的「实际转换第 N 轨」
    // 改口）。页眉统一放在下面打印，保证提示与实际转换的轨一致。

    // 文件头读法的提示。放在 --track 之后：format 2 的提示要报「实际转了哪一轨」，
    // 提前打印就会和用户手动指定的轨道互相矛盾。
    // SMPTE 文件若不说明，用户看到 division=500 会以为解析错了；
    // format 2 若不说明，用户会怀疑「另几段音乐去哪了」。
    {
        // 损坏 / 截断文件必须明确告知：谱面只按实际读到的字节生成，
        // 用户才不会以为「后面的小节是转换器漏掉的」。
        const std::string tn = m2j::truncationLabel(data);
        if (!tn.empty()) std::cout << tn << "\n";
        const std::string tb = m2j::timeBaseLabel(data);
        if (!tb.empty()) std::cout << tb << "\n";
        const std::string fl = m2j::formatLabel(data);
        if (!fl.empty()) std::cout << fl << "（--track 可从 0 起指定别的段落）\n";
        // 控制器悄悄改过时值 / 力度，不说明用户会以为「谱面和 MIDI 对不上」。
        const std::string cl = m2j::controllerLabel(data);
        if (!cl.empty()) std::cout << cl << "\n";
        // 弯音轮推同样的量，不同通道 / 不同段落可以差出整整一个八度；
        // 不点明用户会以为转谱把音高算错了。
        const std::string br = m2j::bendRangeLabel(data);
        if (!br.empty()) std::cout << br << "\n";
        // 系统事件之后省略状态字节是很多导出器的老习惯；补回多少处要说清，
        // 否则用户不知道这份 MIDI 本来会少掉半首曲子。
        const std::string rs = m2j::runningStatusLabel(data);
        if (!rs.empty()) std::cout << rs << "\n";
        // FF 21 让不同端口复用同一个通道号；不点明会让人以为踏板 / 弯音
        // 串味是转换器的 bug，其实是文件本身把两件乐器放在了不同设备上。
        const std::string pl = m2j::portLabel(data);
        if (!pl.empty()) std::cout << pl << "\n";
        // GS/XG 用 SysEx 指定鼓通道时，被丢掉的鼓件音必须有个交代；
        // 否则用户会以为转换漏掉了半首曲子。
        const std::string sx = m2j::sysExLabel(data);
        if (!sx.empty()) std::cout << sx << "\n";
        // CC0/CC32 Bank Select 指定 XG 鼓组、或 0xC0 中途换音色时，说明
        // 乐器名是按时间轴取的，免得用户以为声部标注前后不一致。
        const std::string bk = m2j::bankLabel(data);
        if (!bk.empty()) std::cout << bk << "\n";
        // 手动选段落时说清「另外几轨的歌词 / 力度 / 踏板 / 弯音也一起隔离了」，
        // 否则用户会奇怪为什么谱面上的力度记号和 DAW 里看到的不一样。
        const std::string tk = m2j::trackLabel(data);
        if (!tk.empty()) std::cout << tk << "\n";
    }

    // --transpose 移调
    // 音符与调号一起平移：estimateKey() 优先采信 MIDI 内置调号，只移音不移
    // 调号会让谱面出现「音符在 D 大调、调号还是 1=C」的分叉（唱名变 2 3 #4、
    // MusicXML 的 <fifths> 留在 0 而满谱临时记号）。中途转调点同样跟着平移。
    bool transposeMovedKey = false;   // 文件自带调号是否真的跟着动了（八度移调不动）
    bool transposeShiftsKey = false;  // 这次移调是否改变调性（非整八度）
    if (transpose != 0) {
        const int sfBefore = data.keySig.valid ? data.keySig.sharpsFlats : 0;
        const int foldedNotes = m2j::transposeData(data, transpose);
        // 移整个八度时调号不动，别谎报「同步平移」。
        transposeMovedKey = data.keySig.valid &&
                            data.keySig.sharpsFlats != sfBefore;
        // 没有 0x59 的文件（调性靠音级分布猜）拿不到「上面这条」的证据，
        // 但它的调性一样要跟着走：只要不是整八度，调性就变了。
        transposeShiftsKey = (transpose % 12) != 0;
        std::cout << "移调: " << (transpose > 0 ? "+" : "") << transpose << " 半音"
                  << (transposeMovedKey ? "（调号同步平移）" : "") << "\n";
        if (foldedNotes > 0)
            std::cout << "移调越界: " << foldedNotes
                      << " 个音移调后越出 MIDI 音域（0~127），按八度折回\n";
    }

    // 时值量化：真实录音 MIDI 的 note-off 普遍提前，不量化会把八分判成附点十六
    if (quantize) {
        m2j::QuantizeReport qr = m2j::quantizeTiming(data);
        if (qr.adjusted > 0) {
            std::cout << "时值量化: 修正 " << qr.adjusted << " 个音符 (网格 "
                      << qr.gridTicks << " tick, 门限 " << qr.gate;
            if (qr.laneGate < qr.gate - 0.02)
                std::cout << ", 本声部 " << qr.laneGate;
            std::cout << ")\n";
        }
    }

    // 弱起：--pickup N 给的是「拍」，换算成 tick 写回数据；
    // --no-pickup 则明确关掉自动判定。两者都不给时由 detectPickupTicks 判。
    if (noPickup) {
        data.pickupTicks = 0;
    } else if (pickupBeats >= 0) {
        // 越界的弱起（等于或超过整小节）不能悄悄接受：barGridOf 会把它钳成
        // 0（谱面完全没有弱起），命令行却照样报「弱起 N 拍」，两处口径相反。
        // 早先的表现是一行残缺的「（手动指定）」——前半句为空、只剩后缀。
        const std::string pe = m2j::pickupArgError(pickupBeats, data.timeSig);
        if (!pe.empty()) {
            std::cerr << "错误: " << pe << "\n"; return 1;
        }
        double beatLen = data.division * 4.0 / (double)(1 << data.timeSig.denomPow);
        if (!(beatLen > 0)) beatLen = data.division;
        data.pickupTicks = (long)std::llround(pickupBeats * beatLen);
        // tick 换算后再兜一次：拍数合法但四舍五入顶满小节时，同样会被
        // barGridOf 钳成 0，这里提前报错，不让「提示有弱起、谱面无弱起」发生。
        const long barLen = (long)std::llround(
            data.timeSig.barLengthTicks(data.division));
        if (barLen > 0 && data.pickupTicks >= barLen) {
            std::cerr << "错误: 弱起拍数换算成 tick 后顶满整小节"
                         "（弱起必须短于小节）\n";
            return 1;
        }
    }
    {
        const long pk = m2j::effectivePickupTicks(data);
        const std::string label = pk > 0 ? m2j::pickupLabel(data) : std::string();
        // 标签为空就一个字都不打：否则会剩下一行只有「（手动指定）」的空壳。
        if (!label.empty()) {
            std::cout << label
                      << (pickupBeats >= 0 ? "（手动指定）" : "（自动判定，如不对用 --pickup / --no-pickup 覆盖）")
                      << "\n";
        }
    }

    m2j::KeyEstimate key;
    std::string keySource;    if (!gongArg.empty()) {
        // --gong 优先于 --key：它同时决定「1 落在哪个音」和调式名。
        m2j::KeyEstimate g;
        if (parseKey(gongArg, g)) {
            // --gong 给的是「原调宫音」：宫音必须跟着移调走，否则 1=C 的
            // C 宫五声移两个半音后仍然标成 C 宫，与谱面音高脱节。直接挪
            // 宫音再重推一次，调式名（徵 / 羽…）才跟着新终止音算对。
            int gpc = g.tonicPitchClass;
            int gHint = g.fifthsHint;
            if (transposeShiftsKey) {
                gpc = ((gpc + transpose) % 12 + 12) % 12;
                // 宫音的升降号侧也要跟着移：Gb 宫升两个半音是 Ab 宫（-4）。
                if (gHint != m2j::kFifthsAuto)
                    gHint = m2j::transposeFifths(gHint, transpose);
            }
            key = m2j::keyFromGong(data, gpc, gHint);
            keySource = "(手动指定宫音 " + gongArg
                      + (transposeShiftsKey ? "，已随移调平移" : "") + ")";
            std::cout << "五声调式: " << key.scaleLabel
                      << "，首调 1=" << m2j::keyName(key) << "\n";
        } else {
            // 参数循环里已严格校验过，这里只作兜底：绝不再降级成自动识别。
            std::cerr << "错误: 无法识别的宫音 \"" << gongArg << "\"\n";
            return 1;
        }
    } else if (!keyArg.empty() && parseKey(keyArg, key)) {
        // --key 同理：它描述的是原调，移调后要一起走，否则用户在 README 里
        // 抄的 `--key C --transpose +2` 会得到「1=C 的 D 大调旋律」。
        if (transposeShiftsKey) key = m2j::transposeKey(key, transpose);
        keySource = "(手动指定 " + keyArg
                  + (transposeShiftsKey ? "，已随移调平移" : "") + ")";
    } else {
        if (!keyArg.empty()) {
            // 同上：严格校验已在参数循环里做过，留着防止将来有调用方绕过。
            std::cerr << "错误: 无法识别的调性 \"" << keyArg << "\"\n";
            return 1;
        }
        key = m2j::estimateKey(data);
        keySource = data.keySig.valid ? "(MIDI调号)" : "(自动猜测)";
    }
    // 调号已经跟着 --transpose 平移过，页眉不能再写「MIDI调号」——那会让用户
    // 以为文件里本来就写着这个调。移到这里的标签统一在下面点明。
    if (transposeMovedKey && keySource == "(MIDI调号)")
        keySource = "(MIDI调号，已随移调平移)";
    if (!key.scaleLabel.empty() && gongArg.empty())
        std::cout << "五声调式: " << key.scaleLabel
                  << "，首调 1=" << m2j::keyName(key)
                  << "（--no-pentatonic 可关闭自动校正）\n";

    const m2j::ExpressionMap expr(data);
    if (!expr.summary().empty())
        std::cout << "演奏表情: " << expr.summary()
                  << "（--no-dynamics 可关闭）\n";

    // 声部 / 乐器标注：列出每条含音符的声部及其 GM 乐器名，帮助用户确认
    // 旋律/伴奏轨判定是否正确（例如某轨被误当成旋律时一眼能看出）。
    const std::string psum = m2j::partSummary(data);
    if (!psum.empty())
        std::cout << psum << "\n";

    // 歌词只解析一次，HTML 谱面和 MusicXML 共用同一份 LRC 行。
    // 早先解析写在 --html 分支里，导 MusicXML 时整份歌词都丢了。
    std::vector<m2j::LyricLine> lyricLines;
    const std::vector<m2j::LyricLine>* lyricPtr = nullptr;
    std::string lyricRaw;
    if (!lyricsPath.empty()) {
        // 路径写错必须当场报错。旧实现直接去读，读不到就是 0 行，然后
        // 「歌词已加载: 0 行」+ 退出码 0——用户完全不知道被告知的那个文件
        // 根本没打开过，导出的谱子静默地没有歌词。
        std::ifstream probe(lyricsPath.c_str(), std::ios::binary);
        if (!probe) {
            std::cerr << "错误: 无法打开歌词文件 " << lyricsPath << "\n"; return 1;
        }
        // 顺带把字节读进来：下面要按「扩展名 + 内容」决定走哪套解析器，
        // 再交给纯文本入口。同一份文件读两遍、两处口径，是上一种格式
        // 判定错乱的温床。
        lyricRaw.assign((std::istreambuf_iterator<char>(probe)),
                        std::istreambuf_iterator<char>());
        probe.close();
        if (htmlOut.empty() && xmlOut.empty()) {
            // 纯文本谱没有挂歌词的位置，说了比不说好，否则用户会以为歌词丢了。
            std::cerr << "警告: --lyrics 只作用于 --html / --musicxml 输出，"
                         "文本谱会忽略它\n";
        }
    }
    if (!lyricsPath.empty() && (!htmlOut.empty() || !xmlOut.empty())) {
        // 字节归一与两个读盘入口同口径，之后只看纯文本（v1.110.0）。
        const std::string lyricText = m2j::normalizeLyricFileBytes(lyricRaw);
        const m2j::LyricFormat lyricFmt =
            m2j::detectLyricFormat(lyricsPath, lyricText);
        const bool isLrc = (lyricFmt == m2j::LyricFormat::Lrc);
        size_t droppedLyricLines = 0;
        if (isLrc) {
            // LRC 时间戳是真实秒，必须按 tempo map 分段反解成 tick；
            // 用曲首 BPM 一把梭会让变速后的歌词整体错位。
            lyricLines = m2j::parseLrcText(lyricText, data, &droppedLyricLines);
        } else {
            // JSON 格式：每个字自成一行
            auto syls = m2j::parseLyricsText(lyricText, data.division,
                                             &droppedLyricLines);
            for (const auto& s : syls) lyricLines.push_back({s.tick, s.text});
        }
        // 离谱的时间戳 / [offset:] / start_beat 换算成 32 位 tick 会溢出，
        // 旧版把环绕后的假 tick 当真，歌词被静默挂到曲首。现在显式丢弃并
        // 告诉用户，而不是让控制台继续宣称「歌词已加载: N 行」。
        if (droppedLyricLines)
            std::cerr << "警告: 已忽略 " << droppedLyricLines
                      << " 行歌词（时间戳 / [offset:] / start_beat 数值"
                         "超出谱面时间轴范围，或 JSON 歌词行格式错误）\n";
        std::cout << "歌词已加载: " << lyricLines.size() << " 行\n";
        // 文件读到了却一行都没解析出来，通常只有两种：LRC 的时间戳不是
        // [mm:ss.xx] 形状，或者 JSON 根本不是 [{start_beat,text}] 数组。
        // 提示必须按实际用过的解析器说——早先不管走哪条路都说「检查是否以
        // [mm:ss.xx] 时间戳开头」，把 JSON 用户指去改一个不存在的字段。
        if (lyricLines.empty()) {
            std::cerr << "警告: 歌词文件里没有解析出任何歌词行（按 "
                      << (isLrc ? "LRC" : "JSON") << " 解析 "
                      << lyricsPath << "）\n";
            if (isLrc)
                std::cerr << "      检查每行是否以 [mm:ss.xx] 时间戳开头\n";
            else
                std::cerr << "      检查是否为 [{\"start_beat\":N,"
                             "\"text\":\"...\"}] 数组\n";
            // 扩展名与内容写反是另一类常见错误（.lrc 里放着 JSON，或反过来）。
            // 空手而归时顺手试一下另一种格式：能救回来的就直接说清楚，别让
            // 用户对着「0 行」猜该改哪里。
            size_t altDropped = 0;
            const size_t altCount = isLrc
                ? m2j::parseLyricsText(lyricText, data.division, &altDropped).size()
                : m2j::parseLrcText(lyricText, data, &altDropped).size();
            if (altCount)
                std::cerr << "      内容看起来是 " << (isLrc ? "JSON" : "LRC")
                          << "（按另一种格式能解析出 " << altCount
                          << " 行），检查文件扩展名是否写反\n";
        }
        if (isLrc) {
            const int tc = m2j::tempoChangeCount(data);
            if (tc > 0)
                std::cout << "LRC 时间轴: 检测到 " << tc
                          << " 处变速，歌词秒数已按 tempo map 分段换算\n";
        }
        if (!lyricLines.empty()) {
            lyricPtr = &lyricLines;
            if (!data.midiLyrics.empty())
                std::cout << "外部歌词优先，MIDI 内置歌词已让位\n";
        }
    }

    // 歌词落位体检（v1.113.0）：铺排之后到底有几个字真的挂上了音头。
    // 旧版到这里就只剩「歌词已加载: N 行」——一行十来个字配八音旋律时，
    // placeLyricLine 会把多出来的字直接 break 掉；同戳多行均分后一行一个
    // 音头都分不到时整行 continue。两种情况都不崩、不报错，谱面上少了半句，
    // 控制台却一切正常，用户没法判断是文件坏了还是程序漏了。
    // 这里用与输出层完全相同的两个纯函数（pickMelodyTrack + resolveLyrics）
    // 算一遍，参数一致所以结果必然一致；只是多一次 O(n log n) 的铺排，相比
    // 整份谱面的格式化可以忽略。
    if ((!htmlOut.empty() || !xmlOut.empty())) {
        const int lyricTrack = m2j::pickMelodyTrack(data, lyricPtr);
        if (lyricTrack >= 0) {
            const m2j::LyricAssignment placed =
                m2j::resolveLyrics(data, lyricPtr, lyricTrack);
            if (placed.charsSkipped() > 0)
                std::cerr << "警告: " << placed.charsSkipped()
                          << " 个歌词字没有音头可挂（共 " << placed.charsTotal
                          << " 字，挂上 " << placed.charsPlaced
                          << " 字）；这一段的字比旋律音多，多出来的字会缺席\n";
            if (placed.linesEmpty > 0)
                std::cerr << "警告: " << placed.linesEmpty
                          << " 行歌词整行没挂上（同一时刻的行数多于该处的音头数，"
                             "或整行落在全曲最后一个音之后）\n";
        }
    }

    std::string jianpu = m2j::formatJianpu(data, key, barNumbers);
    const std::string meter = m2j::meterSummary(data);
    std::string header = "\n共提取 " + std::to_string(data.notes.size())
              + " 个音符 (division=" + std::to_string(data.division)
              + ", 拍号=" + meter + ") " + keySource + "\n\n";
    std::cout << header << jianpu;
    const auto meterChanges = m2j::meterChangesOf(data);
    if (!meterChanges.empty())
        std::cout << "\n拍号变化 " << meterChanges.size()
                  << " 处，小节线已按各段拍号分别划分\n";
    const auto keyChanges = m2j::keyChangesOf(data);
    if (!keyChanges.empty())
        std::cout << "\n转调 " << keyChanges.size()
                  << " 处，唱名已按各段调号分别换算（--no-key-changes 可忽略）\n";

    if (!txtOut.empty()) {
        if (!writeOutputFile(txtOut, header + jianpu, "文本简谱")) return 1;
        std::cout << "文本简谱已写入: " << txtOut << "\n";
    }
    if (!htmlOut.empty()) {
        std::string title = input;
        size_t slash = title.find_last_of("/\\");
        if (slash != std::string::npos) title = title.substr(slash + 1);
        size_t dot = title.find_last_of(".");
        if (dot != std::string::npos) title = title.substr(0, dot);
        if (!writeOutputFile(htmlOut,
                            m2j::renderHtml(data, key, title, lyricPtr,
                                            player, barsPerRow),
                            "HTML 谱面")) return 1;
        std::cout << "HTML 谱面已写入: " << htmlOut << "\n";
    }
    if (!xmlOut.empty()) {
        if (!writeOutputFile(xmlOut,
                            m2j::renderMusicXml(data, key, lyricPtr),
                            "MusicXML")) return 1;
        std::cout << "MusicXML 已写入: " << xmlOut << "\n";
    }
    return 0;
}

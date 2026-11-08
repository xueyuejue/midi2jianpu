// main.cpp - 命令行入口
#include "midi2jianpu/midifile.hpp"
#include "midi2jianpu/notation.hpp"
#include "midi2jianpu/html_writer.hpp"
#include "midi2jianpu/musicxml_writer.hpp"
#include "midi2jianpu/version.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

static void printHelp() {
    std::cout <<
        "midi2jianpu " M2J_VERSION " - MIDI 文件转中文简谱\n"
        "用法:\n"
        "  midi2jianpu <input.mid>                 打印文本简谱\n"
        "  midi2jianpu <input.mid> --html out.html 导出 HTML 谱面\n"
        "  midi2jianpu <input.mid> --musicxml out.xml  导出 MusicXML(可用 MuseScore 打开)\n"
        "  midi2jianpu <input.mid> --key Am         手动指定调性(如 Am=小调, C=大调)\n"
        "  midi2jianpu <input.mid> --output out.txt  文本简谱输出到文件\n"
        "  midi2jianpu <input.mid> --track 0         只输出指定轨道\n"
        "  midi2jianpu <input.mid> --list-tracks     列出所有轨道（选 --track 前先看清）\n"
        "  midi2jianpu <input.mid> --transpose +2     移调(半音, 正负都可)\n"
        "  midi2jianpu <input.mid> --lyrics lyrics.lrc  加载歌词(优先于 MIDI 内置歌词)\n"
        "  midi2jianpu <input.mid> --no-midi-lyrics  忽略 MIDI 文件自带的歌词\n"
        "  midi2jianpu <input.mid> --pickup 1       手动指定弱起小节长度(拍)\n"
        "  midi2jianpu <input.mid> --no-pickup      关闭弱起自动判定\n"
        "  midi2jianpu <input.mid> --no-key-changes 忽略中途转调(全曲按一个调记谱)\n"
        "  midi2jianpu <input.mid> --gong C        手动指定五声调式的宫音(简谱 1)\n"
        "  midi2jianpu <input.mid> --no-pentatonic  关闭五声调式宫音自动校正\n"
        "  midi2jianpu <input.mid> --no-dynamics   不写力度/断音/滑音/段落等演奏记号\n"
        "  midi2jianpu <input.mid> --no-quantize    关闭时值量化(保留 MIDI 原始时长)\n"
        "  midi2jianpu --version\n"
        "  midi2jianpu --help\n";
}

// 解析 --key 参数: "C"=C大调, "Am"=a小调, "F#"=F#大调, "Bb"=降B大调
static bool parseKey(const std::string& s, m2j::KeyEstimate& out) {
    static const char* names[]     = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* flatNames[] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    if (s.empty()) return false;
    size_t i = 0;
    std::string name;
    if (s.size() >= 2 && (s[1] == '#' || s[1] == 'b')) { name = s.substr(0, 2); i = 2; }
    else { name = s.substr(0, 1); i = 1; }
    if (name[0] >= 'a' && name[0] <= 'z') name[0] = char(name[0] - 'a' + 'A');

    int pc = -1;
    for (int k = 0; k < 12; k++) if (names[k] == name) { pc = k; break; }
    if (pc < 0) for (int k = 0; k < 12; k++) if (flatNames[k] == name) { pc = k; break; }
    if (pc < 0) return false;

    out.tonicPitchClass = pc;
    out.isMajor = !(i < s.size() && (s[i] == 'm' || s[i] == 'M'));
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
    try {
        size_t used = 0;
        std::string str(s);
        double v = std::stod(str, &used);
        if (used != str.size() || v < 0) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// --list-tracks：把文件里的每条轨列出来（序号 / 名称 / 旋律音数 / 乐器 /
// 歌词字数 / 是否鼓轨），让用户在 `--track N` 之前先看清该选哪条。纯诊断，
// 不参与转换，因此不改动任何既有转换输出，零回归。
static void printTrackList(const m2j::MidiData& data) {
    struct TrackStat {
        int notes = 0;
        int lyrics = 0;
    };
    std::map<int, TrackStat> st;
    for (const auto& n : data.notes) st[n.track].notes++;
    for (const auto& ly : data.midiLyrics)
        if (ly.track >= 0) st[ly.track].lyrics++;

    // 乐器名必须和谱面页眉同源：直接用 partPrograms() 的按轨音色时间轴表。
    // 旧实现在这里读 channelProgram（只留最后一个 0xC0 音色号），中途换
    // 乐器的曲子会报「小提琴」而谱面写「钢琴」，用户照诊断选轨反而被误导。
    const auto progs = m2j::partPrograms(data);
    auto instOf = [&](int track) -> std::string {
        auto it = progs.find(track);
        if (it == progs.end() || it->second < 0) return "";
        return m2j::gmInstrumentName(it->second);
    };

    std::cout << "轨道列表 (format " << data.format << ", 共 " << data.trackCount << " 轨):\n";
    for (int t = 0; t < data.trackCount; t++) {
        const auto& s = st[t];
        std::string name;
        auto nit = data.trackNames.find(t);
        if (nit != data.trackNames.end()) name = nit->second;
        int drum = 0;
        auto dit = data.drumNotesByTrack.find(t);
        if (dit != data.drumNotesByTrack.end()) drum = dit->second;
        std::string inst = instOf(t);

        std::cout << "  #" << t;
        if (!name.empty()) std::cout << "  \"" << name << "\"";
        if (drum > 0 && s.notes == 0)
            std::cout << "   鼓轨 (跳过 " << drum << " 鼓件音)";
        else if (drum > 0)
            std::cout << "   旋律 " << s.notes << " 音, 鼓件音 " << drum;
        else
            std::cout << "   旋律 " << s.notes << " 音";
        if (!inst.empty()) std::cout << "   乐器: " << inst;
        if (s.lyrics > 0) std::cout << "   歌词 " << s.lyrics << " 字";
        std::cout << "\n";
    }
    std::cout << "默认: 转换全部轨道（多轨各自成行）；--track N 只取第 N 轨（从 0 起）\n";
    if (data.format == 2)
        std::cout << "注意: format 2 各轨是独立序列，默认只转第 0 轨；--track N 可指定别的段\n";
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
    double pickupBeats = -1;   // <0 表示没指定（走自动判定）
    bool noPickup = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--version" || a == "-v") {
            std::cout << "midi2jianpu " M2J_VERSION "\n";
            return 0;
        }
        else if (a == "--html" && i + 1 < argc) { htmlOut = argv[++i]; }
        else if (a == "--key" && i + 1 < argc) { keyArg = argv[++i]; }
        else if (a == "--gong" && i + 1 < argc) { gongArg = argv[++i]; }
        else if (a == "--no-pentatonic") { noPentatonic = true; }
        else if (a == "--no-dynamics") { noDynamics = true; }
        else if (a == "--output" && i + 1 < argc) { txtOut = argv[++i]; }
        else if (a == "--musicxml" && i + 1 < argc) { xmlOut = argv[++i]; }
        else if (a == "--lyrics" && i + 1 < argc) { lyricsPath = argv[++i]; }
        else if (a == "--no-quantize") { quantize = false; }
        else if (a == "--no-midi-lyrics") { noMidiLyrics = true; }
        else if (a == "--no-pickup") { noPickup = true; }
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
        }
        else if (a == "--list-tracks") { listTracks = true; }
        else if (a == "--transpose" && i + 1 < argc) {
            if (!parseInt(argv[++i], transpose)) {
                std::cerr << "错误: --transpose 需要整数（半音数，可为负）\n"; return 1;
            }
        }
        else if (a.substr(0, 2) != "--") input = a;
        else {
            std::cerr << "警告: 未知参数 " << a << "（用 --help 查看用法）\n";
            ++i;  // 跳过其可能的取值
        }
    }
    if (input.empty()) { printHelp(); return 1; }

    m2j::MidiData data;
    // --track 要在 format 2 里挑段落，此时不能提前丢掉别的轨。轨号一路
    // 传进解析层：歌词 / 力度 / 踏板 / 清音 / 弯音事件都要按轨隔离，
    // 只在末尾丢音符的话，谱面时值早被别轨的控制器改过了。
    if (!m2j::loadMidiFile(input, data, trackFilter >= 0, trackFilter)) {
        std::cerr << "错误: 无法读取或解析 MIDI 文件: " << input << "\n";
        return 2;
    }
    if (trackFilter >= 0 && data.notes.empty()) {
        std::cerr << "轨道 " << trackFilter << " 无音符。\n";
        return 4;
    }
    // --list-tracks 是纯诊断：列完轨道就退出，不跑后面的转换流程。
    if (listTracks) {
        printTrackList(data);
        return 0;
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
        if (!syls.empty())
            std::cout << "MIDI 内置歌词: " << syls.size() << " 个音节"
                      << m2j::droppedLyricNote(data) << "（自动加载，"
                      << "--no-midi-lyrics 可关闭）\n";
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
    if (transpose != 0) {
        for (auto& n : data.notes) n.pitch += transpose;
        std::cout << "移调: " << (transpose > 0 ? "+" : "") << transpose << " 半音\n";
    }

    // 时值量化：真实录音 MIDI 的 note-off 普遍提前，不量化会把八分判成附点十六
    if (quantize) {
        m2j::QuantizeReport qr = m2j::quantizeTiming(data);
        if (qr.adjusted > 0) {
            std::cout << "时值量化: 修正 " << qr.adjusted << " 个音符 (网格 "
                      << qr.gridTicks << " tick, 门限 " << qr.gate << ")\n";
        }
    }

    // 弱起：--pickup N 给的是「拍」，换算成 tick 写回数据；
    // --no-pickup 则明确关掉自动判定。两者都不给时由 detectPickupTicks 判。
    if (noPickup) {
        data.pickupTicks = 0;
    } else if (pickupBeats >= 0) {
        double beatLen = data.division * 4.0 / (double)(1 << data.timeSig.denomPow);
        if (!(beatLen > 0)) beatLen = data.division;
        data.pickupTicks = (long)std::llround(pickupBeats * beatLen);
    }
    {
        long pk = m2j::effectivePickupTicks(data);
        if (pk > 0) {
            std::cout << m2j::pickupLabel(data)
                      << (pickupBeats >= 0 ? "（手动指定）" : "（自动判定，如不对用 --pickup / --no-pickup 覆盖）")
                      << "\n";
        }
    }

    m2j::KeyEstimate key;
    std::string keySource;    if (!gongArg.empty()) {
        // --gong 优先于 --key：它同时决定「1 落在哪个音」和调式名。
        m2j::KeyEstimate g;
        if (parseKey(gongArg, g)) {
            key = m2j::keyFromGong(data, g.tonicPitchClass);
            keySource = "(手动指定宫音 " + gongArg + ")";
            std::cout << "五声调式: " << key.scaleLabel
                      << "，首调 1=" << m2j::keyName(key) << "\n";
        } else {
            std::cerr << "警告: 无法识别的宫音 \"" << gongArg
                      << "\"，已改用自动识别（示例: C / F / Bb）\n";
            key = m2j::estimateKey(data);
            keySource = data.keySig.valid ? "(MIDI调号)" : "(自动猜测)";
        }
    } else if (!keyArg.empty() && parseKey(keyArg, key)) {
        keySource = "(手动指定 " + keyArg + ")";
    } else {
        if (!keyArg.empty()) {
            std::cerr << "警告: 无法识别的调性 \"" << keyArg
                      << "\"，已改用自动识别（示例: C / Am / F# / Bb）\n";
        }
        key = m2j::estimateKey(data);
        keySource = data.keySig.valid ? "(MIDI调号)" : "(自动猜测)";
    }
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
    if (!lyricsPath.empty() && (!htmlOut.empty() || !xmlOut.empty())) {
        size_t dot = lyricsPath.find_last_of(".");
        std::string ext = (dot != std::string::npos) ? lyricsPath.substr(dot) : "";
        if (ext == ".lrc" || ext == ".LRC") {
            // LRC 时间戳是真实秒，必须按 tempo map 分段反解成 tick；
            // 用曲首 BPM 一把梭会让变速后的歌词整体错位。
            lyricLines = m2j::parseLrcLines(lyricsPath, data);
        } else {
            // JSON 格式：每个字自成一行
            auto syls = m2j::parseLyrics(lyricsPath, data.division);
            for (const auto& s : syls) lyricLines.push_back({s.tick, s.text});
        }
        std::cout << "歌词已加载: " << lyricLines.size() << " 行\n";
        if (ext == ".lrc" || ext == ".LRC") {
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

    std::string jianpu = m2j::formatJianpu(data, key);
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
        std::ofstream f(txtOut);
        f << header << jianpu;
        std::cout << "文本简谱已写入: " << txtOut << "\n";
    }
    if (!htmlOut.empty()) {
        std::ofstream f(htmlOut);
        std::string title = input;
        size_t slash = title.find_last_of("/\\");
        if (slash != std::string::npos) title = title.substr(slash + 1);
        size_t dot = title.find_last_of(".");
        if (dot != std::string::npos) title = title.substr(0, dot);
        f << m2j::renderHtml(data, key, title, lyricPtr);
        std::cout << "HTML 谱面已写入: " << htmlOut << "\n";
    }
    if (!xmlOut.empty()) {
        std::ofstream f(xmlOut);
        f << m2j::renderMusicXml(data, key, lyricPtr);
        std::cout << "MusicXML 已写入: " << xmlOut << "\n";
    }
    return 0;
}

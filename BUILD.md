# 构建与运行

项目为**纯 C++17、零第三方依赖**，只有一个 CMake 工程，产出命令行程序、单元测试与（可选的）WebAssembly 模块。

## 依赖

| 项目 | 要求 |
| --- | --- |
| 编译器 | MSVC 2019+ / GCC 9+ / Clang 10+（需 C++17） |
| CMake | 3.14 及以上 |
| Emscripten | 仅在编译网页版 WASM 时需要 |

无第三方库：SMF 解析、调性识别、和弦检测、HTML/MusicXML 生成全部手写在 `src/` 里。

## 命令行版

```bash
cmake -B build -S .
cmake --build build --config Release
```

产物位置：

- Linux / macOS：`build/midi2jianpu`
- Windows（Visual Studio 生成器，多配置）：`build/Release/midi2jianpu.exe`
- Windows（Ninja / NMake，单配置）：`build/midi2jianpu.exe`

```bash
./build/midi2jianpu 你的歌曲.mid
```

> **中文乱码提示**：源码含中文注释与中文字面量。CMake 已为 MSVC 自动加上 `/utf-8`，若你手动调用 `cl.exe` 编译，请务必自行加 `/utf-8`，否则帮助信息与谱面标题会变乱码。

## 测试

### 单元测试（384 项）

```bash
cd build && ctest --output-on-failure
# 或直接跑
./build/test_basic
```

成功时输出 `共 384 项，失败 0 项 -> 全部通过`。

覆盖范围：调性识别、各调八度基准、升降号书写（含降号调）、时值后缀、三连音、同音合并、和弦检测、移调、MusicXML 小节时长与 `<chord/>`，以及十五组解析层回归——**鼓轨过滤**、**旋律轨判定**、**变速 tempo map 秒数换算**、**弱起小节网格**、**歌词分配与一字多音区间**、**MIDI 内置歌词规整与精确对齐**、**拍号 map 与逐小节网格**、**调号 map（中途转调 / KeyMap）**、**五声调式宫音判定**、**TempoTimeline 与 LRC 变速对齐（tick/秒往返、乱序去重、offset、多时间戳、旧签名对照）**、**ExpressionMap 演奏表情（力度六档 / 渐变 / 断奏 / 重音 / 滑音 / 颤音 / 段落 / 通道隔离 / 开关 / 节奏混合时不误判）与解析层 velocity、原始时值、多轨事件排序**、**GM 音色名（英文全名 / 中文译名覆盖）、`0xC0` 解析（通道 → GM 音色号）、声部乐器标注 `listParts` / `partSummary`、倚音检测与通道隔离、无 0xC0 / 等间隔流零标注回归**、**和弦转位（低音非根音才写斜杠 / 和弦外音不写）、`Bm7b5` 与 `G7sus4` 的类型扩充、`ChordMark` 的结构化字段（rootPc / kind / kindText / bassPc）**、**速度曲线折叠（`tempoMarksOf`：渐变 / 阶跃 / 抖动退化 / 5% 跨度阈值 / 一拍聚簇阈值 / 曲首独立阶跃 / 页眉文案）**、**长音跨小节（6 拍 / 10 拍时值不被截断、延音段写成 `-` 而非休止符、复调轨 4 拍和弦不被门限放大、断奏轨门限还原仍生效）**（都在内存里现构造最小 SMF，不依赖 fixture 文件）。

### 冒烟测试（端到端，验证 README 承诺的每个参数）

需要 Python 3（只用标准库）：

```bash
python tests/fixtures/make_fixture.py     # 生成 16 份 .mid 素材（含 tempolrc / expression / parts / chords / tempo_ramp / long_tie）+ 3 份变速 LRC
python tests/smoke.py build/midi2jianpu   # Windows 换成 build\Release\midi2jianpu.exe
```

覆盖 `--key` / `--track` / `--transpose` / `--musicxml` / `--html` / `--lyrics` / `--no-midi-lyrics` / `--pickup` / `--no-pickup` / `--no-key-changes` / `--gong` / `--no-pentatonic` / `--no-dynamics` / `--output` / `--help` / `--version` 全部参数，并校验导出的 XML 里 `duration` 为整数、和弦音带 `<chord/>`，带歌词时 `<lyric>` / `<slur>` 成对且 `<notations>` 排在 `<lyric>` 之前，变拍号时只有变化小节重写 `<time>`、转调时只在变化小节重写 `<key>`（每个声部 `0 / 2 / -1` 三个 `<fifths>`）、五声调式时加一条 `<words>` 调式标记。

其中 `drums.mid` 专测打击乐轨：鼓轨有 192 个音符（远超旋律的 8 个），谱面里不能出现 `[Drums]`、不能出现鼓的音高，旋律轨必须判给单声部的 Vocal。

`melisma.mid` 专测一字多音：一行 LRC 只有 4 个字，旋律却在两小节里唱 8 个音。四个字必须分到两小节（早先顺序分配会把它们全塞进第一小节，第二小节看着像漏词）；每个字还要在 HTML 里对应一条 `class='sgrp'` 的弧线、在 MusicXML 里对应一对 `begin`/`end` 与成对的 `<slur>`。

`karaoke.mid` 专测 MIDI 内置歌词（`0x05`）：事件流里混着 `@KMIDI` 元数据、`-`/`_` 延续符、`/` 换行、`+` 续行前缀、中文标点和英文单词 `Hello `，规整后应得到 5 个音节（你好世界Hello）、3 组一字多音；不带 `--lyrics` 也要在 HTML 和 MusicXML 里出词，`--no-midi-lyrics` 则完全无词，显式 `--lyrics` 时以 LRC 为准。伴奏是柱式和弦，顺便验证「歌词所在轨」能把旋律轨判对。

`harmony.mid` 专测和声粒度与变速：钢琴一小节换两次和弦（`| C Am | F G |`），conductor 轨在第 5 小节把速度从 120 改成 90。断言要求同一小节里同时出现 `[C]` 与 `[Am]`（小节起点和弦在标记行、第三拍和弦浮在音符上方各算一个），且速度标记只印在第一行谱上（两轨各印一遍会出 4 个而不是 2 个）。

减时线连通也在这一组里校验：三连音必须是「一条线 + 一个 3」，不能被时值表最近的 0.375 档误判成附点十六分（两条线加一个点）。

`meter.mid` 是**混合压力素材**，用来回答「分开测都过、混在一起会不会互相带偏」：4/4 ×2 → 3/4 ×2 → 6/8 ×2，同时在第 3 小节把 120 改成 90 BPM，伴奏轨重复声明同样的 `0x58`，通道 10 放 6 个鼓点，Vocal 轨每个音头写一个 `0x05` 歌词事件（其中 2 处 `-` 延续）。断言覆盖：曲首拍号是 4/4 而不是 6/8、页眉摘要、两个拍号标签、6 个小节时长为 `1920/1920/1440/1440/1440/1440`、MusicXML 只在第 1/3/5 小节写 `<time>`、24 个歌词字与 2 对 slur、鼓轨被丢弃、且**没有**误判出弱起。

`modulation.mid` 是**转调混合压力素材**：12 小节 4/4，第 1/5/9 小节依次把 `0x59` 调号写成 `sf=0 / +2 / -1`（`1=C → 1=D → 1=F`），第 5 小节同时把 120 降成 90 BPM，伴奏轨在 tick 0 和第 5 小节重复声明调号，通道 10 放 12 个鼓点，Vocal 轨每个音头写一个 `0x05` 歌词事件（2 处 `-` 延续）。**三段旋律每小节第一个音都是同一个 C4=60**，所以断言可以直接锁住「三段分别记作 `1` / `#6,` / `5,`」——唱名不随调变就等于功能没生效。其余断言覆盖页眉转调链、`[转1=D]` / `转 1=D` 标记、`--key G` 时整条链平移成 `1=G → 1=A → 1=C`、`--no-key-changes` 时全曲一个调、每声部在第 1/5/9 小节写 `<fifths>` 为 `0/2/-1`、以及降号调里 Bb 和弦不写成 A#。

`pentatonic.mid` 是**五声调式对照素材**：8 小节全用 C 宫五声 `C D E G A`，终止音落在 G，且文件**故意不写 `0x59`**。KS 会把它判成 G 大调（`1=G`），校正后必须是 `1=C 大调 · G 徵调式（宫音 C）`，第一小节记作 `1 2 3 5`；`--no-pentatonic` 回到 `1=G`，`--gong F` 强制 `1=F`。另有两个对照轨分别塞进一个偏音（六声，仍应检出）和两个偏音（七声，必须交回 KS）。

`tempolrc.mid` 是**变速歌词素材**：8 小节 4/4，前 4 小节 120 BPM、第 5 小节起 60 BPM，每小节 4 个四分音符。`tempolrc.lrc` 用真实秒写 8 行词（0 / 2 / 4 / 6 / 8 / 12 / 16 / 20 秒），正好落在各小节起点；逐小节校验 MusicXML 的 `<lyric>` 能同时证明「前半段没变、后半段没跑偏」。旧解析器会把 12 秒算成 tick 11520（差一整小节），16 / 20 秒更是超出曲终导致歌词消失。`tempolrc_offset.lrc` 把每个时间戳 +2 秒再用 `[offset:-2000]` 拉回，断言与主 LRC 完全等价；`tempolrc_multi.lrc` 用一行两个时间戳验证副歌重复句。

`expression.mid` 是**演奏表情混合压力素材**：8 小节 4/4，旋律是 32 个等间隔四分音符（断奏判定的前提），叠了 CC11 渐强 / 渐弱、每 8 个音一个高力度重音、两处极短门限、弯音上滑 / 下滑 / 颤音各一、两个 `0x06` 段落标记，另配一条柱式和弦伴奏轨。断言覆盖 CLI 计数（力度 11 · 渐强 1 · 渐弱 1 · 演奏法 6 · 弯音 3 · 段落 2）、文本 / HTML / MusicXML 三层记号齐全、`<wedge>` 与 `<slide>` 成对、伴奏轨不继承旋律的弯音记号，以及 `--no-dynamics` 让三层一起静默。

`parts.mid` 是**声部乐器标注 + 倚音混合压力素材**：4 条轨（Conductor + Vocal 长笛 `0xC0` 73 / Piano 钢琴 `0xC0` 0 / Bass 原声贝斯 `0xC0` 32），Vocal 轨在曲首放一个极短的 B3→C4 倚音。断言覆盖 CLI / HTML / MusicXML 三层都写出「声部：Vocal·长笛，Piano·钢琴，Bass·原声贝斯」、倚音在文本谱写成 `(7,)1`、HTML 用 `<span class='grace'>`、MusicXML 写 `<grace/>`，以及普通等间隔旋律不被误标倚音（判据按通道收紧）。

`chords.mid` 是**和弦转位 + 七和弦扩充素材**：四小节、每 960 tick 一个和声窗，依次是 `C/E`（E3 在低音）、`Bm7b5`（B D F A）、`G7sus4`（G C D F）、原位 `C`。旋律是八个上行四分音符（单声部、音域宽），确保旋律轨判定不会选中柱式和弦的伴奏轨。断言覆盖文本谱三处新和弦、原位 `[C]` 只出现一次（不写斜杠）、HTML 和弦带同步、MusicXML 写出 4 个 `<harmony>` 且转位带 `<bass-step>E</bass-step>`、`<kind text>` 保留 `m7b5` / `7sus4` 显示后缀。

`tempo_ramp.mid` 是**渐快渐慢素材**：8 小节 4/4，按 DAW 的真实写法造速度曲线——每半拍一个 `set_tempo` 自动化节点（`accel.` 13 个从 100 升到 140，`rit.` 13 个从 140 降到 80），中间夹恒定段，第 8 小节阶跃回 120。断言覆盖页眉折叠成 `♩=100 → accel. 100→140(第2小节) → rit. 140→80(第4小节) → 120(第8小节)`（分隔符只有 3 个，不是 26 个）、HTML 只有 4 个速度标记且具体 BPM 只有曲首 100 与 a tempo 120 两个、MusicXML 写出 `<words>rit.</words>` / `<words>accel.</words>` 且 `<metronome>` 不超过 6 个、以及单调速素材 `sample.mid` 不冒出任何 rit./accel.。

`long_tie.mid` 是**长音跨小节（延音线）素材**：9 小节 4/4，旋律轨在 bar4 放一个 **6 拍**长音（跨到下一小节第 2 拍）、在 bar7 放一个 **10 拍**长音（跨三小节），另配一条每小节一个 **4 拍柱式三和弦**的钢琴轨。断言覆盖文本谱 `3 - - -` 之后跟着 `- - 4 5`（不是 `0 - 4 5`）、每个小节正好 4 个 token（长音没被门限放大）、HTML 单独写出的延音线共 8 个、MusicXML 的 `<tie>` 起止成对 3/3 且 `<tied>` 同步；钢琴轨的 9 个和弦每行正好 12 个 token，锁住「复调轨长音不被放大」这条。

路线图里的「更多真实 MIDI 样本校验」由这些混合素材配合 `die_yi_sui_xue_vocal.mid`（真实人声录音导出）完成：每次发布都用它做一次带 LRC 的全链路回归渲染，确认新的网格 / 调号 map / 调式校正 / 时间轴 / 表情分析没有改变已有曲目的输出（v1.20.0 的真实曲目 MusicXML 与 v1.19.0 **逐字节一致**，HTML 谱面内容也逐字节一致，仅内联 CSS 多了几条定义）。这份曲子音头间隔是 0.5 / 1 / 1.5 / 2 拍混合，正好用来守住「断奏不误判」这道闸门。

### WASM 冒烟测试（校验提交进 docs/wasm/ 的产物是否与源码同步）

```bash
node tests/wasm_smoke.mjs                                  # 默认测 docs/wasm/
node tests/wasm_smoke.mjs build-wasm/midi2jianpu.js        # 或测新编的
```

需要 Node 18+。它走的是 `docs/index.html` 里 `callWasm()` 的同一条调用路径，
逐项检查网页每个按钮背后的导出符号是否存在、返回内容是否合法、坏数据会不会崩。

**这个测试的存在理由**：仓库曾出现「源码加了 `_midi2jianpu_musicxml`，但
`docs/wasm/` 里还是旧产物」的静默回归——CLI 测试全绿，网页按钮点了没反应。
CLI 测试永远发现不了这类问题，只能由它兜住。改了 `src/` 一定要重编 wasm 并跑一遍。

## 输出示例

```text
时值量化: 修正 1 个音符 (网格 20 tick, 门限 1)

共提取 32 个音符 (division=480, 拍号=4/4) (MIDI调号)

1=C 大调    4/4    ♩=120
----------------------------------------
[Vocal] (15 音符) · 和弦: 4 个
[C] [p] 1 2 3 4  |
[F] 5 - 6 7  |
[Cmaj7] [ff] 1' - 3' -  |
[F] [mf] 3(5 5 5 ) 0 6 7  |
[Piano] (15 音符)
1, - - - 3, - - - 5, - - -  |
4, - - - 6, - - - 1 - - -  |
1, - 3, - 5, - 5, - 7, - 2 -  |
4, - - - 6, - - - 1 - - -  |
[Pad] (2 音符)
0 - - 1, -  |
0 1 - - -  |
```

调性一律按**首调**记谱：小调写成 `1=关系大调`，主音落在 `6`。
上面这份 fixture 是大调，所以直接是 `1=C 大调`；换成 a 小调会显示 `1=C 小调 (A小调, 主音=6)`。

## 记谱约定

| 记号 | 含义 |
| --- | --- |
| `1`–`7` | 唱名（1 = do），随调性移动，数字本身不含绝对音高 |
| `'` / `,` | 高 / 低八度，可叠加（`1''`、`1,,`） |
| `#` / `b` | 升 / 降半音，写法跟调号走（降号调优先写 `b`） |
| `0` | 休止符 |
| `-` | 延长一拍（前一音符时值 +1 拍），一个四分音符长音符会显示成 `1 -` |
| `_` `.` `=` `%` | 时值后缀，见下表 |
| `3(...)` | 三连音 |
| `[C]` `[Am7]` | 和弦标记，只标在旋律轨 |
| `[p]` `[ff]` | 力度记号，只跟随所属通道 |

### 时值后缀

后缀紧跟在数字之后，可组合：

| 后缀 | 时值 | 说明 |
| --- | --- | --- |
| `%%` | 三十二分音符 | 三条减时线 |
| `=` | 十六分音符 | 两条减时线 |
| `=.` | 附点十六分音符 | |
| `_` | 八分音符 | 一条减时线 |
| `_.` | 附点八分音符 | |
| *(无)* | 四分音符 | |
| `.` | 附点四分音符 | |
| `-` | 二分音符 | 独立 token，写作 `1 -` |
| `--` / `---` | 附点二分 / 全音符 | 写作 `1 - -` / `1 - - -` |

**八度基准**随主音移动：`1` 代表主音所在八度，而不是固定以 C4 为基准。所以 `--key G` 时 C5 写作 `4` 而不是 `4'`。

## WebAssembly（网页版）

### 一、装 emsdk

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest        # Windows: emsdk.bat install latest
./emsdk activate latest
source ./emsdk_env.sh         # Windows: emsdk_env.bat
emcc -v                       # 应打印 6.0.x / clang 24
```

**Windows 上更省事的做法**：不要进 Git Bash 调 `emcc.bat`（MSYS 会把参数里的路径
改写掉），把命令写进一个 `.cmd` 再执行：

```bat
@echo off
setlocal
set "EMSDK=E:\path\to\emsdk"
set "EM_CONFIG=%EMSDK%\.emscripten"
set "PATH=%EMSDK%;%EMSDK%\upstream\emscripten;%EMSDK%\node\24.19.0_64bit;%PATH%"
emcc --version
```

`EM_CONFIG` 指向 `emsdk activate` 生成的 `.emscripten`，不设它 emcc 会去找
`%USERPROFILE%\.emscripten` 并报找不到配置。

**`emsdk install` 在沙箱/受限环境里会在最后一步失败**（`OSError: SHFileOperationW 失败: 0x78`）：
文件其实已经解压好了，只是清理下载缓存时把 `os.remove` 重定向到回收站，而无桌面
会话时回收站不可用。两个解决办法：

- 加 `EMSDK_KEEP_DOWNLOADS=1` 重跑 `install`，跳过删除缓存的步骤；
- 或者直接重跑 `install`（缓存 zip 还在，会复用），完成后 `activate` 就能通过。

### 二、编译

```bash
emcmake cmake -B build-wasm -S . -DM2J_BUILD_WASM=ON
cmake --build build-wasm

# 先验证产物齐全，再覆盖到文档目录
node tests/wasm_smoke.mjs build-wasm/midi2jianpu.js

cp build-wasm/midi2jianpu.js   docs/wasm/
cp build-wasm/midi2jianpu.wasm docs/wasm/
```

> **Windows 上没有 ninja / make**：`emcmake cmake` 需要 Ninja 或 Make 生成器，
> MSVC 的默认生成器驱动不了 emcc。装一个 ninja（或 `pip install ninja`、
> `choco install ninja`）后用 `emcmake cmake -G Ninja -B build-wasm -S . -DM2J_BUILD_WASM=ON`；
> 实在装不上就直接调 `em++`，参数与 CMakeLists 完全一致：
>
> ```bash
> em++ -std=c++17 -O2 -Iinclude \
>   src/wasm_bindings.cpp src/midifile.cpp src/notation.cpp \
>   src/html_writer.cpp src/musicxml_writer.cpp \
>   -o build-wasm/midi2jianpu.js \
>   -sEXPORTED_FUNCTIONS='["_malloc","_free","_midi2jianpu_parse","_midi2jianpu_html","_midi2jianpu_musicxml","_midi2jianpu_result_len","_midi2jianpu_copy_result","_midi2jianpu_set_lyrics","_midi2jianpu_set_title","_midi2jianpu_set_use_midi_lyrics","_midi2jianpu_midi_lyric_count","_midi2jianpu_key_change_count","_midi2jianpu_tempo_change_count","_midi2jianpu_set_use_pentatonic","_midi2jianpu_pentatonic_info","_midi2jianpu_get_notes_json","_midi2jianpu_set_use_dynamics","_midi2jianpu_expression_count","_midi2jianpu_part_info"]' \
>   -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","lengthBytesUTF8","stringToUTF8","HEAPU8"]' \
>   -sMODULARIZE=1 -sEXPORT_NAME=createMidi2Jianpu \
>   -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,node
> ```

产物 `midi2jianpu.js` + `midi2jianpu.wasm`，`docs/index.html` 直接加载这两个文件。
两个链接参数有坑，改之前先看：

- `-sENVIRONMENT=web,node`：`web` 供浏览器，`node` 让冒烟测试能在命令行跑；只留 `web` 时 Node 加载会直接报环境不支持。
- `-sEXPORTED_RUNTIME_METHODS` 必须含 `HEAPU8`：新版 Emscripten 不再默认把内存视图挂到
  Module 上，而网页正是用 `wasmMod.HEAPU8.set(bytes, ptr)` 把 MIDI 写进 wasm 内存的。
  漏了它页面不报错、只是转换永远没结果——`wasm_smoke.mjs` 会先一步把这它测出来。

用 `file://` 打开会因浏览器跨域策略拒绝加载 `.wasm`，本地预览请起一个 HTTP 服务：

```bash
python -m http.server 8000 -d docs
```

### 三、国内下载 emsdk 的坑

emsdk 的二进制包全部放在 `storage.googleapis.com`，大文件传输很容易被中途重置
（`WinError 10054` / `curl: (35) Recv failure`）。两个实测有效的办法：

1. **curl 的 schannel 吊销检查会误报失败**，加 `--ssl-no-revoke` 即可：
   ```bash
   curl --ssl-no-revoke -O https://storage.googleapis.com/webassembly/...
   ```
2. **分块断点续传**：`tools/fetch.py` 把文件切成 2MB 一块，每块单独重试，
   中断后重跑会从 `.part` 续上。669MB 的主包靠它能稳定下完：
   ```bash
   python tools/fetch.py <url> tools/emsdk/downloads/wasm-binaries.zip --chunk 2097152
   ```
   下完再跑 `./emsdk install latest`，emsdk 会复用 `downloads/` 里的缓存不再重下。

导出的函数（`src/wasm_bindings.cpp`）：

```text
_midi2jianpu_parse          解析 MIDI，返回结果长度
_midi2jianpu_html           生成 HTML 谱面
_midi2jianpu_musicxml       生成 MusicXML
_midi2jianpu_set_lyrics     注入 LRC 歌词
_midi2jianpu_set_title      设置谱面标题（网页传文件名）
_midi2jianpu_set_use_midi_lyrics  网页「使用 MIDI 自带歌词」开关
_midi2jianpu_midi_lyric_count     查询文件自带的可唱音节数（供网页提示）
_midi2jianpu_get_notes_json 取音符数组（供在线播放）
_midi2jianpu_result_len     结果长度
_midi2jianpu_copy_result    拷出结果
```

> **注意**：网页上的「下载 MusicXML」按钮做了能力探测——若 `docs/wasm/midi2jianpu.wasm`
> 是在 `_midi2jianpu_musicxml` 导出加入之前编译的，会提示改用命令行版，而不是静默失败。
> 当前仓库里的产物已包含该导出（Emscripten 6.0.13 编译）。

## 常见问题

**MSVC 报 `crtdbg.h` 找不到**
`vcvars` 环境里的 `WindowsSDKVersion` 为空时会漏掉 SDK 的 ucrt 路径。用 `x64 Native Tools Command Prompt for VS 2022` 打开命令行，或重新安装 Windows 10/11 SDK。

如果当前环境里 `reg.exe` 不可用（沙箱、部分 CI 容器），`vcvars64.bat` 查不到 SDK 版本，同样是这个报错。此时手工把 SDK 路径并进环境变量即可，不必改系统配置：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64;%LIB%"
```

**链接时报 `LNK2019: 无法解析的外部符号`**
确认源文件列表包含全部四个核心文件：`midifile.cpp`、`notation.cpp`、`html_writer.cpp`、`musicxml_writer.cpp`。手写编译脚本时容易漏掉后两个。

**Git Bash / MSYS2 下 `cl /nologo` 报「无法识别的源文件类型」**
MSYS 会把 `/nologo` 当路径转换，先 `export MSYS2_ARG_CONV_EXCL='*'`。

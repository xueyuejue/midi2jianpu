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

### 单元测试（116 项）

```bash
cd build && ctest --output-on-failure
# 或直接跑
./build/test_basic
```

成功时输出 `共 116 项，失败 0 项 -> 全部通过`。

覆盖范围：调性识别、各调八度基准、升降号书写（含降号调）、时值后缀、三连音、同音合并、和弦检测、移调、MusicXML 小节时长与 `<chord/>`，以及五组解析层回归——**鼓轨过滤**、**旋律轨判定**、**变速 tempo map 秒数换算**、**弱起小节网格**、**歌词分配与一字多音区间**（都在内存里现构造最小 SMF，不依赖 fixture 文件）。

### 冒烟测试（端到端，验证 README 承诺的每个参数）

需要 Python 3（只用标准库）：

```bash
python tests/fixtures/make_fixture.py     # 生成 sample / humanized / drums / melisma / harmony / pickup 六份素材
python tests/smoke.py build/midi2jianpu   # Windows 换成 build\Release\midi2jianpu.exe
```

覆盖 `--key` / `--track` / `--transpose` / `--musicxml` / `--html` / `--lyrics` / `--pickup` / `--no-pickup` / `--output` / `--help` / `--version` 全部参数，并校验导出的 XML 里 `duration` 为整数、和弦音带 `<chord/>`，带歌词时 `<lyric>` / `<slur>` 成对且 `<notations>` 排在 `<lyric>` 之前。

其中 `drums.mid` 专测打击乐轨：鼓轨有 192 个音符（远超旋律的 8 个），谱面里不能出现 `[Drums]`、不能出现鼓的音高，旋律轨必须判给单声部的 Vocal。

`melisma.mid` 专测一字多音：一行 LRC 只有 4 个字，旋律却在两小节里唱 8 个音。四个字必须分到两小节（早先顺序分配会把它们全塞进第一小节，第二小节看着像漏词）；每个字还要在 HTML 里对应一条 `class='sgrp'` 的弧线、在 MusicXML 里对应一对 `begin`/`end` 与成对的 `<slur>`。

`harmony.mid` 专测和声粒度与变速：钢琴一小节换两次和弦（`| C Am | F G |`），conductor 轨在第 5 小节把速度从 120 改成 90。断言要求同一小节里同时出现 `[C]` 与 `[Am]`（小节起点和弦在标记行、第三拍和弦浮在音符上方各算一个），且速度标记只印在第一行谱上（两轨各印一遍会出 4 个而不是 2 个）。

减时线连通也在这一组里校验：三连音必须是「一条线 + 一个 3」，不能被时值表最近的 0.375 档误判成附点十六分（两条线加一个点）。

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
>   -sEXPORTED_FUNCTIONS='["_malloc","_free","_midi2jianpu_parse","_midi2jianpu_html","_midi2jianpu_musicxml","_midi2jianpu_result_len","_midi2jianpu_copy_result","_midi2jianpu_set_lyrics","_midi2jianpu_get_notes_json"]' \
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

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

### 单元测试（58 项）

```bash
cd build && ctest --output-on-failure
# 或直接跑
./build/test_basic
```

成功时输出 `PASS=58 FAIL=0`。

覆盖范围：调性识别、各调八度基准、升降号书写（含降号调）、时值后缀、三连音、同音合并、和弦检测、移调、MusicXML 小节时长与 `<chord/>`。

### 冒烟测试（端到端，验证 README 承诺的每个参数）

需要 Python 3（只用标准库）：

```bash
python tests/fixtures/make_fixture.py     # 生成 tests/fixtures/sample.mid
python tests/smoke.py build/midi2jianpu   # Windows 换成 build\Release\midi2jianpu.exe
```

覆盖 `--key` / `--track` / `--transpose` / `--musicxml` / `--html` / `--lyrics` / `--output` / `--help` / `--version` 全部参数，并校验导出的 XML 里 `duration` 为整数、和弦音带 `<chord/>`。

## 输出示例

```text
共提取 30 个音符 (division=480, 拍号=4/4) (MIDI调号)

1 = C 大调    4/4    ♩=120
----------------------------------------
[Vocal] (15 音符) · 和弦: 4 个
[C] [p] 1 2 3 4  |
[F] 5 - 6 7  |
[Cmaj7] [ff] 1' - 3' -  |
[F] [mf] 3(5 5 5 ) 0 6 7  |
[Piano] (15 音符)
1, - - - 3, - - - 5, - - -  |
4, - - - 6, - - - 1 - - -  |
```

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

```bash
# 需要 emsdk 已 source 到 PATH
emcmake cmake -B build-wasm -S . -DM2J_BUILD_WASM=ON
cmake --build build-wasm
```

产物 `midi2jianpu.js` + `midi2jianpu.wasm`，复制到 `docs/wasm/` 即可被 `docs/index.html` 加载。

导出的函数（`src/wasm_bindings.cpp`）：

```text
_midi2jianpu_parse          解析 MIDI，返回结果长度
_midi2jianpu_html           生成 HTML 谱面
_midi2jianpu_musicxml       生成 MusicXML
_midi2jianpu_set_lyrics     注入 LRC 歌词
_midi2jianpu_get_notes_json 取音符数组（供在线播放）
_midi2jianpu_result_len     结果长度
_midi2jianpu_copy_result    拷出结果
```

> **注意**：仓库里已提交的 `docs/wasm/midi2jianpu.wasm` 可能是在 `_midi2jianpu_musicxml` 导出之前编译的。网页上的「下载 MusicXML」按钮已做能力探测，若 wasm 未导出该函数会给出提示而不是静默失败。要启用网页版 MusicXML 导出，按上面的命令重新编译 wasm 并覆盖 `docs/wasm/`。

## 常见问题

**MSVC 报 `crtdbg.h` 找不到**
`vcvars` 环境里的 `WindowsSDKVersion` 为空时会漏掉 SDK 的 ucrt 路径。用 `x64 Native Tools Command Prompt for VS 2022` 打开命令行，或重新安装 Windows 10/11 SDK。

**链接时报 `LNK2019: 无法解析的外部符号`**
确认源文件列表包含全部四个核心文件：`midifile.cpp`、`notation.cpp`、`html_writer.cpp`、`musicxml_writer.cpp`。手写编译脚本时容易漏掉后两个。

**Git Bash / MSYS2 下 `cl /nologo` 报「无法识别的源文件类型」**
MSYS 会把 `/nologo` 当路径转换，先 `export MSYS2_ARG_CONV_EXCL='*'`。

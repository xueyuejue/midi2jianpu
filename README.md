# midi2jianpu

> 直接将 MIDI 文件转换为排版美观、可打印的中文简谱（数字谱）。

**在线试玩：[https://xueyuejue.github.io/midi2jianpu/](https://xueyuejue.github.io/midi2jianpu/)**（拖入 .mid 即转，零安装）

## 这是什么

`midi2jianpu` 是一个面向中文音乐人的开源工具：你手里的 `.mid` 文件——FL Studio 导出、电子琴/合成器录制、扒谱工程——丢进来，就能得到一份排版干净、可以直接打印或在手机上看的**中文简谱（数字谱）**。

### 与现有方案的区别

- 大多数简谱工具从 **MusicXML / 五线谱**倒推，而音乐人手里最多的文件其实是 **MIDI**；
- 现有脚本多是一次性转换器，输出格式杂乱、不能编辑、不能排版；
- 本项目目标是「丢入 MIDI → 干净简谱」一步到位，核心用 C++ 写成，零第三方依赖。

## 已实现

- [x] 解析标准 MIDI 文件（手写二进制解析，多轨、多通道、可变速度）
- [x] 自动识别调性（优先用 MIDI 内置调号，否则旋律轨加权猜测），换算首调唱名
- [x] 手动指定调性：`--key Em` / `--key C`
- [x] 识别拍号、速度（BPM）、调号、轨道名称 meta 事件
- [x] 按拍号划分小节、输出小节线、HTML 自动换行排版
- [x] 时值标注：全/二分/四分/八分/十六分音符及附点，延长线 `-`
- [x] 同音延音合并（`1 1 1` → `1 - -`）
- [x] 多轨分离（优先用 MIDI 内置轨道名）
- [x] 休止符 `0`
- [x] 高低八度点、升降号 `#`/`b`
- [x] 强弱记号（从 MIDI Expression 解析：pp/p/mp/mf/f/ff）
- [x] 三连音自动识别（连续3个等长音符，组内不做同音合并）
- [x] 弯音轮修正（Pitch Bend 平均偏移 ±0.5 半音以上修正音高）
- [x] **HTML 谱面导出**：自包含网页，浏览器打开即可打印 PDF
- [x] **MusicXML 导出**：`--musicxml out.xml`，含调号/拍号/附点/三连音/休止符，可被 MuseScore 打开
- [x] **歌词写入**：`--lyrics lyrics.lrc` 支持 LRC 格式，按时间戳精确对齐到音符
- [x] 文本输出到文件：`--output out.txt`
- [x] 多轨选择：`--track 0`
- [x] **在线 Demo**：WASM 核心，浏览器内运行 C++ 代码
- [x] 26 项单元测试

## 使用

### 构建（需 C++17 编译器 + CMake）

```bash
cmake -B build -S .
cmake --build build
```

### 转换

```bash
# 打印文本简谱（自动猜调性）
./build/midi2jianpu 你的歌.mid

# 手动指定调性（更准）
./build/midi2jianpu 你的歌.mid --key Em

# 导出可打印的 HTML 谱面
./build/midi2jianpu 你的歌.mid --key Em --html 简谱.html

# 文本和HTML同时输出到文件
./build/midi2jianpu 你的歌.mid --key Em --output 简谱.txt --html 简谱.html

# 带歌词（LRC格式，用户普遍有）
./build/midi2jianpu 人声.mid --key Em --html 带歌词简谱.html --lyrics lyrics.lrc
```

打开 `简谱.html` 即可在浏览器查看、打印为 PDF。

## 输出示例

```
1 = E 小调    4/4
----------------------------------------
[Vocal] (286 音符)
1 7 6 7 1. 7 6  |  0 7 1. 7 6  |
风 霜 染 尽 客 行 袍    江 湖 夜 雨
```

记谱约定：数字为唱名（1=do），`'` 高八度，`,` 低八度，`#`/`b` 升降半音，`0` 休止，`-` 延长一拍。

## 技术选型

- 核心：**C++17**，手写 SMF 二进制解析，无第三方库依赖，便于编译成 WASM
- 输出：文本 + 自包含 HTML（内联 CSS）
- 测试：内置断言，`ctest` 运行

## 路线图

- [x] 在线网页版（[GitHub Pages](https://xueyuejue.github.io/midi2jianpu/)，WASM 核心，C++ 编译）
- [x] MusicXML 导出（调号/拍号/附点/三连音/休止符，可被 MuseScore 打开）
- [x] RPN 弯音范围解析
- [x] 歌词写入（LRC 格式，按时间戳精确对齐）
- [x] WASM 编译（浏览器内运行 C++ 核心，功能与命令行版完全同步）
- [x] HTML 排版优化（白色背景、清晰音符、灰色歌词）
- [ ] 延音线弧线（当前用 `-` 符号，后续加弧线视觉）
- [ ] PNG 谱面导出（浏览器 canvas 截图）
- [ ] 更多真实 MIDI 样本校验

## 为什么开源

中文简谱是中国特有的记谱体系，国外打谱软件（MuseScore、Finale）对它支持很差，国内又一直缺少一个干净的、可二次开发的基础库。这个项目想把「MIDI → 简谱」这层地基打好，让后来的人不用重复造轮子。

## 贡献

Issue / PR 都欢迎。尤其欢迎：

- 真实 MIDI 样本（钢琴、民乐、电子琴录制）
- 简谱排版规范与视觉校验
- 各调式换算测试用例

## License

[MIT](./LICENSE)

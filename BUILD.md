# 构建与运行

## C++ 核心（主版本）

依赖：C++17 编译器（GCC 7+/Clang/MSVC 2017+）、CMake 3.14+。

```bash
cmake -B build -S .
cmake --build build
./build/midi2jianpu 你的歌曲.mid        # Linux/macOS
# Windows (VC++ 2022):
# build\Release\midi2jianpu.exe 你的歌曲.mid
```

跑单元测试：

```bash
cd build && ctest --output-on-failure
```

## Python 参考实现（辅助/对照）

位于 `python/`，用于快速验证算法逻辑：

```bash
pip install -r python/requirements.txt
python python/midi2jianpu.py 你的歌曲.mid
```

## 输出示例

```
共提取 8 个音符 (division=480)

主音(1=): C 大调
----------------------------------------
1  (C4, 0.50s)
2  (D4, 0.50s)
3  (E4, 0.50s)
4  (F4, 0.50s)
5  (G4, 0.50s)
6  (A4, 0.50s)
7  (B4, 0.50s)
1'  (C5, 0.50s)
```

记谱约定：数字为简谱唱名（1=do），`'` 表示高八度，`,` 表示低八度，`#`/`b` 为升降半音。

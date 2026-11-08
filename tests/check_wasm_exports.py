#!/usr/bin/env python3
"""WASM 导出清单一致性守护（纯标准库，本地 / CI 都能跑）。

项目的硬约定：新增或改名一个 WASM 导出，必须同步
  build/build_wasm.cmd、CMakeLists.txt、docs/index.html、tests/wasm_smoke.mjs
外加 src/wasm_bindings.cpp 里的 extern "C" 定义。历史上出过「源码已加
_midi2jianpu_musicxml，但 docs/wasm/ 里还是旧产物」的静默回归——CLI 测试全绿、
网页按钮点了没反应。这个脚本不编译、不开 wasm，只做静态比对，把清单漂移挡在
编译之前：

  1. 两个构建入口（cmd / CMake）的 EXPORTED_FUNCTIONS 与 RUNTIME_METHODS 完全一致；
  2. 两处清单与源码 extern "C" 里的业务导出定义完全一致（排除 _malloc / _free）；
  3. 网页与 WASM 冒烟里静态、动态调用到的每个符号都在清单里。

用法：python3 tests/check_wasm_exports.py
退出码 0 = 一致，1 = 有漂移（差异会逐项打印）。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = {"_malloc", "_free"}  # Emscripten 运行时函数，源码里没有定义

problems = []


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def s_list(text, key, where):
    """取出 -sKEY=['a','b'] / -sKEY=["a","b"] 里的名字集合。"""
    m = re.search(r"-s" + key + r"=\[([^\]]*)\]", text)
    if not m:
        problems.append(f"{where}: 找不到 -s{key}=[...]")
        return set()
    names = set(re.findall(r"['\"]([^'\"]+)['\"]", m.group(1)))
    if not names:
        problems.append(f"{where}: -s{key} 清单解析为空")
    return names


def source_definitions(text):
    """从 extern "C" { ... } // extern "C" 块里取函数定义名。"""
    m = re.search(r'extern "C" \{(.*?)\} // extern "C"', text, re.S)
    if not m:
        problems.append('src/wasm_bindings.cpp: 找不到 extern "C" 块')
        return set()
    names = set()
    for line in m.group(1).splitlines():
        if line.lstrip().startswith("//"):
            continue
        mm = re.match(r"^\s*(?:const\s+)?[\w:*&<> ]+\s+(midi2jianpu_[a-z0-9_]+)\s*\(", line)
        if mm:
            names.add("_" + mm.group(1))
    return names


def used_symbols(text):
    """网页 / 冒烟里调用到的符号：wasmMod._x、mod._x、callWasm('x')、call('x')。"""
    found = set(re.findall(r"(?:wasmMod|mod)\.(_[A-Za-z0-9_]+)", text))
    for name in re.findall(r"(?:callWasm|call)\(\s*['\"]([A-Za-z0-9_]+)['\"]", text):
        found.add("_" + name)
    return found


def compare(a, b, label_a, label_b):
    if a != b:
        if a - b:
            problems.append(f"{label_a} 独有（{label_b} 缺）: {sorted(a - b)}")
        if b - a:
            problems.append(f"{label_b} 独有（{label_a} 缺）: {sorted(b - a)}")


def main():
    cmd = read(os.path.join("build", "build_wasm.cmd"))
    cmake = read("CMakeLists.txt")
    src = read(os.path.join("src", "wasm_bindings.cpp"))
    page = read(os.path.join("docs", "index.html"))
    wasm_test = read(os.path.join("tests", "wasm_smoke.mjs"))

    cmd_fns = s_list(cmd, "EXPORTED_FUNCTIONS", "build/build_wasm.cmd")
    cmake_fns = s_list(cmake, "EXPORTED_FUNCTIONS", "CMakeLists.txt")
    compare(cmd_fns, cmake_fns, "build_wasm.cmd", "CMakeLists.txt")

    cmd_rt = s_list(cmd, "EXPORTED_RUNTIME_METHODS", "build/build_wasm.cmd")
    cmake_rt = s_list(cmake, "EXPORTED_RUNTIME_METHODS", "CMakeLists.txt")
    compare(cmd_rt, cmake_rt, "build_wasm.cmd(rt)", "CMakeLists.txt(rt)")

    src_fns = source_definitions(src)
    compare(cmd_fns - RUNTIME, src_fns, "构建清单(业务导出)", "wasm_bindings.cpp")

    for where, text in (("docs/index.html", page), ("tests/wasm_smoke.mjs", wasm_test)):
        missing = used_symbols(text) - cmd_fns - cmd_rt
        if missing:
            problems.append(f"{where} 调用了清单里没有的符号: {sorted(missing)}")

    if problems:
        print("X WASM 导出清单漂移：")
        for p in problems:
            print("  - " + p)
        print("\n新增 / 改名导出时，请同步 build/build_wasm.cmd、CMakeLists.txt、")
        print("src/wasm_bindings.cpp、docs/index.html、tests/wasm_smoke.mjs，并重编 docs/wasm/。")
        return 1
    n = len(cmd_fns - RUNTIME)
    print(f"OK WASM 导出清单一致：{n} 个业务导出 + {len(cmd_rt)} 个运行时方法，")
    print("   build_wasm.cmd / CMakeLists.txt / wasm_bindings.cpp / 网页 / 冒烟 五处无漂移")
    return 0


if __name__ == "__main__":
    sys.exit(main())

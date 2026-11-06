cmake_minimum_required(VERSION 3.14)
project(midi2jianpu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 源码含中文注释与中文字面量：MSVC 默认按系统代码页解释，会输出乱码，
# 必须显式 /utf-8。其余编译器统一开常用告警。
if(MSVC)
  add_compile_options(/utf-8 /W3)
else()
  add_compile_options(-Wall -Wextra)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# 核心库（CLI 与 WASM 共用，避免源文件列表两处维护）
set(M2J_CORE
  src/midifile.cpp
  src/notation.cpp
  src/html_writer.cpp
  src/musicxml_writer.cpp
)

# ---- 命令行程序 ----
add_executable(midi2jianpu src/main.cpp ${M2J_CORE})

# ---- 单元测试 ----
enable_testing()
add_executable(test_basic tests/test_basic.cpp src/midifile.cpp src/notation.cpp)
add_test(NAME basic COMMAND test_basic)

# ---- WebAssembly（可选；需要 emsdk 提供 emcmake / em++）----
#   emcmake cmake -B build-wasm -S . -DM2J_BUILD_WASM=ON
#   cmake --build build-wasm
# 产物：midi2jianpu.js + midi2jianpu.wasm，拷贝到 docs/wasm/ 即可
option(M2J_BUILD_WASM "Build the WebAssembly module (requires Emscripten)" OFF)
if(M2J_BUILD_WASM)
  add_executable(midi2jianpu_wasm src/wasm_bindings.cpp ${M2J_CORE})
  set_target_properties(midi2jianpu_wasm PROPERTIES OUTPUT_NAME "midi2jianpu")
  target_link_options(midi2jianpu_wasm PRIVATE
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_midi2jianpu_parse','_midi2jianpu_html','_midi2jianpu_musicxml','_midi2jianpu_result_len','_midi2jianpu_copy_result','_midi2jianpu_set_lyrics','_midi2jianpu_set_title','_midi2jianpu_set_use_midi_lyrics','_midi2jianpu_midi_lyric_count','_midi2jianpu_key_change_count','_midi2jianpu_set_use_pentatonic','_midi2jianpu_pentatonic_info','_midi2jianpu_get_notes_json']"
    # HEAPU8 必须显式导出：新版 Emscripten 不再默认把它挂到 Module 上，
    # 而 docs/index.html 正是用它把 MIDI 字节拷进 wasm 内存的。
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','lengthBytesUTF8','stringToUTF8','HEAPU8']"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createMidi2Jianpu"
    "-sALLOW_MEMORY_GROWTH=1"
    # web 供浏览器；node 供 CI/命令行冒烟测试（tests/wasm_smoke.mjs）
    "-sENVIRONMENT=web,node")
endif()

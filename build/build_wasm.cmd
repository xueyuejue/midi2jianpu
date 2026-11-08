@echo off
setlocal
rem emsdk lives in ../../tools (part of the portable tree). On another machine
rem use "set EMSDK=..." instead of editing this script.
rem %%~fI resolves ..\.. to a clean absolute path: PATH entries containing
rem ".." make em++ think the config changed and wipe its sysroot cache.
if not defined EMSDK for %%I in ("%~dp0..\..\tools\emsdk") do set "EMSDK=%%~fI"
set "EM_CONFIG=%EMSDK%\.emscripten"
set "PATH=%EMSDK%;%EMSDK%\upstream\emscripten;%EMSDK%\node\24.19.0_64bit;%PATH%"
rem Go back to the repo root from this script's folder (drive-letter agnostic).
cd /d "%~dp0.."
if not exist build-wasm mkdir build-wasm
em++ -std=c++17 -O2 -Iinclude src/wasm_bindings.cpp src/midifile.cpp src/notation.cpp src/html_writer.cpp src/musicxml_writer.cpp -o build-wasm/midi2jianpu.js -sEXPORTED_FUNCTIONS=["_malloc","_free","_midi2jianpu_parse","_midi2jianpu_html","_midi2jianpu_musicxml","_midi2jianpu_result_len","_midi2jianpu_copy_result","_midi2jianpu_set_lyrics","_midi2jianpu_set_title","_midi2jianpu_set_use_midi_lyrics","_midi2jianpu_midi_lyric_count","_midi2jianpu_key_change_count","_midi2jianpu_tempo_change_count","_midi2jianpu_tempo_ramp_count","_midi2jianpu_set_use_pentatonic","_midi2jianpu_pentatonic_info","_midi2jianpu_get_notes_json","_midi2jianpu_set_use_dynamics","_midi2jianpu_expression_count","_midi2jianpu_part_info","_midi2jianpu_file_header_info","_midi2jianpu_controller_info","_midi2jianpu_bend_range_info","_midi2jianpu_running_status_info","_midi2jianpu_port_info","_midi2jianpu_sysex_info"] -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8","HEAPU8"] -sMODULARIZE=1 -sEXPORT_NAME=createMidi2Jianpu -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,node
echo EXIT=%ERRORLEVEL%

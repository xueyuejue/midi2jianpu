@echo off
rem See build_local.cmd: set MSVC env by hand, no vcvars64.bat / reg.exe.
rem Local defaults; override VCT/SDK/SDKV on another machine.
if not defined VCT set "VCT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
if not defined SDK set "SDK=C:\Program Files (x86)\Windows Kits\10"
if not defined SDKV set "SDKV=10.0.26100.0"
set "PATH=%VCT%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%VCT%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt"
set "LIB=%VCT%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
rem Go back to the repo root from this script's folder (drive-letter agnostic).
cd /d "%~dp0.."
if not exist build\testobj mkdir build\testobj
cl /nologo /utf-8 /W3 /std:c++17 /EHsc /O2 /Iinclude ^
   tests\test_basic.cpp src\midifile.cpp src\notation.cpp src\musicxml_writer.cpp ^
   src\html_writer.cpp ^
   /Fe:build\test_basic.exe /Fo:build\testobj\ 2>&1
echo EXIT=%ERRORLEVEL%

@echo off
rem Local-only: skip vcvars64.bat because it queries the registry via reg.exe,
rem which is blocked by the local security policy. Set MSVC + SDK paths by hand.
rem Local defaults; override VCT/SDK/SDKV on another machine.
if not defined VCT set "VCT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
if not defined SDK set "SDK=C:\Program Files (x86)\Windows Kits\10"
if not defined SDKV set "SDKV=10.0.26100.0"
set "PATH=%VCT%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%VCT%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt"
set "LIB=%VCT%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
rem Go back to the repo root from this script's folder (drive-letter agnostic).
cd /d "%~dp0.."
cl /nologo /utf-8 /W3 /std:c++17 /EHsc /O2 /Iinclude ^
   src\main.cpp src\midifile.cpp src\notation.cpp ^
   src\html_writer.cpp src\musicxml_writer.cpp ^
   /Fe:build\midi2jianpu.exe /Fo:build\ 2>&1
echo EXIT=%ERRORLEVEL%

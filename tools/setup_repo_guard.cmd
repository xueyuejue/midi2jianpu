@echo off
REM 换电脑 / 绿色复制整个任务目录后，双击本文件重建仓库本地的署名守护配置。
setlocal
cd /d "%~dp0.."
where python >nul 2>nul
if %errorlevel%==0 (
    python "tools\setup_repo_guard.py"
) else (
    py -3 "tools\setup_repo_guard.py"
)
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo [安装] 未成功，退出码 %RC% 。请确认已安装 Python 3 且当前目录是完整仓库。
    pause
)
exit /b %RC%

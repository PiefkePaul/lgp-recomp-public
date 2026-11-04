@echo off
rem Double-click: the LGP build assistant (tools/kit/build.py). Installs Python first if it is missing.
rem "call" everywhere: py or python may be .cmd shims (uv), and a batch file run without call never returns.
setlocal
cd /d "%~dp0"
set LGP_KIT_PAUSE=1
set PY=
rem The ready-made package (lgp-bauset-windows-x64.zip) brings its own Python.
if exist "%~dp0bauset\python\python.exe" set PY="%~dp0bauset\python\python.exe"
if not defined PY call py -3 -c "import sys; sys.exit(sys.version_info < (3, 9))" >nul 2>nul && set PY=py -3
if not defined PY where python >nul 2>nul && call python -c "import sys; sys.exit(sys.version_info < (3, 9))" >nul 2>nul && set PY=python
if not defined PY (
    echo Python is missing. Installing it with winget ^(official package Python.Python.3.12^).
    winget install --exact --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
    if exist "%LOCALAPPDATA%\Programs\Python\Launcher\py.exe" set PY="%LOCALAPPDATA%\Programs\Python\Launcher\py.exe" -3
    if exist "%WINDIR%\py.exe" set PY="%WINDIR%\py.exe" -3
)
if not defined PY (
    echo Python could not be installed: https://www.python.org/downloads/ ^(tick "Add to PATH"^), then start again.
    pause
    exit /b 1
)
call %PY% tools\kit\build.py %*
exit /b %ERRORLEVEL%

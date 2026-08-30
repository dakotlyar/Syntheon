@echo off
setlocal enableextensions enabledelayedexpansion
title SynBench Windows Build

echo.
echo  ============================================
echo   SynBench ^| Windows Build
echo  ============================================
echo.

REM ========================================================
REM  Configuration — change PY_VERSION to pin a different
REM  Python release. Must be 3.9 or later.
REM ========================================================
set PY_VERSION=3.11.9
set PY_SHORT=311

REM ---- Detect CPU architecture (amd64 or arm64) ----------
set PY_ARCH=amd64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64"      set PY_ARCH=arm64
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64"      set PY_ARCH=arm64

set PY_URL=https://www.python.org/ftp/python/%PY_VERSION%/python-%PY_VERSION%-%PY_ARCH%.exe
set PY_INSTALLER=%TEMP%\python-%PY_VERSION%-%PY_ARCH%.exe

REM ---- Where the user-level silent install will land ------
set PY_INSTALL_DIR=%LOCALAPPDATA%\Programs\Python\Python%PY_SHORT%
set PY_EXE=%PY_INSTALL_DIR%\python.exe

REM ========================================================
REM  Step 1 – Locate or install Python
REM ========================================================

REM Check PATH first
set PYTHON=
python --version >nul 2>&1
if not errorlevel 1 (
    set PYTHON=python
    echo  [1/4] Python found in PATH.
    goto :verify_python
)

REM Check the expected silent-install location
if exist "%PY_EXE%" (
    set PYTHON=%PY_EXE%
    echo  [1/4] Python found at %PY_INSTALL_DIR%.
    goto :verify_python
)

REM Not found — download and install automatically
echo  [1/4] Python not found. Downloading Python %PY_VERSION% (%PY_ARCH%)...
echo        %PY_URL%
echo.

REM Use PowerShell to download (available on all Windows versions since Vista)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ProgressPreference='SilentlyContinue';" ^
    "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;" ^
    "Invoke-WebRequest -Uri '%PY_URL%' -OutFile '%PY_INSTALLER%' -UseBasicParsing"

if not exist "%PY_INSTALLER%" (
    echo.
    echo  ERROR: Download failed.
    echo  Check your internet connection, then try again.
    echo  Or download Python %PY_VERSION% manually from https://python.org
    echo  and re-run this script.
    pause
    exit /b 1
)

echo  Installing Python %PY_VERSION% (user-level, no admin required)...
REM Flags:
REM   /quiet              - silent install
REM   InstallAllUsers=0   - current user only (no UAC prompt)
REM   PrependPath=1       - add to PATH for future sessions
REM   Include_pip=1       - pip needed to install PyInstaller + numpy + PySide6
REM   TargetDir           - pin the install location so we can reference it below
"%PY_INSTALLER%" /quiet ^
    InstallAllUsers=0 ^
    PrependPath=1 ^
    Include_pip=1 ^
    TargetDir="%PY_INSTALL_DIR%"

set INSTALL_ERR=%ERRORLEVEL%
del "%PY_INSTALLER%" >nul 2>&1

if %INSTALL_ERR% neq 0 (
    echo.
    echo  ERROR: Python installer returned error %INSTALL_ERR%.
    echo  Try running this script as Administrator, or install Python manually.
    pause
    exit /b 1
)

set PYTHON=%PY_EXE%
echo  Python installed at %PY_INSTALL_DIR%
echo.

REM ========================================================
REM  Step 2 – Verify Python works and shows version
REM ========================================================
:verify_python
echo.
for /f "tokens=*" %%V in ('"%PYTHON%" --version 2^>^&1') do echo  [2/4] Using: %%V

echo.

REM ========================================================
REM  Step 3 – Install pip dependencies
REM ========================================================
echo  [3/4] Installing build dependencies (pip, pyinstaller, numpy, psutil, PySide6)...
"%PYTHON%" -m pip install --upgrade pip pyinstaller numpy psutil PySide6
if errorlevel 1 (
    echo.
    echo  ERROR: pip install failed. Check your internet connection.
    pause
    exit /b 1
)
echo.

REM ========================================================
REM  Step 4 – Build with PyInstaller
REM ========================================================
echo  [4/6] Building SynBench...
echo.

if exist dist\SynBench (
    echo  Cleaning previous build...
    rmdir /s /q dist\SynBench
)
if exist build rmdir /s /q build

"%PYTHON%" -m PyInstaller synbench.spec --clean
if errorlevel 1 (
    echo.
    echo  ERROR: PyInstaller build failed. See output above.
    pause
    exit /b 1
)

REM ========================================================
REM  Step 5 – Bundle a sidecar Python (for the in-app GPU/NPU
REM  plugin installer to run 'pip install' on a machine with
REM  no system Python at all). Uses the official embeddable
REM  package, same version/arch as the build Python so any
REM  compiled wheels it installs match the frozen app's ABI.
REM ========================================================
echo  [5/6] Bundling portable Python (for optional GPU/NPU plugin installs)...

set EMBED_DIR=dist\SynBench\python-embed
set EMBED_ZIP=%TEMP%\python-%PY_VERSION%-embed-%PY_ARCH%.zip
set EMBED_URL=https://www.python.org/ftp/python/%PY_VERSION%/python-%PY_VERSION%-embed-%PY_ARCH%.zip
set GETPIP=%TEMP%\get-pip.py

if exist "%EMBED_DIR%" rmdir /s /q "%EMBED_DIR%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ProgressPreference='SilentlyContinue';" ^
    "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;" ^
    "Invoke-WebRequest -Uri '%EMBED_URL%' -OutFile '%EMBED_ZIP%' -UseBasicParsing"
if not exist "%EMBED_ZIP%" (
    echo.
    echo  ERROR: Embeddable Python download failed. Check your internet connection.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Expand-Archive -Path '%EMBED_ZIP%' -DestinationPath '%EMBED_DIR%' -Force"
del "%EMBED_ZIP%" >nul 2>&1

REM The embeddable package ships with 'import site' commented out in its
REM ._pth file, which disables site-packages (and pip) entirely — re-enable it.
for %%F in ("%EMBED_DIR%\python*._pth") do (
    powershell -NoProfile -Command ^
        "(Get-Content '%%F') -replace '^#import site', 'import site' | Set-Content '%%F'"
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ProgressPreference='SilentlyContinue';" ^
    "Invoke-WebRequest -Uri 'https://bootstrap.pypa.io/get-pip.py' -OutFile '%GETPIP%' -UseBasicParsing"
"%EMBED_DIR%\python.exe" "%GETPIP%" --no-warn-script-location --quiet
del "%GETPIP%" >nul 2>&1

echo  Sidecar Python bundled at %EMBED_DIR%
echo.

REM ========================================================
REM  Step 6 – Build the installer (Inno Setup), if available.
REM  Optional: not everyone doing a local build has Inno Setup
REM  installed, so this is skipped (not fatal) when missing.
REM ========================================================
echo  [6/6] Building installer...

set ISCC=
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe
where ISCC.exe >nul 2>&1 && for /f "delims=" %%I in ('where ISCC.exe') do set ISCC=%%I

if not defined SYNBENCH_VERSION set SYNBENCH_VERSION=0.0.0-local

if defined ISCC (
    "%ISCC%" installer.iss /DMyAppVersion=%SYNBENCH_VERSION%
    if errorlevel 1 (
        echo.
        echo  WARNING: Installer build failed. See output above.
        echo  dist\SynBench\ is still usable as a portable folder.
    ) else (
        echo  Installer built: installer_output\SynBench-Setup.exe
    )
) else (
    echo  Inno Setup not found — skipping installer build.
    echo  Install it from https://jrsoftware.org/isdl.php and re-run
    echo  this script to also produce installer_output\SynBench-Setup.exe.
)
echo.

echo.
echo  ============================================
echo   Build complete!
echo.
echo   dist\SynBench\SynBench.exe
echo     ^> Double-click to launch the GUI
echo.
echo   dist\SynBench\SynBench-CLI.exe
echo     ^> Run from a Command Prompt / PowerShell:
echo        SynBench-CLI.exe --cli
echo        SynBench-CLI.exe --cli --tests cpu ram disk --json
echo        SynBench-CLI.exe --cli --tests disk --disk-dir D:\
echo.
echo   installer_output\SynBench-Setup.exe
echo     ^> Self-extracting installer with an uninstaller (if built above)
echo.
echo   Or zip dist\SynBench\ and share it as a portable folder —
echo   no Python installation is required on the end-user machine.
echo  ============================================
echo.
pause

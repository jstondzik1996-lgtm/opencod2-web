@echo off
setlocal EnableExtensions
title CoD2 Browser MP - isolated update and test
rem Download this BAT into the EXISTING work folder, beside opencod2-web/.
rem Uses the same GitHub project; it never clones, deletes, resets or force-pushes.
rem Builds in separate QA directories; leaves existing 8080/8081 previews alone.

set "WORK=%~dp0"
set "REPO=%WORK%opencod2-web"
set "SERVERROOT=%WORK%native-server"
set "ASSETS=%WORK%mp-assets\main"
set "QAWEB=%REPO%\build\qa-web"
set "QANATIVE=%REPO%\build-win32-qa"
set "QAHOME=%WORK%native-server-qa"
set "SERVERS=%WORK%cod2-qa-servers.json"
set "BRANCH=codex/browser-mp-checkpoint"
set "PYTHON=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
set "PYTHONPATH=%WORK%python-tools;%PYTHONPATH%"
set "COD2_RELAY_ALLOW_ANY="
set "COD2_PORT_MIN=28963"
set "COD2_PORT_MAX=28963"

echo.
echo  [CoD2 QA] Existing project: "%REPO%"
echo  [CoD2 QA] Separate ports: HTTP 8082 / UDP 28963
echo  [CoD2 QA] Existing preview ports will not be stopped.
echo.

if not exist "%REPO%\.git" (
  echo ERROR: "%REPO%" does not exist. No files changed.
  goto :fail
)
if not exist "%SERVERROOT%\main" (
  echo ERROR: Original clean server root is missing: "%SERVERROOT%\main"
  goto :fail
)
if not exist "%ASSETS%" (
  echo ERROR: Existing browser asset pack is missing: "%ASSETS%"
  goto :fail
)
if not exist "%WORK%emsdk\emsdk_env.bat" (
  echo ERROR: Existing Emscripten SDK not found in work\emsdk.
  goto :fail
)
if not exist "%PYTHON%" set "PYTHON=python"
rem Check Git first. The original BAT's where/git || (...) condition carried on
rem after the error; never treat "missing git" as a branch error again.
call :ensure_git
if errorlevel 1 goto :fail

rem CMake and Ninja may also be installed outside the current PowerShell PATH.
call :ensure_build_tools
if errorlevel 1 goto :fail
if not exist "%WORK%w64devkit\bin\gcc.exe" (
  echo ERROR: Existing w64devkit compiler is missing.
  goto :fail
)

rem Refuse to clobber local unfinished edits or switch an unexpected branch.
set "DIRTY="
for /f "delims=" %%x in ('git -C "%REPO%" status --porcelain --untracked-files=no') do set "DIRTY=1"
if defined DIRTY (
  echo ERROR: Local tracked changes exist. Commit/stash them manually first.
  echo No fetch/build attempted.
  goto :fail
)
set "CURRENT="
for /f "delims=" %%x in ('git -C "%REPO%" branch --show-current') do set "CURRENT=%%x"

echo [1/5] Updating existing repo without overwriting local work...
rem Fetch only changes metadata and downloaded objects, not local tracked files.
git -C "%REPO%" fetch origin "%BRANCH%"
if errorlevel 1 goto :fail
if /I not "%CURRENT%"=="%BRANCH%" (
  echo [CoD2 QA] Switching existing checkout from "%CURRENT%" to "%BRANCH%"...
  git -C "%REPO%" switch "%BRANCH%"
  if errorlevel 1 (
    echo ERROR: Could not safely switch existing checkout. No reset was run.
    goto :fail
  )
)
git -C "%REPO%" merge --ff-only "origin/%BRANCH%"
if errorlevel 1 (
  echo ERROR: Branch has diverged; refusing to overwrite local commits.
  goto :fail
)

echo [2/5] Preparing existing SDK...
set "PATH=%WORK%w64devkit\bin;%PATH%"
call "%WORK%emsdk\emsdk_env.bat" >nul
if errorlevel 1 goto :fail
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake.exe is not installed or available on PATH.
  goto :fail
)
where ninja >nul 2>nul
if errorlevel 1 (
  echo ERROR: ninja.exe missing; check w64devkit / portable SDK.
  goto :fail
)

echo [3/5] Building separate WASM and Win32 QA targets...
pushd "%REPO%"
call emcmake cmake -S . -B build/qa-web -G Ninja -DCMAKE_OBJECT_PATH_MAX=200 -DCOD2_WEB_SAFE_HEAP=ON -DCOD2_WEB_TESTING=ON
if errorlevel 1 (popd & goto :fail)
cmake --build build/qa-web -j 6
if errorlevel 1 (popd & goto :fail)
cmake -S . -B build-win32-qa -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=gcc -DCMAKE_ASM_COMPILER=gcc -DCMAKE_RC_COMPILER=windres
if errorlevel 1 (popd & goto :fail)
cmake --build build-win32-qa -j 6
if errorlevel 1 (popd & goto :fail)
popd

if not exist "%QAWEB%\cod2.html" (echo ERROR: HTML build missing & goto :fail)
if not exist "%QAWEB%\cod2.wasm" (echo ERROR: WASM build missing & goto :fail)
if not exist "%QANATIVE%\cod2_win32_ded.exe" (echo ERROR: Server executable missing & goto :fail)

echo [4/5] Linking EXISTING game assets without copying them...
if not exist "%QAWEB%\main\" (
  mklink /J "%QAWEB%\main" "%ASSETS%"
  if errorlevel 1 goto :fail
)
if exist "%REPO%\build\web\assets.json" (
  copy /Y "%REPO%\build\web\assets.json" "%QAWEB%\assets.json" >nul
)
if not exist "%QAWEB%\assets.json" (
  echo WARNING: No assets.json manifest. Web page may require selecting local files.
)
if exist "%REPO%\build\web\zone" if not exist "%QAWEB%\zone" (
  mklink /J "%QAWEB%\zone" "%REPO%\build\web\zone"
  if errorlevel 1 echo WARNING: Could not link optional zone assets.
)
if not exist "%QAHOME%\main" mkdir "%QAHOME%\main"

rem Create a one-server QA directory (no public master list).
> "%SERVERS%" echo {"servers":[{"id":"qa-local","name":"CoD2 QA - Local","region":"local","host":"127.0.0.1","port":28963,"mode":"dm","maxPlayers":4}]}

echo [5/5] Starting ISOLATED server and web launcher...
rem Do not replace, terminate or update old preview executables.
start "CoD2 QA Server 28963" /D "%SERVERROOT%" "%QANATIVE%\cod2_win32_ded.exe" +set dedicated 1 +set net_ip 127.0.0.1 +set net_port 28963 +set net_lanauthorize 0 +set fs_basepath "%SERVERROOT%" +set fs_homepath "%QAHOME%" +exec browser-test.cfg
timeout /t 3 /nobreak >nul
start "CoD2 QA HTTP 8082" /D "%REPO%" "%PYTHON%" "%REPO%\tools\serve_web_local.py" --port 8082 --master-port 20723 --relay-port 20724 --root "%QAWEB%" --servers "%SERVERS%" --sessions
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:8082/cod2.html"
echo.
echo Open: http://127.0.0.1:8082/cod2.html
echo Your OLD preview (8081) was left untouched.
echo QA is for manual verification only; successful build != correct gameplay.
echo.
pause
exit /b 0

:fail
echo.
echo UPDATE / BUILD FAILED. Existing previews were not intentionally stopped.
echo Check the error above, do NOT delete the work folder or game assets.
pause
exit /b 1

rem --------------------------------------------------------------------------
rem Git bootstrap: do not require admin rights or a machine-wide installation.
rem The fallback is the official Git for Windows MinGit v2.56.0(3) x64 archive,
rem with its published SHA256 checked before unpacking. (~40 MB download)
rem --------------------------------------------------------------------------
:ensure_git
where git >nul 2>nul
if not errorlevel 1 (
  git --version
  exit /b 0
)
if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
if exist "%LOCALAPPDATA%\Programs\Git\cmd\git.exe" set "PATH=%LOCALAPPDATA%\Programs\Git\cmd;%PATH%"
if exist "%WORK%emsdk\git\cmd\git.exe" set "PATH=%WORK%emsdk\git\cmd;%PATH%"
if exist "%WORK%emsdk\git\bin\git.exe" set "PATH=%WORK%emsdk\git\bin;%PATH%"
where git >nul 2>nul
if not errorlevel 1 (
  git --version
  exit /b 0
)
set "COD2_MINGIT_DIR=%WORK%portable-git"
if not exist "%COD2_MINGIT_DIR%\cmd\git.exe" (
  echo [CoD2 QA] Git not found on this PC. Downloading verified portable Git...
  powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $dir=$env:COD2_MINGIT_DIR; $archive=Join-Path $env:TEMP 'cod2-mingit-2.56.0.3-x64.zip'; $url='https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.3/MinGit-2.56.0.3-64-bit.zip'; $sha='2e5619966e588fde5f8122c5557cad50c0fbafcaed736689f40ec1dfb1d78ea7'; if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha) { Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $archive }; if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha) { throw 'Portable Git SHA256 check failed' }; New-Item -ItemType Directory -Path $dir -Force | Out-Null; Expand-Archive -LiteralPath $archive -DestinationPath $dir -Force; if (!(Test-Path -LiteralPath (Join-Path $dir 'cmd\git.exe'))) { throw 'Portable Git extraction failed' }"
  if errorlevel 1 (
    echo ERROR: Portable Git download or verification failed. Do not run unknown installers.
    exit /b 1
  )
)
set "PATH=%COD2_MINGIT_DIR%\cmd;%PATH%"
where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: git.exe still missing after portable extraction.
  exit /b 1
)
git --version
exit /b 0

:ensure_build_tools
rem A normal CMake install is enough; do not download toolchains as a side effect.
where cmake >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
  if exist "%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe" set "PATH=%LOCALAPPDATA%\Programs\CMake\bin;%PATH%"
  if exist "%WORK%cmake\bin\cmake.exe" set "PATH=%WORK%cmake\bin;%PATH%"
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake is missing. Install CMake or add its bin folder to PATH.
  exit /b 1
)
where ninja >nul 2>nul
if errorlevel 1 (
  if exist "%WORK%w64devkit\bin\ninja.exe" set "PATH=%WORK%w64devkit\bin;%PATH%"
  if exist "%WORK%ninja\ninja.exe" set "PATH=%WORK%ninja;%PATH%"
)
where ninja >nul 2>nul
if errorlevel 1 (
  echo ERROR: Ninja is missing from the existing portable SDK.
  exit /b 1
)
exit /b 0

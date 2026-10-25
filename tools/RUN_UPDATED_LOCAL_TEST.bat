@echo off
setlocal EnableExtensions
title CoD2 Browser MP - verified isolated local test
rem ONE existing Git repository, no cloning, no reset, no force push.
rem Never stop existing previews 8080/8081. This QA build uses HTTP 8082.
rem Place this BAT directly in the existing "work" folder.

set "WORK=%~dp0"
set "BASE_REPO=%WORK%opencod2-web"
set "QA_SOURCE=%WORK%qa-source-worktree"
set "SOURCE=%BASE_REPO%"
set "QAWEB=%WORK%qa-build-web"
set "QANATIVE=%WORK%qa-build-win32"
set "SERVERROOT=%WORK%native-server"
set "QAHOME=%WORK%native-server-qa"
set "ASSETS=%WORK%mp-assets\main"
set "SERVERS=%WORK%cod2-qa-servers.json"
set "BRANCH=codex/browser-mp-checkpoint"
set "REMOTE_URL=https://github.com/jstondzik1996-lgtm/opencod2-web.git"
set "PYTHON=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
set "PYTHONPATH=%WORK%python-tools;%PYTHONPATH%"
set "COD2_RELAY_ALLOW_ANY="
rem The QA UDP port is selected dynamically before service startup.

echo.
echo [CoD2 QA] Existing project: "%BASE_REPO%"
echo [CoD2 QA] Old previews 8080/8081 untouched. QA ports chosen from a free range.
echo [CoD2 QA] Downloads only verified, portable developer tools.
echo.

if not exist "%BASE_REPO%\.git" (
  echo ERROR: Existing Git repository not found at "%BASE_REPO%".
  goto :fail
)
if not exist "%WORK%emsdk\emsdk_env.bat" (
  echo ERROR: The already configured work\emsdk is missing.
  goto :fail
)
if not exist "%WORK%w64devkit\bin\gcc.exe" (
  echo ERROR: The existing portable w64devkit compiler is missing.
  goto :fail
)
if not exist "%SERVERROOT%\main" (
  echo ERROR: Dedicated server root is missing: "%SERVERROOT%\main".
  goto :fail
)
if not exist "%ASSETS%" (
  echo ERROR: Existing browser assets are missing: "%ASSETS%".
  goto :fail
)
if not exist "%PYTHON%" set "PYTHON=python"

call :ensure_git
if errorlevel 1 goto :fail
call :ensure_cmake
if errorlevel 1 goto :fail
call :ensure_ninja
if errorlevel 1 goto :fail

echo.
echo [1/5] Fetching the actual project branch (no tracking-ref assumptions)...
git -C "%BASE_REPO%" fetch --no-tags "%REMOTE_URL%" "refs/heads/%BRANCH%"
if errorlevel 1 (
  echo ERROR: Could not fetch shared branch. Existing files were not reset.
  goto :fail
)
set "QA_SHA="
for /f "delims=" %%H in ('git -C "%BASE_REPO%" rev-parse --verify FETCH_HEAD') do set "QA_SHA=%%H"
if not defined QA_SHA (
  echo ERROR: Git returned no fetched commit.
  goto :fail
)
echo [CoD2 QA] Remote revision: %QA_SHA%

rem Preserve all local Work/Claude changes. Fast-forward the existing branch
rem only when it is clean AND the new commit descends from local HEAD.
set "DIRTY="
set "CURRENT="
for /f "delims=" %%I in ('git -C "%BASE_REPO%" status --porcelain --untracked-files=no') do set "DIRTY=1"
for /f "delims=" %%I in ('git -C "%BASE_REPO%" branch --show-current') do set "CURRENT=%%I"

set "USE_WORKTREE=1"
if not defined DIRTY if /I "%CURRENT%"=="%BRANCH%" (
  git -C "%BASE_REPO%" merge-base --is-ancestor HEAD "%QA_SHA%" >nul 2>nul
  if not errorlevel 1 (
    git -C "%BASE_REPO%" merge --ff-only "%QA_SHA%"
    if errorlevel 1 goto :fail
    set "USE_WORKTREE="
    set "SOURCE=%BASE_REPO%"
  )
)

if defined USE_WORKTREE (
  echo [CoD2 QA] Existing checkout has local work or diverged history.
  echo [CoD2 QA] Keeping it UNCHANGED. Preparing another WORKTREE of the same repository.
  if exist "%QA_SOURCE%\.git" (
    set "QA_DIRTY="
    for /f "delims=" %%I in ('git -C "%QA_SOURCE%" status --porcelain --untracked-files=no') do set "QA_DIRTY=1"
    if defined QA_DIRTY (
      echo ERROR: QA worktree has unsaved changes; refusing to overwrite them.
      goto :fail
    )
    git -C "%QA_SOURCE%" checkout --detach "%QA_SHA%"
    if errorlevel 1 goto :fail
  ) else (
    if exist "%QA_SOURCE%" (
      echo ERROR: QA folder exists but is not a worktree. Will not overwrite it.
      goto :fail
    )
    git -C "%BASE_REPO%" worktree add --detach "%QA_SOURCE%" "%QA_SHA%"
    if errorlevel 1 goto :fail
  )
  set "SOURCE=%QA_SOURCE%"
)
echo [CoD2 QA] Build source: "%SOURCE%"
rem Use a separate build folder per revision. Older QA servers may still be
rem serving previous builds; never overwrite assets they are actively serving.
set "QAWEB=%WORK%qa-build-web-%QA_SHA:~0,10%"
set "QANATIVE=%WORK%qa-build-win32-%QA_SHA:~0,10%"

echo.
echo [2/5] Loading existing emsdk and compiler...
set "PATH=%WORK%w64devkit\bin;%PATH%"
call "%WORK%emsdk\emsdk_env.bat" >nul
if errorlevel 1 goto :fail
where cmake >nul 2>nul
if errorlevel 1 (echo ERROR: CMake was lost after emsdk setup. & goto :fail)
where ninja >nul 2>nul
if errorlevel 1 (echo ERROR: Ninja was lost after emsdk setup. & goto :fail)
where emcmake >nul 2>nul
if errorlevel 1 (echo ERROR: Emscripten emcmake not available. & goto :fail)

echo.
echo [3/5] Building separate QA WebAssembly and dedicated server...
call emcmake cmake -S "%SOURCE%" -B "%QAWEB%" -G Ninja -DCMAKE_OBJECT_PATH_MAX=200 -DCOD2_WEB_SAFE_HEAP=ON -DCOD2_WEB_TESTING=ON
if errorlevel 1 goto :fail
cmake --build "%QAWEB%" -j 6
if errorlevel 1 goto :fail
cmake -S "%SOURCE%" -B "%QANATIVE%" -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=gcc -DCMAKE_ASM_COMPILER=gcc -DCMAKE_RC_COMPILER=windres
if errorlevel 1 goto :fail
cmake --build "%QANATIVE%" -j 6
if errorlevel 1 goto :fail

if not exist "%QAWEB%\cod2.html" (echo ERROR: Missing cod2.html. & goto :fail)
if not exist "%QAWEB%\cod2.wasm" (echo ERROR: Missing cod2.wasm. & goto :fail)
if not exist "%QANATIVE%\cod2_win32_ded.exe" (echo ERROR: Missing Win32 dedicated exe. & goto :fail)

echo.
echo [4/5] Linking your existing local game assets...
if not exist "%QAWEB%\main\" (
  mklink /J "%QAWEB%\main" "%ASSETS%"
  if errorlevel 1 goto :fail
)
if exist "%BASE_REPO%\build\web\assets.json" copy /Y "%BASE_REPO%\build\web\assets.json" "%QAWEB%\assets.json" >nul
if exist "%BASE_REPO%\build\web\zone" if not exist "%QAWEB%\zone" (
  mklink /J "%QAWEB%\zone" "%BASE_REPO%\build\web\zone"
)
if not exist "%QAWEB%\assets.json" echo WARNING: assets.json is missing; file picker may be required on the new port.
echo.
echo [5/5] Finding unused QA ports (avoiding the old running 8082)...
set "QA_PORT_SET="
for /f "delims=" %%P in ('powershell -NoProfile -NonInteractive -Command "$ip=[Net.IPAddress]::Loopback; foreach($i in 0..15){ $p=@((8082+$i),(20723+2*$i),(20724+2*$i)); $u=28963+$i; $ls=@(); $udp=$null; $ok=$true; try { foreach($n in $p){$t=[Net.Sockets.TcpListener]::new($ip,$n); $t.Start(); $ls+=,$t }; $udp=[Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new($ip,$u)) } catch { $ok=$false } finally { if($udp){$udp.Dispose()}; foreach($t in $ls){$t.Stop()} }; if($ok){Write-Output ($p[0].ToString()+','+$p[1]+','+$p[2]+','+$u); break} }"') do set "QA_PORT_SET=%%P"
if not defined QA_PORT_SET (
  echo ERROR: All QA port sets are occupied. Close old QA windows and retry.
  goto :fail
)
for /f "tokens=1-4 delims=," %%A in ("%QA_PORT_SET%") do (
  set "QA_HTTP_PORT=%%A"
  set "QA_MASTER_PORT=%%B"
  set "QA_RELAY_PORT=%%C"
  set "QA_GAME_PORT=%%D"
)
set "QAHOME=%WORK%native-server-qa-%QA_GAME_PORT%"
set "SERVERS=%WORK%cod2-qa-servers-%QA_GAME_PORT%.json"
set "COD2_PORT_MIN=%QA_GAME_PORT%"
set "COD2_PORT_MAX=%QA_GAME_PORT%"
echo [CoD2 QA] Using HTTP %QA_HTTP_PORT%, UDP %QA_GAME_PORT%.
if not exist "%QAHOME%\main" mkdir "%QAHOME%\main"
> "%SERVERS%" echo {"servers":[{"id":"qa-local","name":"CoD2 QA Local","region":"local","host":"127.0.0.1","port":%QA_GAME_PORT%,"mode":"dm","maxPlayers":4}]}

echo.
echo [CoD2 QA] Starting isolated QA services on selected ports...
start "CoD2 QA Server %QA_GAME_PORT%" /D "%SERVERROOT%" "%QANATIVE%\cod2_win32_ded.exe" +set dedicated 1 +set net_ip 127.0.0.1 +set net_port %QA_GAME_PORT% +set net_lanauthorize 0 +set fs_basepath "%SERVERROOT%" +set fs_homepath "%QAHOME%" +exec browser-test.cfg
timeout /t 3 /nobreak >nul
start "CoD2 QA HTTP %QA_HTTP_PORT%" /D "%SOURCE%" "%PYTHON%" "%SOURCE%\tools\serve_web_local.py" --port %QA_HTTP_PORT% --master-port %QA_MASTER_PORT% --relay-port %QA_RELAY_PORT% --root "%QAWEB%" --servers "%SERVERS%" --sessions
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:%QA_HTTP_PORT%/cod2.html"
echo.
echo [CoD2 QA] QA URL: http://127.0.0.1:%QA_HTTP_PORT%/cod2.html
echo [CoD2 QA] Old preview on port 8081 was NOT touched.
echo [CoD2 QA] A successful build alone does not prove correct gameplay.
pause
exit /b 0

:fail
echo.
echo [CoD2 QA] ERROR. The existing preview was not intentionally stopped.
echo Please capture the FIRST error displayed above; keep your game assets.
pause
exit /b 1

:ensure_git
where git >nul 2>nul
if not errorlevel 1 exit /b 0
if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
if exist "%LOCALAPPDATA%\Programs\Git\cmd\git.exe" set "PATH=%LOCALAPPDATA%\Programs\Git\cmd;%PATH%"
if exist "%WORK%portable-git\cmd\git.exe" set "PATH=%WORK%portable-git\cmd;%PATH%"
where git >nul 2>nul
if not errorlevel 1 (git --version & exit /b 0)
echo [CoD2 QA] Downloading official verified portable Git...
set "COD2_MINGIT_DIR=%WORK%portable-git"
powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $dir=$env:COD2_MINGIT_DIR; $zip=Join-Path $env:TEMP 'cod2-Mingit-2.56.0.3.zip'; $url='https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.3/MinGit-2.56.0.3-64-bit.zip'; $hash='2e5619966e588fde5f8122c5557cad50c0fbafcaed736689f40ec1dfb1d78ea7'; if (!(Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $zip }; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { throw 'MinGit checksum mismatch' }; New-Item -ItemType Directory -Path $dir -Force | Out-Null; Expand-Archive -Path $zip -DestinationPath $dir -Force; if (!(Test-Path (Join-Path $dir 'cmd\git.exe'))) { throw 'git.exe absent after extraction' }"
if errorlevel 1 exit /b 1
set "PATH=%COD2_MINGIT_DIR%\cmd;%PATH%"
where git >nul 2>nul
if errorlevel 1 exit /b 1
git --version
exit /b 0

:ensure_cmake
where cmake >nul 2>nul
if not errorlevel 1 exit /b 0
set "COD2_CMAKE_BIN=%WORK%portable-cmake\cmake-4.4.4-windows-x86_64\bin"
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
if exist "%WORK%cmake\bin\cmake.exe" set "PATH=%WORK%cmake\bin;%PATH%"
if exist "%COD2_CMAKE_BIN%\cmake.exe" set "PATH=%COD2_CMAKE_BIN%;%PATH%"
where cmake >nul 2>nul
if not errorlevel 1 (cmake --version & exit /b 0)
echo [CoD2 QA] Downloading verified portable CMake v4.4.4 (54 MB)...
set "COD2_CMAKE_DIR=%WORK%portable-cmake"
powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $dir=$env:COD2_CMAKE_DIR; $zip=Join-Path $env:TEMP 'cod2-cmake-4.4.4-x64.zip'; $url='https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'; $hash='bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'; if (!(Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $zip }; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { throw 'CMake checksum mismatch' }; New-Item -ItemType Directory -Path $dir -Force | Out-Null; Expand-Archive -Path $zip -DestinationPath $dir -Force; if (!(Test-Path (Join-Path $dir 'cmake-4.4.4-windows-x86_64\bin\cmake.exe'))) { throw 'cmake.exe absent after extraction' }"
if errorlevel 1 exit /b 1
set "PATH=%COD2_CMAKE_BIN%;%PATH%"
where cmake >nul 2>nul
if errorlevel 1 exit /b 1
cmake --version
exit /b 0

:ensure_ninja
where ninja >nul 2>nul
if not errorlevel 1 exit /b 0
set "COD2_NINJA_DIR=%WORK%portable-ninja"
if exist "%WORK%w64devkit\bin\ninja.exe" set "PATH=%WORK%w64devkit\bin;%PATH%"
if exist "%COD2_NINJA_DIR%\ninja.exe" set "PATH=%COD2_NINJA_DIR%;%PATH%"
where ninja >nul 2>nul
if not errorlevel 1 (ninja --version & exit /b 0)
echo [CoD2 QA] Downloading verified portable Ninja v1.13.2...
powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $dir=$env:COD2_NINJA_DIR; $zip=Join-Path $env:TEMP 'cod2-ninja-1.13.2-win.zip'; $url='https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'; $hash='07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'; if (!(Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $zip }; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $hash) { throw 'Ninja checksum mismatch' }; New-Item -ItemType Directory -Path $dir -Force | Out-Null; Expand-Archive -Path $zip -DestinationPath $dir -Force; if (!(Test-Path (Join-Path $dir 'ninja.exe'))) { throw 'ninja.exe absent after extraction' }"
if errorlevel 1 exit /b 1
set "PATH=%COD2_NINJA_DIR%;%PATH%"
where ninja >nul 2>nul
if errorlevel 1 exit /b 1
ninja --version
exit /b 0

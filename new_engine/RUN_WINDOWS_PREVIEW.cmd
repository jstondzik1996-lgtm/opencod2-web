@echo off
setlocal EnableExtensions
title WWII Browser Multiplayer - NEW ENGINE - Windows local test
cd /d "%~dp0"

echo.
echo ===========================================================
echo  WWII Browser Multiplayer - Jules PlayCanvas prototype
echo  Local test: 1 human + 15 MOVING bots (NOT finished AI)
echo ===========================================================
echo.
echo This test does not overwrite the original CoD2 engine.
echo New client preview: http://127.0.0.1:8090
echo New game server:   ws://127.0.0.1:2567
echo.

where node >nul 2>nul
if errorlevel 1 (
    echo ERROR: Node.js is not installed or not in PATH.
    echo Install official Node.js LTS from https://nodejs.org
    echo OR in a new PowerShell window: winget install OpenJS.NodeJS.LTS
    echo Reopen this CMD file after installing Node.js.
    goto :fail
)
where npm >nul 2>nul
if errorlevel 1 (
    echo ERROR: npm is unavailable. Install official Node.js LTS from https://nodejs.org
    goto :fail
)
echo Node:
node --version
echo npm:
call npm --version
echo.

if not exist "server\package.json" (
    echo ERROR: server\package.json not found. This must run from new_engine.
    goto :fail
)
if not exist "client\package.json" (
    echo ERROR: client\package.json not found. This must run from new_engine.
    goto :fail
)
if not exist "client\serve.js" (
    echo ERROR: client\serve.js not found. Refresh/download latest Jules branch.
    goto :fail
)

set "PUPPETEER_SKIP_DOWNLOAD=1"
echo [1/5] Install server dependencies locally (first run can take longer).
pushd "server"
call npm ci --no-audit --no-fund
if errorlevel 1 goto :fail_popd
echo [2/5] Compile server (TypeScript).
call npm run build
if errorlevel 1 goto :fail_popd
popd

echo [3/5] Install browser client dependencies locally.
pushd "client"
call npm ci --no-audit --no-fund
if errorlevel 1 goto :fail_popd
echo [4/5] Build browser client (webpack).
call npm run build
if errorlevel 1 goto :fail_popd
popd

echo [5/5] Start TWO local terminal windows.
echo     Do not close these windows while testing.
start "WWII Game Server (2567)" /D "%~dp0server" cmd /k "node dist\index.js"
start "WWII Client Preview (8090)" /D "%~dp0client" cmd /k "node serve.js"
timeout /t 4 /nobreak >nul
echo.
echo Opening browser at http://127.0.0.1:8090
start "" "http://127.0.0.1:8090"
echo.
echo Test controls in current prototype: WASD, mouse look, left click shoot.
echo Bot models currently are simple cylinders; bot AI is random movement ONLY.
echo Keep both command windows open. If screen is blank, capture their errors
echo and the browser devtools console (F12), then report to the developer.
echo.
echo This script only starts LOCAL test processes on ports 8090 and 2567.
echo It does not publish the game or merge the pull request.
pause
exit /b 0

:fail_popd
popd
:fail
echo.
echo The build/start was NOT completed. Capture the FIRST ERROR above.
pause
exit /b 1

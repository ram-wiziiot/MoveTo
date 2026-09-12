@echo off
REM Registers the 64-bit MoveTo shell extension. Must run elevated.
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo This must be run as Administrator.
    echo Right-click install.cmd and choose "Run as administrator".
    pause
    exit /b 1
)

set "DLL=%~dp0..\bin\x64\Release\MoveTo.dll"

if not exist "%DLL%" (
    echo Could not find:
    echo     %DLL%
    echo.
    echo Build it first:
    echo     msbuild MoveTo.sln /p:Configuration=Release /p:Platform=x64
    pause
    exit /b 1
)

regsvr32 "%DLL%"
if errorlevel 1 (
    echo Registration failed.
    pause
    exit /b 1
)

echo.
echo Registered. Restart Explorer so it picks up the change:
echo     taskkill /f /im explorer.exe ^&^& start explorer.exe
echo.
echo The menu item appears under "Show more options" (Shift+F10)
echo when you right-click a folder.
pause

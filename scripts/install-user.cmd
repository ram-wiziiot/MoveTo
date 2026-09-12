@echo off
REM Registers MoveTo for the CURRENT USER only. No administrator rights needed.
setlocal

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

REM /n suppresses DllRegisterServer; /i:user reaches DllInstall, which is
REM the only supported way to register this DLL under HKCU.
regsvr32 /n /i:user "%DLL%"
if errorlevel 1 (
    echo Registration failed.
    pause
    exit /b 1
)

echo.
echo Registered for %USERNAME%. Restart Explorer to pick it up:
echo     taskkill /f /im explorer.exe ^&^& start explorer.exe
echo.
echo Right-click a file or folder, then "Show more options" (Shift+F10).
pause

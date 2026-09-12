@echo off
REM Unregisters the MoveTo shell extension. Must run elevated.
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo This must be run as Administrator.
    echo Right-click uninstall.cmd and choose "Run as administrator".
    pause
    exit /b 1
)

set "DLL=%~dp0..\bin\x64\Release\MoveTo.dll"

if not exist "%DLL%" (
    echo Could not find:
    echo     %DLL%
    echo.
    echo Unregistering needs the same DLL that was registered.
    pause
    exit /b 1
)

regsvr32 /u "%DLL%"

echo.
echo Unregistered. Restart Explorer to release the DLL:
echo     taskkill /f /im explorer.exe ^&^& start explorer.exe
echo.
echo Your wildcard list is left in the registry at
echo     HKCU\Software\Wiziiot\MoveTo
pause

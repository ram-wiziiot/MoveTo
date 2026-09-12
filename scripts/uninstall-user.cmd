@echo off
REM Unregisters MoveTo for the current user.
setlocal

set "DLL=%~dp0..\bin\x64\Release\MoveTo.dll"

if not exist "%DLL%" (
    echo Could not find: %DLL%
    echo Unregistering needs the same DLL that was registered.
    pause
    exit /b 1
)

regsvr32 /n /u /i:user "%DLL%"

echo.
echo Unregistered. Restart Explorer to release the DLL:
echo     taskkill /f /im explorer.exe ^&^& start explorer.exe
echo.
echo Your destination list is left at
echo     HKCU\Software\Wiziiot\MoveTo
pause

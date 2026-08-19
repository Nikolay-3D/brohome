@echo off
setlocal
cd /d "%~dp0"

echo BroHome ESP32-S3 flashing wizard
echo =================================
echo.
echo This wizard will ask for Wi-Fi settings and the BroHome server IP.
echo It will compile and upload the firmware without a full flash erase.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\flash-esp32.ps1"
set "wizard_exit=%errorlevel%"

echo.
if "%wizard_exit%"=="0" (
  echo BroHome flashing wizard completed successfully.
) else (
  echo BroHome flashing wizard stopped with an error.
  echo Read the error above and OPEN-ME-FIRST.txt, then try again.
)
echo.
pause
exit /b %wizard_exit%

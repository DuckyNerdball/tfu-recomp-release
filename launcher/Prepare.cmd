@echo off
rem RecompLauncher setup step: windowless, prints "progress <0-1> <text>" lines.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-TFU.ps1" %*
exit /b %ERRORLEVEL%

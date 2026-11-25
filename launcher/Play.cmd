@echo off
setlocal
set "SRC=%~dp0src"
if exist "%~dp0tfu_src.txt" set /p SRC=<"%~dp0tfu_src.txt"
if not exist "%SRC%\tools\run.ps1" (
  echo The game is not built yet. Run its setup in RecompLauncher, or run Prepare.cmd.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\tools\run.ps1" %*
exit /b %ERRORLEVEL%

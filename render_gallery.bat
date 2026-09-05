@ECHO OFF
SETLOCAL

REM Regenerate the versioned gallery baselines. Every render is compared
REM against its committed baseline before the baseline is replaced; see
REM scripts\render_gallery.ps1 and docs\lessons-from-hdcodex.md R9.

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

PowerShell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\render_gallery.ps1" %*
EXIT /B %ERRORLEVEL%

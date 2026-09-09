@ECHO OFF
SETLOCAL

REM Open a stage in usdview with the hdClaude delegate selected.

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "SCENE_FILE=%~1"
IF "%SCENE_FILE%"=="" SET "SCENE_FILE=C:\Users\paolo\Desktop\openusd\ALab-2.3.0\ALab\entity\lab_structure01\lab_structure01.usda"

IF NOT EXIST "%SCENE_FILE%" (
  ECHO [hdClaude] Stage not found: %SCENE_FILE%
  EXIT /B 1
)

set HDCLAUDE_FRAME_LOG=.\debugging_frames.log
set HDCLAUDE_STATS_REPORT=.\debugging_session.stats
set HDCLAUDE_SAMPLES_PER_PIXEL 4096

ECHO [hdClaude] usdview: %SCENE_FILE%
CALL usdview "%SCENE_FILE%" --renderer "Claude GPU Path Tracer"
EXIT /B %ERRORLEVEL%

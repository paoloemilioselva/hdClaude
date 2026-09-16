@ECHO OFF
SETLOCAL

REM Open a stage in usdview with the hdClaude delegate selected.

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "SCENE_FILE=%~1"
IF "%SCENE_FILE%"=="" SET "SCENE_FILE=%~dp0gallery\shader_ball_gold.usda"

IF NOT EXIST "%SCENE_FILE%" (
  ECHO [hdClaude] Stage not found: %SCENE_FILE%
  EXIT /B 1
)

SET HDCLAUDE_SAMPLES_PER_PIXEL=1024
SET HDCLAUDE_MAX_BOUNCES=32
SET HDCLAUDE_LIGHT_GEOMETRY=on
SET HDCLAUDE_CURVE_GEOMETRY=implicit

cmd

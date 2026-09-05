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

ECHO [hdClaude] usdview: %SCENE_FILE%
CALL usdview "%SCENE_FILE%" --renderer "Claude GPU Path Tracer"
EXIT /B %ERRORLEVEL%

@ECHO OFF
SETLOCAL

REM Render with the hdClaude delegate. Accepts normal usdrecord arguments.
REM Camera lighting is always disabled so authored or fallback lighting is what
REM gets tested -- a default headlight would mask every lighting defect.

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

IF "%~1"=="" (
  ECHO Usage: render_claude.bat [usdrecord options] ^<scene.usd^> ^<output.exr^>
  ECHO.
  ECHO Settings are taken from the environment:
  ECHO   HDCLAUDE_SAMPLES_PER_PIXEL   1-4096   default 128
  ECHO   HDCLAUDE_SAMPLES_PER_UPDATE  1-64     default 8
  ECHO   HDCLAUDE_MAX_BOUNCES         1-16     default 8
  ECHO   HDCLAUDE_ENABLE_SUBDIVISION  0 or 1   default 1
  ECHO   HDCLAUDE_SUBDIVISION_LEVEL   0-8      default 2
  ECHO   HDCLAUDE_ENABLE_DISPLACEMENT 0 or 1   default 1
  EXIT /B 2
)

CALL usdrecord --renderer "Claude GPU Path Tracer" --disableCameraLight %*
EXIT /B %ERRORLEVEL%

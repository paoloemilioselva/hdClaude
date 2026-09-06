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
  ECHO Settings are taken from the environment. These are the names the
  ECHO delegate reads; see src/hydra/render_delegate.cpp.
  ECHO   HDCLAUDE_SAMPLES_PER_PIXEL     default 64    samples in the image
  ECHO   HDCLAUDE_SAMPLES_PER_FRAME     default 4     samples per progressive update
  ECHO   HDCLAUDE_MAX_BOUNCES           default 8     path length
  ECHO   HDCLAUDE_SUBDIVISION_LEVEL     0-6, default 2, 0 disables refinement
  ECHO   HDCLAUDE_EXPOSURE              default 0     stops, applied after resolve
  ECHO   HDCLAUDE_ENVIRONMENT_INTENSITY default 1     stand-in sky, unlit stages
  ECHO   HDCLAUDE_SUN_INTENSITY         default 1     stand-in sun, unlit stages
  ECHO   HDCLAUDE_DEVICE                substring of the GPU to select
  EXIT /B 2
)

CALL usdrecord --renderer "Claude GPU Path Tracer" --disableCameraLight %*
EXIT /B %ERRORLEVEL%

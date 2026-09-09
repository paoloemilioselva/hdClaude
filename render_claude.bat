@ECHO OFF
SETLOCAL

REM Render with the hdClaude delegate. Accepts normal usdrecord arguments.
REM Camera lighting is always disabled so authored or fallback lighting is what
REM gets tested -- a default headlight would mask every lighting defect.
REM
REM The render purpose is always requested, and this is not a preference. A
REM production asset puts its real geometry under purpose="render" and keeps a
REM cheap stand-in under purpose="proxy", and usdrecord renders the "default"
REM purpose unless told otherwise -- so the default is not a subset of the right
REM answer, it is the wrong half of the stage. ALab's lab_structure01 has 36
REM render meshes and a 1636-instance PointInstancer against 2 proxy meshes:
REM without this flag it reports 25 instances and 304,698 triangles, with it
REM 2,074 and 87,749,024. The first renders in 47 seconds, succeeds, and is a
REM picture of almost nothing, which is the failure worth guarding against --
REM nothing errors and the image looks like an image. A measurement taken
REM without it describes a different scene by two orders of magnitude.
REM
REM Colour correction is always disabled for the same class of reason. The
REM delegate's AOV is scene-linear and the output must stay that way: the sRGB
REM transform belongs to the EXR-to-JPEG conversion, which is what
REM hdClaudeDisplayTransform does, and nowhere else. Letting usdrecord encode
REM the AOV instead applies a transfer function to unclamped linear radiance,
REM and raising a negative sample to a fractional power produces a NaN -- which
REM is exactly how a clean render came to look like a renderer emitting
REM thousands of non-finite pixels.

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
  ECHO.
  ECHO Diagnostics. Off unless set, and none of them changes an image.
  ECHO   HDCLAUDE_REPEAT_RENDERS        render the finished image n more
  ECHO                                  times in the same process, reporting
  ECHO                                  each render with its ray counts and
  ECHO                                  its hit and ray hashes
  ECHO   HDCLAUDE_POISON_PATH_STATE     fill every path buffer with a known
  ECHO                                  pattern before a frame
  ECHO   HDCLAUDE_STATS_REPORT          write the render stats to this path
  EXIT /B 2
)

CALL usdrecord --renderer "Claude GPU Path Tracer" --disableCameraLight --colorCorrectionMode disabled --purposes render %*
EXIT /B %ERRORLEVEL%

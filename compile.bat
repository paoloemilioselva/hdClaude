@ECHO OFF
SETLOCAL

REM ===========================================================================
REM hdClaude - configure, build, and install the delegate.
REM
REM Usage:
REM   compile.bat              build the full delegate (preset "dev")
REM   compile.bat core-only    build only the dependency-free core and its tests
REM   compile.bat dev-dlss     build with the optional NVIDIA DLSS backend
REM   compile.bat clean        remove the build tree, then build "dev"
REM
REM Third-party dependencies are fetched by CMake into the gitignored _deps
REM tree on the first configure. Nothing needs to be installed by hand; see
REM cmake\Dependencies.cmake for the pinned versions.
REM ===========================================================================

CALL "%~dp0setup_usd_env.bat"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

SET "HDCLAUDE_PRESET=%~1"
IF "%HDCLAUDE_PRESET%"=="" SET "HDCLAUDE_PRESET=dev"

IF /I "%HDCLAUDE_PRESET%"=="clean" (
  ECHO [hdClaude] Removing build tree.
  IF EXIST "%~dp0build" RMDIR /S /Q "%~dp0build"
  SET "HDCLAUDE_PRESET=dev"
)

REM ---------------------------------------------------------------------------
REM Toolchain. Enter the Visual Studio developer environment only if the tools
REM are not already on PATH, so running from a developer prompt stays fast.
REM ---------------------------------------------------------------------------
SET "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

WHERE cl.exe >NUL 2>NUL
IF ERRORLEVEL 1 GOTO setup_msvc
WHERE cmake.exe >NUL 2>NUL
IF ERRORLEVEL 1 GOTO setup_msvc
WHERE ninja.exe >NUL 2>NUL
IF NOT ERRORLEVEL 1 GOTO toolchain_ready

:setup_msvc
IF NOT EXIST "%VSDEVCMD%" (
  ECHO [hdClaude] MSVC, CMake, and Ninja were not found, and the Visual Studio
  ECHO            2022 developer environment is unavailable at:
  ECHO            %VSDEVCMD%
  ECHO            See docs\building.md.
  EXIT /B 1
)
CALL "%VSDEVCMD%" -arch=x64 -host_arch=x64 >NUL
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

:toolchain_ready
ECHO [hdClaude] Configuring preset "%HDCLAUDE_PRESET%"...
cmake --preset "%HDCLAUDE_PRESET%" -DHDCLAUDE_OPENUSD_ROOT="%USDROOT%" -DCMAKE_INSTALL_PREFIX="%USDEXTRA%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

ECHO [hdClaude] Building...
cmake --build --preset "%HDCLAUDE_PRESET%"
IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%

REM The core-only preset builds no installable artifact; it exists to run the
REM host-independent tests.
IF /I NOT "%HDCLAUDE_PRESET%"=="core-only" (
  ECHO [hdClaude] Installing to %USDEXTRA%...
  cmake --build "%~dp0build\%HDCLAUDE_PRESET%" --target install
  IF ERRORLEVEL 1 EXIT /B %ERRORLEVEL%
)

ECHO [hdClaude] Build completed: preset "%HDCLAUDE_PRESET%".
EXIT /B 0

@ECHO OFF
REM ===========================================================================
REM hdClaude - shared standalone OpenUSD runtime environment.
REM
REM Every USD-facing script in this repository calls this file. It is the one
REM place that knows where OpenUSD, the Vulkan SDK, and optional SDKs live.
REM
REM This script is CALLed, not run in a subshell, so it deliberately does not
REM SETLOCAL: the variables it sets must survive into the caller.
REM
REM Overrides, all optional, all honoured if already set:
REM   USDROOT                 OpenUSD install root
REM   USDEXTRA                install prefix for hdClaude's own plugin
REM   HDCLAUDE_VALIDATION_LAYER_DIR  directory holding the validation layer
REM   HDCLAUDE_DLSS_SDK       NVIDIA DLSS SDK root
REM   HDCLAUDE_SKIP_PYTHON    set to 1 to skip the Python conformance check
REM ===========================================================================

SET "HDCLAUDE_ROOT=%~dp0"
IF "%HDCLAUDE_ROOT:~-1%"=="\" SET "HDCLAUDE_ROOT=%HDCLAUDE_ROOT:~0,-1%"

REM ---------------------------------------------------------------------------
REM 1. Locate OpenUSD.
REM     An explicit USDROOT always wins. Otherwise probe known-good roots,
REM     newest first, and validate by the presence of pxrConfig.cmake.
REM ---------------------------------------------------------------------------
IF DEFINED USDROOT GOTO have_usdroot
FOR %%R IN (
  "C:\dev\usd-26.03"
  "C:\dev\usd-26.05"
  "C:\dev\usd-25.08"
) DO IF NOT DEFINED USDROOT IF EXIST "%%~R\pxrConfig.cmake" SET "USDROOT=%%~R"

:have_usdroot
IF NOT DEFINED USDROOT (
  ECHO [hdClaude] OpenUSD was not found.
  ECHO            Set USDROOT to an OpenUSD install root containing pxrConfig.cmake,
  ECHO            or install one under C:\dev. See docs\building.md.
  EXIT /B 1
)
IF NOT EXIST "%USDROOT%\pxrConfig.cmake" (
  ECHO [hdClaude] USDROOT does not look like an OpenUSD install: %USDROOT%
  ECHO            Expected "%USDROOT%\pxrConfig.cmake" to exist.
  EXIT /B 1
)

REM Install prefix for the hdClaude plugin itself. Kept beside the USD install
REM rather than inside it so a USD upgrade does not delete the plugin, and a
REM plugin rebuild cannot corrupt a USD distribution.
IF NOT DEFINED USDEXTRA SET "USDEXTRA=%USERPROFILE%\Desktop\usd-26.03-extra"

REM ---------------------------------------------------------------------------
REM 2. Python conformance.
REM     OpenUSD's Python bindings are ABI-locked to the interpreter the
REM     distribution was built against. pxrConfig.cmake records that exact
REM     interpreter, so the requirement is derived rather than hard-coded, and
REM     it stays correct when USDROOT changes.
REM
REM     The active environment's Python is used as-is. This script never
REM     selects, installs, or provisions an interpreter.
REM ---------------------------------------------------------------------------
IF "%HDCLAUDE_SKIP_PYTHON%"=="1" GOTO python_done

SET "USD_PYTHON_RECORDED="
FOR /F "usebackq tokens=2 delims=[]" %%A IN (
  `FINDSTR /C:"set(Python3_EXECUTABLE [[" "%USDROOT%\pxrConfig.cmake"`
) DO IF NOT DEFINED USD_PYTHON_RECORDED SET "USD_PYTHON_RECORDED=%%A"

SET "USD_PYTHON_REQUIRED="
IF DEFINED USD_PYTHON_RECORDED IF EXIST "%USD_PYTHON_RECORDED%" (
  FOR /F "usebackq delims=" %%V IN (
    `"%USD_PYTHON_RECORDED%" -c "import sys;print('%%d.%%d'%%sys.version_info[:2])" 2^>NUL`
  ) DO SET "USD_PYTHON_REQUIRED=%%V"
)
REM Fall back to the version OpenUSD 26.03 ships against when the recorded
REM interpreter has been moved or removed.
IF NOT DEFINED USD_PYTHON_REQUIRED SET "USD_PYTHON_REQUIRED=3.12"

WHERE python.exe >NUL 2>NUL
IF ERRORLEVEL 1 (
  ECHO [hdClaude] Python was not found in the active environment.
  ECHO            OpenUSD at %USDROOT% requires Python %USD_PYTHON_REQUIRED%.
  EXIT /B 1
)

FOR /F "usebackq delims=" %%V IN (
  `python -c "import sys;print('%%d.%%d'%%sys.version_info[:2])" 2^>NUL`
) DO SET "USD_PYTHON_ACTIVE=%%V"

IF NOT "%USD_PYTHON_ACTIVE%"=="%USD_PYTHON_REQUIRED%" (
  ECHO [hdClaude] Python version mismatch.
  ECHO            Active:   %USD_PYTHON_ACTIVE%
  ECHO            Required: %USD_PYTHON_REQUIRED%  ^(the interpreter OpenUSD was built against^)
  IF DEFINED USD_PYTHON_RECORDED ECHO            Recorded: %USD_PYTHON_RECORDED%
  ECHO            Activate a matching environment, or set HDCLAUDE_SKIP_PYTHON=1
  ECHO            for builds and renders that do not use the Python bindings.
  EXIT /B 1
)

:python_done

REM ---------------------------------------------------------------------------
REM 3. OpenUSD runtime paths.
REM     The hdClaude install prefix is placed ahead of nothing and behind
REM     nothing that matters: plugin discovery is a union, and both trees are
REM     required for usdview/usdrecord to find the delegate.
REM ---------------------------------------------------------------------------
SET "PXR_PLUGINPATH_NAME=%USDROOT%;%USDROOT%\plugin\usd;%USDEXTRA%\plugin\usd"
SET "PYTHONPATH=%USDROOT%\lib\python;%USDEXTRA%\lib\python;%PYTHONPATH%"
SET "PATH=%USDROOT%\bin;%USDEXTRA%\bin;%USDROOT%\lib;%USDEXTRA%\lib;%PATH%"

REM ---------------------------------------------------------------------------
REM 4. Vulkan validation layer.
REM     Validation is a gate, not a convenience: the GPU tests fail without it
REM     (docs\lessons-from-hdcodex.md R8). The layer is built from source into
REM     _deps by CMake and is never committed; see cmake\ValidationLayers.cmake.
REM     A source-built or copy-only layer has no system registration, so
REM     VK_LAYER_PATH is set explicitly.
REM
REM     Search order: an explicit override, the source-built layer, a
REM     copy-only SDK under _deps, then a system SDK.
REM ---------------------------------------------------------------------------
IF DEFINED HDCLAUDE_VALIDATION_LAYER_DIR GOTO have_validation_layer

IF EXIST "%HDCLAUDE_ROOT%\_deps\validation-layers\install\bin\VkLayer_khronos_validation.json" (
  SET "HDCLAUDE_VALIDATION_LAYER_DIR=%HDCLAUDE_ROOT%\_deps\validation-layers\install\bin"
  GOTO have_validation_layer
)

FOR /D %%D IN ("%HDCLAUDE_ROOT%\_deps\vulkan-sdk-*") DO (
  IF EXIST "%%~D\Bin\VkLayer_khronos_validation.json" (
    SET "HDCLAUDE_VULKAN_SDK=%%~D"
    SET "HDCLAUDE_VALIDATION_LAYER_DIR=%%~D\Bin"
  )
)
IF NOT DEFINED HDCLAUDE_VALIDATION_LAYER_DIR IF DEFINED VULKAN_SDK (
  IF EXIST "%VULKAN_SDK%\Bin\VkLayer_khronos_validation.json" (
    SET "HDCLAUDE_VULKAN_SDK=%VULKAN_SDK%"
    SET "HDCLAUDE_VALIDATION_LAYER_DIR=%VULKAN_SDK%\Bin"
  )
)

:have_validation_layer
IF DEFINED HDCLAUDE_VALIDATION_LAYER_DIR IF EXIST "%HDCLAUDE_VALIDATION_LAYER_DIR%\VkLayer_khronos_validation.json" (
  SET "VK_LAYER_PATH=%HDCLAUDE_VALIDATION_LAYER_DIR%;%VK_LAYER_PATH%"
  SET "PATH=%HDCLAUDE_VALIDATION_LAYER_DIR%;%PATH%"
)

REM ---------------------------------------------------------------------------
REM 5. NVIDIA DLSS SDK - optional, proprietary, never committed.
REM     Nothing is set here. The SDK is fetched at the tag cmake\NvidiaDLSS.cmake
REM     pins; HDCLAUDE_DLSS_SDK is for pointing at a *different* SDK by hand.
REM     This used to set it to _deps\dlss-src whenever that existed, which is
REM     the fetch's own checkout: CMake then took it for a hand-supplied SDK,
REM     stopped updating it, and a changed pin built the old SDK silently.
REM ---------------------------------------------------------------------------

REM ---------------------------------------------------------------------------
REM 6. RenderMan - optional. This OpenUSD build includes the RenderMan OSL
REM     parser; make its runtime visible when a matching install is present.
REM     Non-RenderMan setups are unaffected.
REM ---------------------------------------------------------------------------
SET "HDCLAUDE_RMAN=C:\Program Files\Pixar\RenderManProServer-26.3"
IF DEFINED RMANTREE IF EXIST "%RMANTREE%\lib\libprman.dll" SET "HDCLAUDE_RMAN=%RMANTREE%"
IF EXIST "%HDCLAUDE_RMAN%\lib\libprman.dll" SET "PATH=%HDCLAUDE_RMAN%\bin;%HDCLAUDE_RMAN%\lib;%PATH%"

REM ---------------------------------------------------------------------------
REM 7. hdClaude runtime defaults. Only set what is not already set, so a
REM     caller's explicit choice always wins.
REM ---------------------------------------------------------------------------
IF NOT DEFINED HDCLAUDE_SHADER_CACHE SET "HDCLAUDE_SHADER_CACHE=%HDCLAUDE_ROOT%\shader-cache"
IF NOT DEFINED MATERIALX_SEARCH_PATH SET "MATERIALX_SEARCH_PATH=%HDCLAUDE_ROOT%\mtlx;%USDROOT%\libraries"

EXIT /B 0

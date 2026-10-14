@ECHO OFF
REM ===========================================================================
REM hdClaude - configure, build, and install the delegate.
REM
REM Usage:
REM   compile.bat              build the full delegate (preset "dev")
REM   compile.bat core-only    build only the dependency-free core and its tests
REM   compile.bat dev-dlss     build with the optional NVIDIA DLSS backend
REM   compile.bat clean        remove the build tree, then build "dev"
REM
REM The build itself is compile.py, the one driver on every platform; this
REM forwards to it so the familiar command keeps working. setup_usd_env.bat,
REM which compile.py reads, already requires Python on PATH.
REM ===========================================================================

python "%~dp0compile.py" %*
EXIT /B %ERRORLEVEL%

@ECHO OFF
REM Runs depth.py in the OpenUSD environment; ctest calls it.
REM Usage: run_depth.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0depth.py" "%~1"
EXIT /B %ERRORLEVEL%

@ECHO OFF
REM Runs physical_brightness.py in the OpenUSD environment; ctest calls it.
REM Usage: run_physical_brightness.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0physical_brightness.py" "%~1"
EXIT /B %ERRORLEVEL%

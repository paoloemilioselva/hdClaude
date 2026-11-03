@ECHO OFF
REM Runs fog_volumes.py in the OpenUSD environment; ctest calls it.
REM Usage: run_fog_volumes.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0fog_volumes.py" "%~1"
EXIT /B %ERRORLEVEL%

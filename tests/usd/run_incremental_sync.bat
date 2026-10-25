@ECHO OFF
REM Runs incremental_sync.py in the OpenUSD environment; ctest calls it.
REM Usage: run_incremental_sync.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0incremental_sync.py" "%~1"
EXIT /B %ERRORLEVEL%

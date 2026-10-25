@ECHO OFF
REM Runs purposes.py in the OpenUSD environment; ctest calls it.
REM Usage: run_purposes.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0purposes.py" "%~1"
EXIT /B %ERRORLEVEL%

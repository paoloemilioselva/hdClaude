@ECHO OFF
REM Runs landing_page.py in the OpenUSD environment; ctest calls it.
REM Usage: run_landing_page.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0landing_page.py" "%~1"
EXIT /B %ERRORLEVEL%

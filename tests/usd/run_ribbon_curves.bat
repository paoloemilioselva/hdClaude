@ECHO OFF
REM Runs ribbon_curves.py in the OpenUSD environment; ctest calls it.
REM Usage: run_ribbon_curves.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0ribbon_curves.py" "%~1"
EXIT /B %ERRORLEVEL%

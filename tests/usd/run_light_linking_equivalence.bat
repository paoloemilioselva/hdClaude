@ECHO OFF
REM Runs light_linking_equivalence.py in the OpenUSD environment; ctest calls it.
REM Usage: run_light_linking_equivalence.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0light_linking_equivalence.py" "%~1"
EXIT /B %ERRORLEVEL%

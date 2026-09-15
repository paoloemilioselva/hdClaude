@ECHO OFF
REM Runs instancing_equivalence.py in the OpenUSD environment; ctest calls it.
REM Usage: run_instancing_equivalence.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0instancing_equivalence.py" "%~1"
EXIT /B %ERRORLEVEL%

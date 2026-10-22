@ECHO OFF
REM Runs cull_volume_equivalence.py in the OpenUSD environment; ctest calls it.
REM Usage: run_cull_volume_equivalence.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0cull_volume_equivalence.py" "%~1"
EXIT /B %ERRORLEVEL%

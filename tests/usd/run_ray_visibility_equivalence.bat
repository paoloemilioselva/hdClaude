@ECHO OFF
REM Runs ray_visibility_equivalence.py in the OpenUSD environment; ctest calls it.
REM Usage: run_ray_visibility_equivalence.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0ray_visibility_equivalence.py" "%~1"
EXIT /B %ERRORLEVEL%

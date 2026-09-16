@ECHO OFF
REM Runs texture_quality.py in the OpenUSD environment; ctest calls it.
REM Usage: run_texture_quality.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0texture_quality.py" "%~1"
EXIT /B %ERRORLEVEL%

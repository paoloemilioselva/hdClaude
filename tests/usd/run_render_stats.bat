@ECHO OFF
REM Runs render_stats.py in the OpenUSD environment; ctest calls it.
REM Usage: run_render_stats.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0render_stats.py" "%~1"
EXIT /B %ERRORLEVEL%

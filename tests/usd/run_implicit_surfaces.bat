@ECHO OFF
REM Runs implicit_surfaces.py in the OpenUSD environment; ctest calls it.
REM Usage: run_implicit_surfaces.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0implicit_surfaces.py" "%~1"
EXIT /B %ERRORLEVEL%

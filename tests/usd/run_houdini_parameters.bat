@ECHO OFF
REM Runs houdini_parameters.py in the OpenUSD environment; ctest calls it.
REM Usage: run_houdini_parameters.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0houdini_parameters.py" "%~1"
EXIT /B %ERRORLEVEL%

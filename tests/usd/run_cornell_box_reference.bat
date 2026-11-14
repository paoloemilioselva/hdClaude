@ECHO OFF
REM Runs cornell_box_reference.py in the OpenUSD environment; ctest calls it.
REM Usage: run_cornell_box_reference.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0cornell_box_reference.py" "%~1"
EXIT /B %ERRORLEVEL%

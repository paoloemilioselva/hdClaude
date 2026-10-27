@ECHO OFF
REM Runs material_edits.py in the OpenUSD environment; ctest calls it.
REM Usage: run_material_edits.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0material_edits.py" "%~1"
EXIT /B %ERRORLEVEL%

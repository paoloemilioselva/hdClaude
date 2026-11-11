@ECHO OFF
REM Runs unusable_materials.py in the OpenUSD environment; ctest calls it.
REM Usage: run_unusable_materials.bat <output directory>
SETLOCAL
CALL "%~dp0..\..\setup_usd_env.bat" >NUL || EXIT /B 1
python "%~dp0unusable_materials.py" "%~1"
EXIT /B %ERRORLEVEL%

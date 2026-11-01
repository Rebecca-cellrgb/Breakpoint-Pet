@echo off
setlocal
set "PYTHON="
for %%P in (python.exe) do set "PYTHON=%%~$PATH:P"
if not defined PYTHON set "PYTHON=%~dp0..\.venv-pose\Scripts\python.exe"
set "PYTHONPATH=%~dp0..\.venv-pose\Lib\site-packages;%PYTHONPATH%"
set "SCRIPT=%~dp0live_camera_viewer.py"
if not exist "%PYTHON%" (
  echo Python was not found.
  pause
  exit /b 1
)
"%PYTHON%" "%SCRIPT%"
pause

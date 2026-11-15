@echo off
cd /d "%~dp0"
if not exist .venv\Scripts\activate.bat (echo Run SETUP_WINDOWS.bat first.&pause&exit /b 1)
if not exist config.json (echo Create config.json from config.example.json.&pause&exit /b 1)
call .venv\Scripts\activate.bat
python server.py
pause

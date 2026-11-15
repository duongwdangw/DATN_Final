@echo off
cd /d "%~dp0"
echo === ESP32 Face AI - setup once ===
where py >nul 2>nul
if %errorlevel%==0 (
    py -3.11 -m venv .venv
) else (
    python -m venv .venv
)
if errorlevel 1 (echo Python 3.11 not found. Install Python 3.11 x64 then retry.&pause&exit /b 1)
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
if errorlevel 1 (echo Failed to install Python dependencies.&pause&exit /b 1)
python download_models.py
if errorlevel 1 (echo Cannot download model weights. Check Internet connection.&pause&exit /b 1)
if not exist config.json copy config.example.json config.json >nul
echo.
echo Open config.json now: set device_key and admin_key to DIFFERENT RANDOM STRINGS.
echo The device_key must be identical to ESP32_S3_CAM/main/face_config.h.
pause

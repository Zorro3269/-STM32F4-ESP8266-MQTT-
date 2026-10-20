@echo off
setlocal
cd /d "%~dp0.."
python Tools\ota_server.py --host 0.0.0.0 --port 8000
echo.
pause

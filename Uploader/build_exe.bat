@echo off
setlocal
echo FirmwareUpdater Derleniyor...
set PYTHON_EXE=python
if exist ".venv\Scripts\python.exe" (
    set PYTHON_EXE=.venv\Scripts\python.exe
)
"%PYTHON_EXE%" -m PyInstaller --clean FirmwareUpdater.spec
echo.
echo Islem tamamlandi!
pause

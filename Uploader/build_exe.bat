@echo off
echo FirmwareUpdater Derleniyor...
python -m PyInstaller --clean FirmwareUpdater.spec
echo.
echo Islem tamamlandi!
pause

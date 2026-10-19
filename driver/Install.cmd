@echo off
rem Right-click this file and choose "Run as administrator".
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-ML1250.ps1" -PrinterName "Samsung ML-1250"
pause

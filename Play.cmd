@echo off
rem Start the game (checks for updates first). Saves and settings stay in saves\ here.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0scripts\launch.ps1" %*

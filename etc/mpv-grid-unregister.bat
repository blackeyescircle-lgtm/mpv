@echo off
reg delete "HKCU\Software\Classes\.mpvgrid" /f >nul 2>nul
reg delete "HKCU\Software\Classes\mpv-grid.project" /f >nul 2>nul
reg delete "HKCU\Software\Classes\Applications\mpv-grid.exe" /f >nul 2>nul
echo Removed the current-user mpv Grid file association.

@echo off
setlocal
set "EXE=%~dp0mpv-grid.exe"
if not exist "%EXE%" (
  echo mpv-grid.exe was not found next to this script.
  exit /b 1
)

reg add "HKCU\Software\Classes\.mpvgrid" /ve /d "mpv-grid.project" /f >nul
reg add "HKCU\Software\Classes\mpv-grid.project" /ve /d "mpv Grid Project" /f >nul
reg add "HKCU\Software\Classes\mpv-grid.project\DefaultIcon" /ve /d "\"%EXE%\",0" /f >nul
reg add "HKCU\Software\Classes\mpv-grid.project\shell\open\command" /ve /d "\"%EXE%\" --grid-project=\"%%1\"" /f >nul
reg add "HKCU\Software\Classes\Applications\mpv-grid.exe\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul

echo Registered .mpvgrid for the current user.
endlocal

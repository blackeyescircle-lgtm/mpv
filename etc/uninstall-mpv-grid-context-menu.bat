@echo off
setlocal EnableExtensions

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $exts='3g2','3gp','avi','flv','ivf','m2ts','m4v','mj2','mkv','mov','mp4','mpeg','mpg','mxf','ogv','rmvb','ts','webm','wmv','y4m'; $keys=@($exts|ForEach-Object{'HKCU:\Software\Classes\SystemFileAssociations\.'+$_+'\shell\mpv-grid'}); $keys+='HKCU:\Software\Classes\SystemFileAssociations\video\shell\mpv-grid'; foreach($key in $keys){Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue};"
if errorlevel 1 goto :error

echo.
echo Removed the mpv Grid video context menu.
echo Windows Explorer may need a few seconds to refresh.
if not defined MPV_GRID_NO_PAUSE pause
exit /b 0

:error
echo.
echo ERROR: Failed to remove the mpv Grid video context menu.
if not defined MPV_GRID_NO_PAUSE pause
exit /b 1

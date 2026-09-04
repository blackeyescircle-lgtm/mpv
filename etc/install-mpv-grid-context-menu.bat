@echo off
setlocal EnableExtensions

set "MPV_GRID_EXE=%~dp0mpv-grid.exe"
if not exist "%MPV_GRID_EXE%" (
    echo ERROR: mpv-grid.exe was not found next to this script.
    echo Put both files in the same directory and run this script again.
    if not defined MPV_GRID_NO_PAUSE pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $exe=$env:MPV_GRID_EXE; $q=[char]34; $label=[string][char]0x4f7f+[char]0x7528+' mpv Grid '+[char]0x6253+[char]0x5f00; $exts='3g2','3gp','avi','flv','ivf','m2ts','m4v','mj2','mkv','mov','mp4','mpeg','mpg','mxf','ogv','rmvb','ts','webm','wmv','y4m'; $keys=@($exts|ForEach-Object{'HKCU:\Software\Classes\SystemFileAssociations\.'+$_+'\shell\mpv-grid'}); $keys+='HKCU:\Software\Classes\SystemFileAssociations\video\shell\mpv-grid'; foreach($key in $keys){New-Item -Path $key -Force|Out-Null; Set-Item -Path $key -Value $label; New-ItemProperty -Path $key -Name Icon -PropertyType String -Value ($q+$exe+$q+',0') -Force|Out-Null; New-Item -Path ($key+'\command') -Force|Out-Null; Set-Item -Path ($key+'\command') -Value ($q+$exe+$q+' -- '+$q+'%%1'+$q)};"
if errorlevel 1 goto :error

echo.
echo Installed the video context menu: Open with mpv Grid.
echo On Windows 11, look under Show more options if necessary.
echo Run this installer again after moving mpv-grid.exe.
if not defined MPV_GRID_NO_PAUSE pause
exit /b 0

:error
echo.
echo ERROR: Failed to install the mpv Grid video context menu.
if not defined MPV_GRID_NO_PAUSE pause
exit /b 1

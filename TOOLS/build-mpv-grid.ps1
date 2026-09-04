$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$msys = "C:\msys64\usr\bin\bash.exe"
$rootUnix = $root -replace '\\', '/'
if ($rootUnix -match '^([A-Za-z]):(.*)$') {
    $rootUnix = "/" + $Matches[1].ToLower() + $Matches[2]
}

if (-not (Test-Path -LiteralPath $msys)) {
    throw "MSYS2 was not found at $msys"
}

$setupMode = if (Test-Path -LiteralPath (Join-Path $root "build-mingw\meson-private\coredata.dat")) { "--reconfigure" } else { "" }
& $msys -lc "export PATH=/mingw64/bin:/usr/bin; cd '$rootUnix'; meson setup build-mingw $setupMode -Dbuild-date=false -Dlibmpv=false -Dtests=true -Dmanpage-build=disabled; ninja -C build-mingw mpv.exe mpv.com; meson test -C build-mingw --print-errorlogs"
if ($LASTEXITCODE -ne 0) {
    throw "mpv Grid build or tests failed"
}

$dist = Join-Path $root "dist\mpv-grid"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item -LiteralPath (Join-Path $root "build-mingw\mpv.exe") -Destination (Join-Path $dist "mpv-grid.exe") -Force
Copy-Item -LiteralPath (Join-Path $root "build-mingw\mpv.com") -Destination (Join-Path $dist "mpv-grid.com") -Force
Copy-Item -LiteralPath (Join-Path $root "etc\mpv-grid.ini") -Destination $dist -Force

$deps = & $msys -lc "export PATH=/mingw64/bin:/usr/bin; cd '$rootUnix'; ldd build-mingw/mpv.exe"
foreach ($line in $deps) {
    if ($line -match '=> /mingw64/bin/([^ ]+)') {
        $source = Join-Path "C:\msys64\mingw64\bin" $Matches[1]
        Copy-Item -LiteralPath $source -Destination $dist -Force
    }
}

Copy-Item -LiteralPath (Join-Path $root "etc\mpv-grid-register.bat") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "etc\mpv-grid-unregister.bat") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "LICENSE.GPL") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "Copyright") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "MPV_GRID.md") -Destination $dist -Force

Write-Host "Built $dist\mpv-grid.exe"

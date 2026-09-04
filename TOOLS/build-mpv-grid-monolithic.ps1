$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$bash = "C:\msys64\usr\bin\bash.exe"
$ffmpegSource = Join-Path $root "build-static-deps\src\ffmpeg-8.1.2"
$prefix = Join-Path $root "build-static-deps\prefix"
$jobs = [Math]::Max(1, [Environment]::ProcessorCount)

function Convert-ToMsysPath([string] $path) {
    $result = $path -replace '\\', '/'
    if ($result -match '^([A-Za-z]):(.*)$') {
        return "/" + $Matches[1].ToLower() + $Matches[2]
    }
    return $result
}

if (-not (Test-Path -LiteralPath $bash)) {
    throw "MSYS2 was not found at $bash"
}
if (-not (Test-Path -LiteralPath (Join-Path $ffmpegSource "configure"))) {
    throw "FFmpeg 8.1.2 source was not found at $ffmpegSource"
}

$rootUnix = Convert-ToMsysPath $root
$ffmpegUnix = Convert-ToMsysPath $ffmpegSource
$prefixUnix = Convert-ToMsysPath $prefix

# This is intentionally a broad local-playback build. Do not replace it with a
# --disable-everything codec whitelist: uncommon but valid media must keep
# working in the single-file distribution.
$ffmpegOptions = @(
    "--prefix='$prefixUnix'"
    "--disable-shared"
    "--enable-static"
    "--disable-programs"
    "--disable-doc"
    "--disable-network"
    "--disable-autodetect"
    "--disable-debug"
    "--disable-stripping"
    "--disable-avdevice"
    "--enable-zlib"
    "--enable-bzlib"
    "--enable-lzma"
    "--enable-iconv"
    "--enable-d3d11va"
    "--enable-dxva2"
    "--extra-cflags=-O2"
) -join " "

$env:MSYSTEM = "MINGW64"
$env:MSYS2_PATH_TYPE = "inherit"
$env:PKG_CONFIG_DONT_DEFINE_PREFIX = "1"
& $bash -lc "export PATH=/mingw64/bin:/usr/bin; cd '$ffmpegUnix'; ./configure $ffmpegOptions; make -j$jobs; make install"
if ($LASTEXITCODE -ne 0) {
    throw "The broad FFmpeg static build failed"
}

$componentHeader = Join-Path $ffmpegSource "config_components.h"
$required = @(
    "CONFIG_TRUEHD_DECODER 1"
    "CONFIG_MLP_DECODER 1"
    "CONFIG_DCA_DECODER 1"
    "CONFIG_PNG_DECODER 1"
    "CONFIG_PGS_FRAME_MERGE_BSF 1"
)
foreach ($component in $required) {
    if (-not (Select-String -LiteralPath $componentHeader -SimpleMatch $component -Quiet)) {
        throw "Required FFmpeg component is missing: $component"
    }
}

$setupMode = if (Test-Path -LiteralPath (Join-Path $root "build-mingw-monolithic\meson-private\coredata.dat")) {
    "--reconfigure --clearcache"
} else {
    ""
}
$mesonOptions = @(
    "-Dbuildtype=release"
    "-Dprefer_static=true"
    "-Ddefault_library=static"
    "-Dbuild-date=false"
    "-Dlibmpv=false"
    "-Dtests=false"
    "-Dmanpage-build=disabled"
    "-Dc_link_args='-static -static-libgcc'"
    "-Dlibavdevice=disabled"
    "-Dlibarchive=disabled"
    "-Dlibbluray=disabled"
    "-Dcdda=disabled"
    "-Ddvdnav=disabled"
    "-Ddvbin=disabled"
    "-Djavascript=disabled"
    "-Dlua=disabled"
    "-Drubberband=disabled"
    "-Duchardet=disabled"
    "-Dvapoursynth=disabled"
    "-Dzimg=disabled"
    "-Dzlib=disabled"
    "-Diconv=disabled"
    "-Djpeg=disabled"
    "-Dlcms2=disabled"
    "-Dcaca=disabled"
    "-Ddirect3d=disabled"
    "-Dgl=disabled"
    "-Dshaderc=enabled"
    "-Dspirv-cross=enabled"
    "-Dsixel=disabled"
    "-Dvulkan=disabled"
    "-Dd3d11=enabled"
    "-Dd3d-hwaccel=enabled"
    "-Dwasapi=enabled"
    "-Dwin32-smtc=disabled"
) -join " "

& $bash -lc "export PATH=/mingw64/bin:/usr/bin; export PKG_CONFIG_DONT_DEFINE_PREFIX=1; export PKG_CONFIG_PATH='$prefixUnix/lib/pkgconfig:/mingw64/lib/pkgconfig'; cd '$rootUnix'; meson setup build-mingw-monolithic $setupMode $mesonOptions; ninja -C build-mingw-monolithic"
if ($LASTEXITCODE -ne 0) {
    throw "The monolithic mpv Grid build failed"
}

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$output = Join-Path $dist "mpv-grid.exe"
Copy-Item -LiteralPath (Join-Path $root "build-mingw-monolithic\mpv.exe") -Destination $output -Force
& "C:\msys64\mingw64\bin\strip.exe" --strip-all $output
if ($LASTEXITCODE -ne 0) {
    throw "Failed to strip the final executable"
}
Copy-Item -LiteralPath (Join-Path $root "etc\mpv-grid.ini") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "etc\install-mpv-grid-context-menu.bat") -Destination $dist -Force
Copy-Item -LiteralPath (Join-Path $root "etc\uninstall-mpv-grid-context-menu.bat") -Destination $dist -Force

$ldd = & $bash -lc "export PATH=/mingw64/bin:/usr/bin; ldd '$(Convert-ToMsysPath $output)'"
if ($ldd -match '/mingw64/bin/') {
    throw "The result still depends on an MSYS2 runtime DLL:`n$ldd"
}

$hash = Get-FileHash -LiteralPath $output -Algorithm SHA256
Write-Host "Built $output"
Write-Host "Size: $((Get-Item -LiteralPath $output).Length) bytes"
Write-Host "SHA-256: $($hash.Hash)"

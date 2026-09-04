param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\assets")
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$player = Join-Path $root "dist\mpv-grid.exe"
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$sources = @(
    (Join-Path $SourceDirectory "octopus.webm"),
    (Join-Path $SourceDirectory "storm.webm"),
    (Join-Path $SourceDirectory "iss-night.mp4"),
    (Join-Path $SourceDirectory "sunrise.mp4")
)

if (-not (Test-Path -LiteralPath $player)) {
    throw "Build dist\mpv-grid.exe before creating the demo."
}
foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Missing demo source: $source"
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputDirectory = (Resolve-Path $OutputDirectory).Path
$capture = Join-Path $outputDirectory "mpv-multiview-demo.mp4"
$gif = Join-Path $outputDirectory "mpv-multiview-demo.gif"
$palette = Join-Path $env:TEMP "mpv-multiview-demo-palette.png"
$ffmpegLog = Join-Path $env:TEMP "mpv-multiview-demo-ffmpeg.log"
$pipeName = "mpv-multiview-demo-$PID"
$windowTitle = "mpv MultiView Demo"

$playerArgs = @(
    "--no-config",
    "--grid=2x2",
    "--force-window=yes",
    "--geometry=960x540+80+80",
    "--border=no",
    "--title=`"$windowTitle`"",
    "--input-ipc-server=\\.\pipe\$pipeName",
    "--loop-file=inf",
    "--osd-level=1",
    "--osd-font-size=28",
    "--osd-border-size=2",
    "--osd-align-y=top",
    "--osd-margin-y=30",
    "--cursor-autohide=no",
    $sources[0]
)

$playerProcess = Start-Process -FilePath $player -ArgumentList $playerArgs -PassThru
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".", $pipeName,
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None
)

try {
    $pipe.Connect(15000)
    $writer = [System.IO.StreamWriter]::new($pipe)
    $reader = [System.IO.StreamReader]::new($pipe)
    $writer.AutoFlush = $true

    function Send-MpvCommand([object[]]$Command) {
        $writer.WriteLine((@{ command = $Command } | ConvertTo-Json -Compress))
        do {
            $line = $reader.ReadLine()
            if ($null -eq $line) { throw "mpv IPC connection closed." }
            $reply = $line | ConvertFrom-Json
        } until ($reply.PSObject.Properties.Name -contains "error")
        if ($reply.error -ne "success") {
            throw "mpv command failed: $($Command -join ' ') ($($reply.error))"
        }
    }

    Start-Sleep -Seconds 4

    $recordArgs = @(
        "-y", "-f", "lavfi",
        "-i", "ddagrab=output_idx=0:draw_mouse=1:framerate=20:video_size=960x540:offset_x=80:offset_y=80",
        "-t", "16", "-vf", "hwdownload,format=bgra",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
        "-pix_fmt", "yuv420p", $capture
    )
    $quotedRecordArgs = $recordArgs | ForEach-Object {
        if ($_ -match "\s") { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }
    $recorder = Start-Process -FilePath $ffmpeg -ArgumentList $quotedRecordArgs `
        -WindowStyle Hidden -PassThru -RedirectStandardError $ffmpegLog

    Start-Sleep -Milliseconds 800
    Send-MpvCommand @("show-text", "mpv MultiView  |  Multi-Video, Highlight & Detail Viewer", 2100)
    Start-Sleep -Milliseconds 2300

    Send-MpvCommand @("grid-layout", 3, 3)
    Send-MpvCommand @("show-text", "2x2  /  3x3  /  4x4  and more", 1800)
    Start-Sleep -Milliseconds 2200

    Send-MpvCommand @("grid-layout", 2, 2)
    Start-Sleep -Milliseconds 700
    Send-MpvCommand @("grid-drop", 720, 135, $sources[1])
    Send-MpvCommand @("grid-drop", 240, 405, $sources[2])
    Send-MpvCommand @("grid-drop", 720, 405, $sources[3])
    Send-MpvCommand @("show-text", "Different videos, one tightly packed Grid", 2100)
    Start-Sleep -Milliseconds 2500

    Send-MpvCommand @("mouse", 720, 405, -1)
    Send-MpvCommand @("keydown", "MBTN_LEFT")
    Send-MpvCommand @("show-text", "Hold and move  |  2x detail zoom", 2400)
    Start-Sleep -Milliseconds 700
    Send-MpvCommand @("mouse", 850, 460, -1)
    Send-MpvCommand @("keypress", "MOUSE_MOVE")
    Start-Sleep -Milliseconds 900
    Send-MpvCommand @("mouse", 600, 330, -1)
    Send-MpvCommand @("keypress", "MOUSE_MOVE")
    Start-Sleep -Milliseconds 900
    Send-MpvCommand @("keyup", "MBTN_LEFT")

    Send-MpvCommand @("mouse", 720, 405, -1)
    Send-MpvCommand @("keypress", "WHEEL_UP")
    Send-MpvCommand @("show-text", "Mouse wheel  |  seek 5 seconds in this tile", 1700)
    Start-Sleep -Milliseconds 1900
    Send-MpvCommand @("show-text", "F1 save scene  |  F2 resume  |  Esc close", 2200)

    if (-not $recorder.WaitForExit(25000)) {
        $recorder.Kill()
        throw "Screen recording timed out."
    }
    if ($recorder.ExitCode -ne 0) {
        throw "FFmpeg recording failed. See $ffmpegLog"
    }
} finally {
    if ($null -ne $recorder -and -not $recorder.HasExited) {
        $recorder.Kill()
        $recorder.WaitForExit()
    }
    if ($pipe.IsConnected) {
        try { Send-MpvCommand @("quit") } catch {}
    }
    $pipe.Dispose()
    if (-not $playerProcess.HasExited) {
        $playerProcess.WaitForExit(5000) | Out-Null
    }
}

& $ffmpeg -y -i $capture -vf "fps=8,scale=640:-1:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" $palette
if ($LASTEXITCODE -ne 0) { throw "GIF palette generation failed." }
& $ffmpeg -y -i $capture -i $palette -lavfi "fps=8,scale=640:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" -loop 0 $gif
if ($LASTEXITCODE -ne 0) { throw "GIF generation failed." }
if ((Get-Item -LiteralPath $capture).Length -lt 500KB -or
    (Get-Item -LiteralPath $gif).Length -lt 500KB) {
    throw "The captured demo is unexpectedly small and may contain blank frames."
}

Write-Host "Created $capture"
Write-Host "Created $gif"

# mpv Grid

`mpv-grid.exe` is a Windows-first fork of mpv v0.41.0 (`41f6a645`) for playing
up to 16 independent timelines in one native window. It keeps one mpv core,
one `vo_gpu_next` output, one D3D11 device/swapchain and one audio endpoint.

## Run

Open a media file directly with `mpv-grid.exe`. The renamed executable enables
Grid mode automatically and restores the last layout; the first layout is 2x2.
The original `mpv.exe`, or `mpv-grid.exe --grid=no`, retains normal mpv behavior.

Explicit forms:

```
mpv-grid.exe --grid=2x2 video.mp4
mpv-grid.exe --grid=2x3 video.mp4
mpv-grid.exe --grid=3x3 video.mp4
mpv-grid.exe --grid=3x4 video.mp4
mpv-grid.exe --grid=4x4 video.mp4
mpv-grid.exe --grid-project=demo.mpvgrid --grid-open-mode=resume
```

On Windows, Grid mode defaults to `vo=gpu-next`, `gpu-api=d3d11` and the safe
copy hardware decoder `hwdec=d3d11va-copy`, unless the user already selected a
different value. Auxiliary pipelines reference the D3D11 device exported by
the same VO and fall back to software decoding independently.

## Input

- `Tab` / `Shift+Tab`: next / previous tile
- `Space` / `Ctrl+Space`: pause current / all tiles
- arrows: seek current tile by 5 or 30 seconds
- wheel / `Shift+wheel`: seek the hovered tile by 1 or 5 seconds
- `M` / `Shift+M`: mute current tile / solo and restore
- `+` / `-`: current tile volume
- `[` / `]`: current tile speed
- `B`: store the current position as the fixed start
- `1`: logical 1x1 mode (`0` remains a compatibility alias)
- `2`: 2x2
- `3`: 3x3
- `4`: 4x4
- `5`: 2x3
- `6`: 3x4
- `F1`: create a desktop `.lnk` whose explicit screenshot icon is a composite
  of every visible tile and freeze every current tile at that highlight moment
- `F2`: silently resume every tile from the progress saved when the project
  was opened, without a success OSD
- `F`: fullscreen; tiles form one tightly packed, centered canvas. The first
  video's native display resolution is the maximum size of each tile, so a
  larger surface can have equal padding on both the top/bottom and left/right;
  the complete canvas is uniformly reduced only when it does not fit
- A differently shaped dropped video is aspect-fit inside its existing cell,
  touching one opposite edge pair without changing the outer Grid dimensions
- `Esc`: close the current player window
- `Ctrl+1` ... `Ctrl+6`: the same layout mappings
- `Ctrl+S`: atomically save the current project
- hold left mouse: select and temporarily zoom 2x; move to pan; release to reset
- mouse wheel: seek the hovered tile and show a translucent, tile-local time
  and progress indicator near its lower edge

Grid audio uses a configurable rolling FIFO. Tiles 1 through 3 are selected for
sound by default. Set `grid-audio-count=1..16` in the INI `[settings]` section
or pass `--grid-audio-count=1..16` to change its size. Clicking a tile moves it
to the newest FIFO position; when the limit is exceeded, the oldest selected
tile stops contributing to the mix. Manual mute, per-tile volume and solo are applied on top of this policy.
Solo temporarily bypasses the FIFO and restores it on exit. The read-only
`grid.audioFifoLimit` and `grid.audioFifo` nodes report the configured limit
and zero-based selected tile ids, for example `3` and `[0,1,2]`.
Auxiliary decoders discard seek preroll by timestamp and re-anchor after an
underrun, so audio begins at the tile's current video clock instead of the
preceding keyframe.

Windows Explorer drops use DPI-aware client coordinates. Dropped files are
appended in Explorer order. When the Grid is empty, dropping the first single
video anywhere opens it as the primary media and automatically fills every tile
in the current (or last remembered) layout with evenly distributed starting
positions. Later drops start immediately in the target tile, so an automatically
populated grid can be replaced tile by tile without waiting for the old item to
finish.

The last mode is persistent, including logical 1x1 (stored internally as `no`
for `--grid=no` compatibility) after pressing `1` or its `0` compatibility alias. Number keys
remain available in remembered normal mode so Grid can be enabled again without
restarting. A single automatically populated local video also writes
`%LOCALAPPDATA%\mpv-grid\last-session.json` atomically every five seconds and
on clean unload. The state contains the normalized media path, mode and all
visible tile positions. It is restored only when the next launch opens the same
single file with automatic mode selection; unrelated files, mixed per-tile
playlists and explicit `.mpvgrid` projects do not inherit it. A naturally
completed ordinary video records position zero instead of reopening at EOF.

F1 creates one user-visible `.lnk` on the Windows desktop. Its generated name
contains the media name, layout and active-tile timestamp. A 256x256 ICO with
the complete composed Grid and a self-contained `.grd` project are stored under
`%LOCALAPPDATA%\mpv-grid\shortcuts`. The link targets that `.grd` document, not
an executable, and uses the explicit `.ico` as its icon. Logical 1x1 snapshots
use the same format and restore the main file and timestamp.
For multi-file layouts, every tile playlist is serialized in order. Every media
item retains its relative path, absolute fallback, fixed start and latest resume
position, together with the layout, active tile and current playlist indexes.
Opening the link defaults to all captured F1 fixed starts. A translucent
bottom-center hint explains that F2 restores every tile to the resume positions
loaded at startup; it disappears after three seconds or on a tile click. The
startup resume anchors are kept separately in memory so a five-second autosave
cannot change the destination of F2 during the current launch.
Legacy `.grd` containers without the snapshot marker promote their saved resume
positions to fixed highlight starts on first load.

Every launch of `mpv-grid.exe` refreshes the per-user `.grd` ProgID and open
command with the executable's actual current path. Since the `.lnk` contains no
executable path, moving the single EXE is supported: run it manually once in its
new location and all existing shortcuts open there afterward. Registration
uses HKCU and does not require elevation.

Before any media is loaded, the entire Windows client area acts as a native
title bar for dragging the window. Window borders keep their normal resize
behavior. As soon as media is loaded, the client area returns to normal tile
mouse interaction.

The first video's display resolution also determines the initial Grid window
size. The preferred client size is `video width × columns` by `video height ×
rows`, so a small source is shown at native resolution in every tile. If that
combined rectangle exceeds the current screen work area, it is scaled down
uniformly and translated back inside the current monitor if the empty window
was previously dragged near an edge. This keeps each cell at the source aspect ratio and avoids the large
letterbox regions that would result from stretching the Grid to an unrelated
screen aspect ratio. Explicit `--geometry` and `--autofit*` settings still win.

When an automatically populated single-video Grid changes layout, all visible
tiles are rebuilt from the same source. Their fixed starts are recalculated as
`duration × tile / tile-count`, beginning at 0, all tiles resume playback, and
the window is resized for the new rows and columns. Once a user explicitly
adds, removes, or drops media into an individual tile, layout changes preserve
those independent playlists instead.

## Portable INI configuration

`mpv-grid.ini` is loaded from the executable directory on `mpv-grid.exe`
startup. `[settings]` accepts ordinary mpv `option=value` entries, `[keys]`
defines bindings that remain active in logical 1x1 mode, and `[grid-keys]`
defines bindings enabled only for multi-tile playback. Command-line options
override INI settings. `--no-config` skips the file, and a missing file falls
back to the compiled defaults. Restart the player after editing the file.

## IPC and scripting

The read-only `grid` node property exposes layout, active tile, status,
positions, per-tile audio state and playlists. Runtime audio state includes
`fifoSelected`, `bufferedSamples`, `mixedSamples`, `readPosition`,
`syncPending`, `prerollDroppedSamples` and `syncDroppedSamples` diagnostics;
these fields
are deliberately omitted from saved projects. Commands are available through
input bindings, Lua and JSON IPC:

```
grid-disable
grid-desktop-shortcut
grid-resume-project
grid-layout ROWS COLUMNS
grid-active TILE
grid-cycle-active DIRECTION
grid-append TILE PATH [FIXED_START]
grid-remove TILE [INDEX]
grid-seek TILE SECONDS [relative|absolute]
grid-pause TILE [toggle|no|yes]
grid-pause-all
grid-mute TILE [toggle|no|yes]
grid-solo [TILE]
grid-volume TILE VALUE [set|add]
grid-speed TILE VALUE [set|add]
grid-fixed-start [TILE] [POSITION]
grid-save [PATH]
```

Use tile `-1` for the active tile. Saving without a path updates the open
project; a new session defaults to `<media-path>.mpvgrid`.

## Project format

`.mpvgrid` is UTF-8 JSON with `format: "mpv-grid-project"` and `version: 1`.
It stores the layout, active tile, open mode, per-tile pause/speed/audio state,
and each playlist item's path, absolute fallback, fixed start and resume
position. Writes use a temporary file and atomic replacement. Resume positions
are checkpointed every five seconds and on a clean unload.

`.grd` wraps the same version-1 JSON payload after a standard ICO image. The
loader accepts both formats; ordinary `.mpvgrid` files remain plain readable
JSON for editing and interchange.

`--grid-open-mode=fixed` uses fixed starts; `resume` uses checkpoints. `ask`
currently uses fixed-start semantics and remains represented in the format so
a native chooser can be added without a migration.

## Build and status

Run `TOOLS\build-mpv-grid.ps1` from PowerShell on an MSYS2 MinGW64 development
machine. It builds, runs the upstream Meson tests, and assembles a portable
directory at `dist\mpv-grid`. The release pipeline should pin FFmpeg 8.1.2,
libplacebo 7.360.0, libass 0.17.5 and Meson 1.9.2; the local validation build
uses installed ABI-compatible packages.

A monolithic Windows build is also emitted as `dist\mpv-grid.exe`. It statically
links FFmpeg, libplacebo, libass and their non-system dependencies into the EXE,
does not unpack files at startup, and does not require adjacent runtime DLLs.
Normal Windows system libraries and the installed graphics driver are still
loaded by the operating system. The static FFmpeg profile targets local media
playback and deliberately does not use a `--disable-everything` codec whitelist.
It retains FFmpeg's built-in decoders, demuxers, parsers, bitstream filters,
encoders, muxers and playback filters, plus zlib, bzip2, LZMA and character-set
support. D3D11VA/DXVA2 acceleration covers H.264, H.265, MPEG-2, VC-1, VP9,
WMV3 and AV1. The audio profile includes Dolby TrueHD/MLP (including
Atmos-bearing TrueHD tracks), AC-3/E-AC-3, DTS/DTS-HD, AAC, FLAC, Opus and the
other built-in audio codecs; multichannel tracks are decoded and mixed to the
active WASAPI output format. Only FFmpeg command-line programs, documentation,
capture devices, network I/O and automatic optional-library detection are
excluded from this local-playback executable.

`TOOLS/build-mpv-grid-monolithic.ps1` reproduces this broad static profile and
fails the build if important TrueHD, MLP, DTS, PNG or PGS components disappear.

The current implementation includes the single-core state model, 16 auxiliary
video pipelines, shared D3D11 hardware context, multi-viewport `gpu-next`
composition, independent scheduling/seek/pause/speed/looping, one-output PCM
audio mixer with sample-rate conversion, normalization and limiting, mouse/drop controls, commands,
project persistence and packaging. The current portable validation binary was
built with MSYS2 GCC; the reproducible release job still needs to be moved to
the planned clang-cl/MSVC ABI dependency lock.

Subtitle decoding remains on the main mpv pipeline. The native `ask` chooser,
missing-file relocation UI, read-only-project state fallback, per-tile subtitle
rendering, macOS platform glue, and the two-hour/16x1080 acceptance matrix are
subsequent milestones rather than claims of this development build.

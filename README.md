# AudioSplitX

AudioSplitX is a Windows WASAPI app that routes one source playback endpoint to multiple output endpoints with low-latency synchronization.

This project is now software-only.
It does not include a custom driver or installer flow.

Use a virtual cable endpoint (for example, VB-Cable `CABLE Input`) as the source, then route that audio to one or more physical output devices.

## Requirements

- Windows 10/11
- Visual Studio 2022 with Desktop development for C++
- CMake 3.24+
- A virtual cable device for MVP testing (VB-Cable or Virtual Audio Cable)

## Quick Build (Recommended)

```powershell
tools\build-software.ps1
```

Output:

```text
dist\Audio Split-X.exe
```

## Manual Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Key outputs:

```text
build\Release\Audio Split-X.exe
build\Release\audiosplitx.exe
```

## Run The UI

```powershell
.\dist\Audio Split-X.exe
```

The UI includes:

- source device selection
- output device toggles
- start and stop routing
- sync scope and per-output meters

Design reference:

```text
design\audiosplitx-main-ui.svg
```

## Run The CLI

List active playback endpoints:

```powershell
.\build\Release\audiosplitx.exe --list
```

Route virtual cable to two devices:

```powershell
.\build\Release\audiosplitx.exe --source "CABLE Input" --out "Speakers" --out "Headphones"
```

Use shared mode only:

```powershell
.\build\Release\audiosplitx.exe --source "CABLE Input" --out "Speakers" --out "Headphones" --shared
```

Write telemetry CSV:

```powershell
.\build\Release\audiosplitx.exe --source "CABLE Input" --out "Speakers" --out "Headphones" --debug-csv .\latency.csv
```

## Current Behavior

- Captures from the selected render endpoint using WASAPI loopback.
- Normalizes audio to interleaved float32 PCM internally.
- Fans out the source stream to one SPSC ring per output.
- Runs one event-driven WASAPI render thread per output device.
- Attempts exclusive mode first, then falls back to shared mode.
- Uses MMCSS `Pro Audio` scheduling for capture, master, and render threads.
- Aligns startup buffering to the slowest endpoint latency.
- Applies per-device ppm correction through adaptive resampling.
- Exposes live telemetry and optional CSV logging.

## Known Limitations

- Depends on virtual cable behavior for source loopback path.
- Resampler is MVP-grade; replace `AdaptiveResampler` with a higher-quality library for production.
- Automatic full graph rebuild after endpoint invalidation is not yet implemented.
- Bit-perfect output is only possible when exclusive mode accepts the exact source format.

## Usage Notes

- Do not use the same endpoint as both source and output.
- Prefer full endpoint IDs from `--list` when names are similar.

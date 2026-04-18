# AudioSplitX Architecture

AudioSplitX currently uses a software-only MVP architecture.
There is no custom kernel driver path in the active build.

## MVP Pipeline

```text
Application
  -> existing virtual cable playback endpoint
  -> WASAPI loopback capture thread
  -> capture SPSC ring, float32 interleaved
  -> master fan-out thread
  -> one SPSC ring per output device
  -> per-device sync controller
  -> per-device adaptive resampler
  -> WASAPI render thread
  -> hardware endpoint
```

## Main Components

- `DeviceEnumerator`: Lists and resolves active render endpoints, reads endpoint mix formats, and registers endpoint topology notifications.
- `WasapiLoopbackCapture`: Captures packets from the selected virtual render endpoint using WASAPI loopback and writes float32 PCM into a lock-free SPSC ring.
- `AudioEngine`: Owns capture, fan-out, output startup, latency target calculation, and telemetry.
- `OutputDevice` (in `WasapiRenderer.h/.cpp`): Owns one WASAPI render client, one input SPSC ring, one sync controller, one adaptive resampler, and one render thread.
- `AdaptiveResampler`: Pulls source-rate float32 PCM from an output ring and produces endpoint-rate float32 PCM with ppm correction.
- `SyncController`: Converts ring-fill error into a smoothed ppm correction value.

## App Surface

- UI app (`Audio Split-X.exe`): Windows GUI for source/output selection and start/stop control.
- CLI app (`audiosplitx.exe`): command-line path for listing endpoints, routing, and telemetry output.

## Threading Model

- Capture thread: event-driven WASAPI loopback capture, MMCSS `Pro Audio`, no locks in the packet path.
- Master thread: pulls source frames from the capture ring and pushes them into every output ring.
- Output threads: one event-driven WASAPI render thread per endpoint, MMCSS `Pro Audio`.
- Monitor thread: low-priority console and CSV telemetry. It does not participate in audio packet movement.

## Synchronization

Initial alignment:

1. Each output endpoint reports `IAudioClient::GetStreamLatency`.
2. The engine finds the slowest endpoint.
3. Faster devices receive a larger target input-ring fill.
4. Every output starts with silence seeded to its target fill.

Drift correction:

1. Each render callback observes its input-ring fill.
2. `SyncController` compares current fill against target fill.
3. The controller emits a smoothed correction in parts per million.
4. `AdaptiveResampler` changes its source-consumption ratio by that ppm amount.

This keeps devices converged without discontinuous sample drops in the normal path.

## Fidelity Rules

- Internal samples are float32 PCM.
- Exclusive mode is attempted first with source sample rate and channel count.
- If exact exclusive output is available, Windows shared-mode mixing is bypassed for that endpoint.
- If exclusive mode fails, shared mode is used so the device can still participate.
- Format conversion is explicit and local: PCM16, packed PCM24, PCM24-in-32, PCM32, and float32 are supported.

## Production Replacement Points

- Replace `AdaptiveResampler` with libsamplerate or speexdsp.
- Add automatic graph rebuild for `AUDCLNT_E_DEVICE_INVALIDATED`.
- Add persistent per-device latency calibration instead of using only WASAPI stream latency.


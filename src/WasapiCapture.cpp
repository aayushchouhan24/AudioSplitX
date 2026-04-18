#include "WasapiCapture.h"

#include "AudioSampleConvert.h"
#include "Common.h"
#include "Mmcss.h"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace asx {

WasapiLoopbackCapture::WasapiLoopbackCapture(std::wstring endpointId, AudioFormat format, double ringMs, double bufferMs)
    : endpointId_(std::move(endpointId))
    , format_(format)
    , bufferMs_(bufferMs)
{
    const auto frames = static_cast<size_t>((static_cast<double>(format_.sampleRate) * ringMs) / 1000.0);
    ring_.resetCapacity(std::max<size_t>(frames * format_.channels, format_.channels * 1024));
}

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
}

void WasapiLoopbackCapture::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread(&WasapiLoopbackCapture::threadProc, this);
}

void WasapiLoopbackCapture::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

CaptureStatsSnapshot WasapiLoopbackCapture::stats() const noexcept
{
    CaptureStatsSnapshot out;
    out.framesCaptured = framesCaptured_.load(std::memory_order_acquire);
    out.packets = packets_.load(std::memory_order_acquire);
    out.silentPackets = silentPackets_.load(std::memory_order_acquire);
    out.ringOverruns = ringOverruns_.load(std::memory_order_acquire);
    return out;
}

void WasapiLoopbackCapture::threadProc()
{
    try {
        ComApartment com;
        MmcssScope mmcss;

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
            "CoCreateInstance(MMDeviceEnumerator)");

        ComPtr<IMMDevice> device;
        checkHr(enumerator->GetDevice(endpointId_.c_str(), &device), "IMMDeviceEnumerator::GetDevice(capture)");

        ComPtr<IAudioClient> audioClient;
        checkHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient), "Activate capture IAudioClient");

        WAVEFORMATEX* rawMix = nullptr;
        checkHr(audioClient->GetMixFormat(&rawMix), "capture GetMixFormat");
        CoMemPtr<WAVEFORMATEX> mix(rawMix);
        const AudioFormat actualFormat = parseWaveFormat(*mix);
        format_ = actualFormat;

        UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event) {
            checkHr(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent(capture)");
        }

        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
        checkHr(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, msToHns(bufferMs_), 0, mix.get(), nullptr),
            "capture IAudioClient::Initialize(loopback shared)");
        checkHr(audioClient->SetEventHandle(event.get()), "capture SetEventHandle");

        ComPtr<IAudioCaptureClient> captureClient;
        checkHr(audioClient->GetService(IID_PPV_ARGS(&captureClient)), "GetService(IAudioCaptureClient)");
        checkHr(audioClient->Start(), "capture IAudioClient::Start");

        std::vector<float> scratch;
        UINT32 packetFrames = 0;

        while (running_.load(std::memory_order_acquire)) {
            WaitForSingleObject(event.get(), 5);

            checkHr(captureClient->GetNextPacketSize(&packetFrames), "capture GetNextPacketSize");
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flagsOut = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                checkHr(captureClient->GetBuffer(&data, &frames, &flagsOut, &devicePosition, &qpcPosition),
                    "capture GetBuffer");

                const bool silent = (flagsOut & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                scratch.resize(static_cast<size_t>(frames) * actualFormat.channels);
                convertToFloat(data, frames, actualFormat, scratch.data(), silent);

                const size_t samples = scratch.size();
                const size_t available = ring_.writeAvailable();
                if (available < samples) {
                    ring_.discard(samples - available);
                    ringOverruns_.fetch_add(1, std::memory_order_acq_rel);
                }
                ring_.push(scratch.data(), samples);

                framesCaptured_.fetch_add(frames, std::memory_order_acq_rel);
                packets_.fetch_add(1, std::memory_order_acq_rel);
                if (silent) {
                    silentPackets_.fetch_add(1, std::memory_order_acq_rel);
                }

                checkHr(captureClient->ReleaseBuffer(frames), "capture ReleaseBuffer");
                checkHr(captureClient->GetNextPacketSize(&packetFrames), "capture GetNextPacketSize(loop)");
            }
        }

        audioClient->Stop();
    } catch (const std::exception& ex) {
        std::cerr << "Capture thread failed: " << ex.what() << "\n";
        running_.store(false, std::memory_order_release);
    }
}

} // namespace asx


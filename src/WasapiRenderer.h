#pragma once

#include "AudioFormat.h"
#include "Resampler.h"
#include "SpscRingBuffer.h"
#include "SyncManager.h"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace asx {

struct OutputDeviceConfig {
    std::wstring endpointId;
    std::wstring name;
    AudioFormat sourceFormat;
    bool preferExclusive = true;
    double ringMs = 500.0;
    double bufferMs = 10.0;
};

struct OutputDeviceSnapshot {
    std::wstring name;
    bool ready = false;
    bool failed = false;
    bool exclusive = false;
    std::string formatDescription;
    double streamLatencyMs = 0.0;
    double ringFillMs = 0.0;
    double targetFillMs = 0.0;
    double correctionPpm = 0.0;
    uint64_t framesRendered = 0;
    uint64_t underruns = 0;
    uint64_t inputOverruns = 0;
};

class OutputDevice {
public:
    explicit OutputDevice(OutputDeviceConfig config);
    OutputDevice(const OutputDevice&) = delete;
    OutputDevice& operator=(const OutputDevice&) = delete;
    ~OutputDevice();

    void start();
    void stop();
    bool waitUntilReady(DWORD timeoutMs);
    void releaseStartGate();

    void pushInput(const float* frames, size_t frameCount);
    void seedSilence(size_t frameCount);
    void setTargetFillFrames(size_t frames) noexcept;

    size_t streamLatencySourceFrames() const noexcept;
    OutputDeviceSnapshot snapshot() const;
    std::string lastError() const;

private:
    struct ClientSelection {
        Microsoft::WRL::ComPtr<IAudioClient> client;
        CoMemPtr<WAVEFORMATEX> format;
        AudioFormat parsedFormat;
        AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
        bool exclusive = false;
    };

    void threadProc();
    ClientSelection initializeClient(IMMDevice& device);
    bool tryExclusive(IMMDevice& device, ClientSelection& selection);
    void initializeShared(IMMDevice& device, ClientSelection& selection);
    void fillEndpointBuffer(IAudioRenderClient& renderClient, UINT32 frames);
    void setReady(bool ready, bool failed, std::string error = {});

    OutputDeviceConfig config_;
    SpscRingBuffer<float> inputRing_;
    AdaptiveResampler resampler_;
    SyncController sync_;
    std::vector<float> floatScratch_;
    std::vector<float> silenceScratch_;

    std::atomic<bool> running_ { false };
    std::atomic<bool> startReleased_ { false };
    std::thread thread_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateCv_;
    bool ready_ = false;
    bool failed_ = false;
    std::string lastError_;

    std::atomic<bool> exclusive_ { false };
    std::atomic<uint32_t> outputSampleRate_ { 0 };
    std::atomic<uint16_t> outputChannels_ { 0 };
    std::atomic<uint64_t> streamLatencyHns_ { 0 };
    std::atomic<size_t> streamLatencySourceFrames_ { 0 };
    std::atomic<double> ringFillMs_ { 0.0 };
    std::atomic<double> targetFillMs_ { 0.0 };
    std::atomic<uint64_t> framesRendered_ { 0 };
    std::atomic<uint64_t> underruns_ { 0 };
    std::atomic<uint64_t> inputOverruns_ { 0 };
    std::string formatDescription_;
    AudioFormat outputFormat_;
};

} // namespace asx

#pragma once

#include "DeviceEnumerator.h"
#include "WasapiCapture.h"
#include "WasapiRenderer.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace asx {

struct AudioEngineConfig {
    AudioEndpoint source;
    std::vector<AudioEndpoint> outputs;
    bool preferExclusive = true;
    double captureRingMs = 750.0;
    double outputRingMs = 750.0;
    double endpointBufferMs = 10.0;
    bool consoleMeter = true;
    std::string debugCsvPath;
};

class AudioEngine {
public:
    explicit AudioEngine(AudioEngineConfig config);
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    ~AudioEngine();

    void start();
    void stop();
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void masterThreadProc();
    void monitorThreadProc();
    void configureOutputTargets();

    AudioEngineConfig config_;
    AudioFormat sourceFormat_;
    std::unique_ptr<WasapiLoopbackCapture> capture_;
    std::vector<std::unique_ptr<OutputDevice>> outputs_;
    std::unique_ptr<NotificationRegistration> notifications_;

    std::atomic<bool> running_ { false };
    std::thread masterThread_;
    std::thread monitorThread_;
    std::vector<float> masterScratch_;
};

} // namespace asx


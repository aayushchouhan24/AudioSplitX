#pragma once

#include "AudioFormat.h"
#include "SpscRingBuffer.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace asx {

struct CaptureStatsSnapshot {
    uint64_t framesCaptured = 0;
    uint64_t packets = 0;
    uint64_t silentPackets = 0;
    uint64_t ringOverruns = 0;
};

class WasapiLoopbackCapture {
public:
    WasapiLoopbackCapture(std::wstring endpointId, AudioFormat format, double ringMs, double bufferMs);
    WasapiLoopbackCapture(const WasapiLoopbackCapture&) = delete;
    WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;
    ~WasapiLoopbackCapture();

    void start();
    void stop();

    SpscRingBuffer<float>& ring() noexcept { return ring_; }
    const AudioFormat& format() const noexcept { return format_; }
    CaptureStatsSnapshot stats() const noexcept;

private:
    void threadProc();

    std::wstring endpointId_;
    AudioFormat format_;
    double bufferMs_ = 20.0;
    SpscRingBuffer<float> ring_;
    std::atomic<bool> running_ { false };
    std::thread thread_;

    std::atomic<uint64_t> framesCaptured_ { 0 };
    std::atomic<uint64_t> packets_ { 0 };
    std::atomic<uint64_t> silentPackets_ { 0 };
    std::atomic<uint64_t> ringOverruns_ { 0 };
};

} // namespace asx


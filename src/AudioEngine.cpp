#include "AudioEngine.h"

#include "Common.h"
#include "Mmcss.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace asx {

AudioEngine::AudioEngine(AudioEngineConfig config)
    : config_(std::move(config))
{
    DeviceEnumerator enumerator;
    sourceFormat_ = enumerator.getMixFormat(config_.source.id);
}

AudioEngine::~AudioEngine()
{
    stop();
}

void AudioEngine::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    notifications_ = std::make_unique<NotificationRegistration>();
    capture_ = std::make_unique<WasapiLoopbackCapture>(config_.source.id, sourceFormat_, config_.captureRingMs, config_.endpointBufferMs);
    capture_->start();

    outputs_.clear();
    outputs_.reserve(config_.outputs.size());
    for (const auto& endpoint : config_.outputs) {
        OutputDeviceConfig outConfig;
        outConfig.endpointId = endpoint.id;
        outConfig.name = endpoint.name;
        outConfig.sourceFormat = sourceFormat_;
        outConfig.preferExclusive = config_.preferExclusive;
        outConfig.ringMs = config_.outputRingMs;
        outConfig.bufferMs = config_.endpointBufferMs;

        auto output = std::make_unique<OutputDevice>(std::move(outConfig));
        output->start();
        outputs_.push_back(std::move(output));
    }

    for (auto& output : outputs_) {
        if (!output->waitUntilReady(8000)) {
            const std::string error = output->lastError();
            stop();
            throw std::runtime_error("Output endpoint failed to initialize: " + (error.empty() ? "timeout" : error));
        }
    }

    configureOutputTargets();

    const size_t chunkFrames = std::max<size_t>(64, sourceFormat_.sampleRate / 500);
    masterScratch_.resize(chunkFrames * sourceFormat_.channels);
    masterThread_ = std::thread(&AudioEngine::masterThreadProc, this);
    monitorThread_ = std::thread(&AudioEngine::monitorThreadProc, this);

    for (auto& output : outputs_) {
        output->releaseStartGate();
    }
}

void AudioEngine::stop()
{
    if (!running_.exchange(false)) {
        return;
    }

    if (masterThread_.joinable()) {
        masterThread_.join();
    }
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }

    if (capture_) {
        capture_->stop();
    }

    for (auto& output : outputs_) {
        output->stop();
    }
    outputs_.clear();
    notifications_.reset();
}

void AudioEngine::configureOutputTargets()
{
    size_t maxLatencyFrames = 0;
    for (const auto& output : outputs_) {
        maxLatencyFrames = std::max(maxLatencyFrames, output->streamLatencySourceFrames());
    }

    const size_t safetyFrames = std::max<size_t>(
        static_cast<size_t>((static_cast<double>(sourceFormat_.sampleRate) * config_.endpointBufferMs) / 1000.0),
        sourceFormat_.sampleRate / 250);

    for (auto& output : outputs_) {
        const size_t latency = output->streamLatencySourceFrames();
        const size_t target = safetyFrames + (maxLatencyFrames > latency ? maxLatencyFrames - latency : 0);
        output->setTargetFillFrames(target);
        output->seedSilence(target);
    }
}

void AudioEngine::masterThreadProc()
{
    try {
        MmcssScope mmcss;

        LARGE_INTEGER qpcFrequency {};
        LARGE_INTEGER lastQpc {};
        QueryPerformanceFrequency(&qpcFrequency);
        QueryPerformanceCounter(&lastQpc);

        const uint16_t channels = sourceFormat_.channels;
        const size_t maxFrames = masterScratch_.size() / channels;

        while (running_.load(std::memory_order_acquire)) {
            const size_t availableFrames = capture_->ring().readAvailable() / channels;
            if (availableFrames == 0) {
                LARGE_INTEGER now {};
                QueryPerformanceCounter(&now);
                const double elapsedMs = (static_cast<double>(now.QuadPart - lastQpc.QuadPart) * 1000.0)
                    / static_cast<double>(qpcFrequency.QuadPart);
                if (elapsedMs < 1.0) {
                    SwitchToThread();
                } else {
                    Sleep(1);
                    lastQpc = now;
                }
                continue;
            }

            const size_t frames = std::min(availableFrames, maxFrames);
            const size_t samples = frames * channels;
            const size_t popped = capture_->ring().pop(masterScratch_.data(), samples);
            const size_t poppedFrames = popped / channels;
            if (poppedFrames == 0) {
                continue;
            }

            for (auto& output : outputs_) {
                output->pushInput(masterScratch_.data(), poppedFrames);
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Master thread failed: " << ex.what() << "\n";
        running_.store(false, std::memory_order_release);
    }
}

void AudioEngine::monitorThreadProc()
{
    std::ofstream csv;
    if (!config_.debugCsvPath.empty()) {
        csv.open(config_.debugCsvPath, std::ios::out | std::ios::trunc);
        if (csv) {
            csv << "time_ms,device,exclusive,format,stream_latency_ms,ring_fill_ms,target_fill_ms,ppm,frames_rendered,underruns,input_overruns\n";
        }
    }

    const auto start = steadyNow();
    uint64_t lastGeneration = notifications_ ? notifications_->client().generation() : 0;

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const auto now = steadyNow();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        if (notifications_) {
            const uint64_t gen = notifications_->client().generation();
            if (gen != lastGeneration) {
                lastGeneration = gen;
                std::cerr << "Audio endpoint topology changed. Active streams will continue until WASAPI invalidates them.\n";
            }
        }

        if (config_.consoleMeter) {
            const auto captureStats = capture_->stats();
            std::ostringstream line;
            line << "[" << std::setw(6) << elapsedMs << " ms] captured=" << captureStats.framesCaptured
                 << " captureOverruns=" << captureStats.ringOverruns;
            std::cerr << line.str() << "\n";
        }

        for (const auto& output : outputs_) {
            const auto snap = output->snapshot();
            if (config_.consoleMeter) {
                std::cerr << "  " << narrow(snap.name)
                          << " mode=" << (snap.exclusive ? "exclusive" : "shared")
                          << " fill=" << std::fixed << std::setprecision(2) << snap.ringFillMs << "ms"
                          << " target=" << snap.targetFillMs << "ms"
                          << " ppm=" << std::showpos << std::setprecision(2) << snap.correctionPpm << std::noshowpos
                          << " underruns=" << snap.underruns
                          << " overruns=" << snap.inputOverruns
                          << "\n";
            }

            if (csv) {
                csv << elapsedMs << ",\"" << narrow(snap.name) << "\","
                    << (snap.exclusive ? 1 : 0) << ",\"" << snap.formatDescription << "\","
                    << snap.streamLatencyMs << "," << snap.ringFillMs << "," << snap.targetFillMs << ","
                    << snap.correctionPpm << "," << snap.framesRendered << "," << snap.underruns << ","
                    << snap.inputOverruns << "\n";
            }
        }

        if (csv) {
            csv.flush();
        }
    }
}

} // namespace asx


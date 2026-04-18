#include "WasapiRenderer.h"

#include "AudioSampleConvert.h"
#include "Common.h"
#include "Mmcss.h"

#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace asx {

static ComPtr<IAudioClient> activateAudioClient(IMMDevice& device)
{
    ComPtr<IAudioClient> client;
    checkHr(device.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "IMMDevice::Activate(IAudioClient render)");
    return client;
}

OutputDevice::OutputDevice(OutputDeviceConfig config)
    : config_(std::move(config))
{
    const auto frames = static_cast<size_t>((static_cast<double>(config_.sourceFormat.sampleRate) * config_.ringMs) / 1000.0);
    inputRing_.resetCapacity(std::max<size_t>(frames * config_.sourceFormat.channels, config_.sourceFormat.channels * 2048));
}

OutputDevice::~OutputDevice()
{
    stop();
}

void OutputDevice::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread(&OutputDevice::threadProc, this);
}

void OutputDevice::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    startReleased_.store(true, std::memory_order_release);
    stateCv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool OutputDevice::waitUntilReady(DWORD timeoutMs)
{
    std::unique_lock lock(stateMutex_);
    const bool completed = stateCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return ready_ || failed_;
    });
    return completed && ready_ && !failed_;
}

void OutputDevice::releaseStartGate()
{
    startReleased_.store(true, std::memory_order_release);
    stateCv_.notify_all();
}

void OutputDevice::setTargetFillFrames(size_t frames) noexcept
{
    sync_.setTargetFillFrames(frames);
    if (config_.sourceFormat.sampleRate) {
        targetFillMs_.store((static_cast<double>(frames) * 1000.0) / static_cast<double>(config_.sourceFormat.sampleRate),
            std::memory_order_release);
    }
}

void OutputDevice::pushInput(const float* frames, size_t frameCount)
{
    const size_t samples = frameCount * config_.sourceFormat.channels;
    if (samples == 0) {
        return;
    }

    const size_t available = inputRing_.writeAvailable();
    if (available < samples) {
        inputRing_.discard(samples - available);
        inputOverruns_.fetch_add(1, std::memory_order_acq_rel);
    }
    inputRing_.push(frames, samples);
}

void OutputDevice::seedSilence(size_t frameCount)
{
    const size_t chunkFrames = 512;
    silenceScratch_.assign(chunkFrames * config_.sourceFormat.channels, 0.0f);
    size_t remaining = frameCount;
    while (remaining > 0) {
        const size_t frames = std::min(remaining, chunkFrames);
        pushInput(silenceScratch_.data(), frames);
        remaining -= frames;
    }
}

size_t OutputDevice::streamLatencySourceFrames() const noexcept
{
    return streamLatencySourceFrames_.load(std::memory_order_acquire);
}

OutputDeviceSnapshot OutputDevice::snapshot() const
{
    OutputDeviceSnapshot out;
    out.name = config_.name;
    {
        std::scoped_lock lock(stateMutex_);
        out.ready = ready_;
        out.failed = failed_;
    }
    out.exclusive = exclusive_.load(std::memory_order_acquire);
    out.formatDescription = formatDescription_;
    out.streamLatencyMs = hnsToMs(static_cast<REFERENCE_TIME>(streamLatencyHns_.load(std::memory_order_acquire)));
    out.ringFillMs = ringFillMs_.load(std::memory_order_acquire);
    out.targetFillMs = targetFillMs_.load(std::memory_order_acquire);
    out.correctionPpm = sync_.correctionPpm();
    out.framesRendered = framesRendered_.load(std::memory_order_acquire);
    out.underruns = underruns_.load(std::memory_order_acquire);
    out.inputOverruns = inputOverruns_.load(std::memory_order_acquire);
    return out;
}

std::string OutputDevice::lastError() const
{
    std::scoped_lock lock(stateMutex_);
    return lastError_;
}

void OutputDevice::setReady(bool ready, bool failed, std::string error)
{
    {
        std::scoped_lock lock(stateMutex_);
        ready_ = ready;
        failed_ = failed;
        lastError_ = std::move(error);
    }
    stateCv_.notify_all();
}

bool OutputDevice::tryExclusive(IMMDevice& device, ClientSelection& selection)
{
    if (!config_.preferExclusive) {
        return false;
    }

    auto client = activateAudioClient(device);
    auto desired = makeFloatWaveFormat(config_.sourceFormat.sampleRate, config_.sourceFormat.channels, config_.sourceFormat.channelMask);

    HRESULT support = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, desired.get(), nullptr);
    if (support != S_OK) {
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    checkHr(client->GetDevicePeriod(&defaultPeriod, &minPeriod), "exclusive GetDevicePeriod");
    REFERENCE_TIME period = std::max(msToHns(config_.bufferMs), minPeriod);
    if (period <= 0) {
        period = defaultPeriod;
    }

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period, desired.get(), nullptr);
    if (FAILED(hr)) {
        return false;
    }

    selection.client = client;
    selection.format = std::move(desired);
    selection.parsedFormat = parseWaveFormat(*selection.format);
    selection.shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
    selection.exclusive = true;
    return true;
}

void OutputDevice::initializeShared(IMMDevice& device, ClientSelection& selection)
{
    auto client = activateAudioClient(device);
    auto desired = makeFloatWaveFormat(config_.sourceFormat.sampleRate, config_.sourceFormat.channels, config_.sourceFormat.channelMask);

    WAVEFORMATEX* closestRaw = nullptr;
    HRESULT support = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, desired.get(), &closestRaw);
    CoMemPtr<WAVEFORMATEX> closest(closestRaw);

    CoMemPtr<WAVEFORMATEX> chosen;
    if (support == S_OK) {
        chosen = std::move(desired);
    } else if (support == S_FALSE && closest) {
        chosen = std::move(closest);
    } else {
        WAVEFORMATEX* mixRaw = nullptr;
        checkHr(client->GetMixFormat(&mixRaw), "shared GetMixFormat");
        chosen.reset(mixRaw);
    }

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    checkHr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, msToHns(config_.bufferMs), 0, chosen.get(), nullptr),
        "shared render IAudioClient::Initialize");

    selection.client = client;
    selection.format = std::move(chosen);
    selection.parsedFormat = parseWaveFormat(*selection.format);
    selection.shareMode = AUDCLNT_SHAREMODE_SHARED;
    selection.exclusive = false;
}

OutputDevice::ClientSelection OutputDevice::initializeClient(IMMDevice& device)
{
    ClientSelection selection;
    if (!tryExclusive(device, selection)) {
        initializeShared(device, selection);
    }

    if (selection.parsedFormat.sampleType == SampleType::Unknown) {
        throw std::runtime_error("Endpoint returned an unsupported render format: " + describeFormat(selection.parsedFormat));
    }

    return selection;
}

void OutputDevice::fillEndpointBuffer(IAudioRenderClient& renderClient, UINT32 frames)
{
    if (frames == 0) {
        return;
    }

    BYTE* output = nullptr;
    checkHr(renderClient.GetBuffer(frames, &output), "render GetBuffer");

    const size_t currentFillFrames = inputRing_.readAvailable() / config_.sourceFormat.channels + resampler_.bufferedInputFrames();
    const bool previousLow = currentFillFrames < sync_.targetFillFrames() / 4;
    const double ppm = sync_.update(currentFillFrames, static_cast<double>(config_.sourceFormat.sampleRate), previousLow);
    resampler_.setCorrectionPpm(ppm);

    const size_t neededSamples = static_cast<size_t>(frames) * outputFormat_.channels;
    if (floatScratch_.size() < neededSamples) {
        floatScratch_.resize(neededSamples);
    }

    const ResampleResult result = resampler_.process(inputRing_, floatScratch_.data(), frames);
    if (result.underrun) {
        underruns_.fetch_add(1, std::memory_order_acq_rel);
    }

    convertFromFloat(floatScratch_.data(), frames, outputFormat_, output);
    checkHr(renderClient.ReleaseBuffer(frames, 0), "render ReleaseBuffer");

    framesRendered_.fetch_add(frames, std::memory_order_acq_rel);
    ringFillMs_.store((static_cast<double>(currentFillFrames) * 1000.0) / static_cast<double>(config_.sourceFormat.sampleRate),
        std::memory_order_release);
}

void OutputDevice::threadProc()
{
    try {
        ComApartment com;
        MmcssScope mmcss;

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
            "CoCreateInstance(MMDeviceEnumerator render)");

        ComPtr<IMMDevice> device;
        checkHr(enumerator->GetDevice(config_.endpointId.c_str(), &device), "IMMDeviceEnumerator::GetDevice(render)");

        ClientSelection selection = initializeClient(*device.Get());
        outputFormat_ = selection.parsedFormat;
        formatDescription_ = describeFormat(outputFormat_);
        exclusive_.store(selection.exclusive, std::memory_order_release);
        outputSampleRate_.store(outputFormat_.sampleRate, std::memory_order_release);
        outputChannels_.store(outputFormat_.channels, std::memory_order_release);

        UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event) {
            checkHr(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent(render)");
        }
        checkHr(selection.client->SetEventHandle(event.get()), "render SetEventHandle");

        UINT32 bufferFrames = 0;
        checkHr(selection.client->GetBufferSize(&bufferFrames), "render GetBufferSize");

        REFERENCE_TIME latency = 0;
        if (SUCCEEDED(selection.client->GetStreamLatency(&latency))) {
            streamLatencyHns_.store(static_cast<uint64_t>(latency), std::memory_order_release);
            const auto sourceFrames = static_cast<size_t>((hnsToMs(latency) * static_cast<double>(config_.sourceFormat.sampleRate)) / 1000.0);
            streamLatencySourceFrames_.store(sourceFrames, std::memory_order_release);
        }

        resampler_.configure(config_.sourceFormat.sampleRate, config_.sourceFormat.channels, outputFormat_.sampleRate, outputFormat_.channels);
        floatScratch_.resize(static_cast<size_t>(bufferFrames) * outputFormat_.channels);

        ComPtr<IAudioRenderClient> renderClient;
        checkHr(selection.client->GetService(IID_PPV_ARGS(&renderClient)), "GetService(IAudioRenderClient)");

        setReady(true, false);

        {
            std::unique_lock lock(stateMutex_);
            stateCv_.wait(lock, [this] {
                return startReleased_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire);
            });
        }

        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        fillEndpointBuffer(*renderClient.Get(), bufferFrames);
        checkHr(selection.client->Start(), "render IAudioClient::Start");

        while (running_.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(event.get(), 20);
            if (wait == WAIT_FAILED) {
                checkHr(HRESULT_FROM_WIN32(GetLastError()), "WaitForSingleObject(render)");
            }

            UINT32 padding = 0;
            HRESULT hr = selection.client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                checkHr(hr, "render GetCurrentPadding");
            }

            const UINT32 framesAvailable = bufferFrames > padding ? bufferFrames - padding : 0;
            if (framesAvailable > 0) {
                fillEndpointBuffer(*renderClient.Get(), framesAvailable);
            }
        }

        selection.client->Stop();
    } catch (const std::exception& ex) {
        std::cerr << "Render thread failed for " << narrow(config_.name) << ": " << ex.what() << "\n";
        setReady(false, true, ex.what());
        running_.store(false, std::memory_order_release);
    }
}

} // namespace asx


#include "SyncManager.h"

#include <algorithm>

namespace asx {

void SyncController::setTargetFillFrames(size_t frames) noexcept
{
    targetFillFrames_.store(frames, std::memory_order_release);
}

size_t SyncController::targetFillFrames() const noexcept
{
    return targetFillFrames_.load(std::memory_order_acquire);
}

double SyncController::update(size_t currentFillFrames, double sampleRate, bool underrun) noexcept
{
    const size_t target = targetFillFrames();
    if (target == 0 || sampleRate <= 0.0) {
        ppm_.store(0.0, std::memory_order_release);
        return 0.0;
    }

    const double errorFrames = static_cast<double>(currentFillFrames) - static_cast<double>(target);
    double rawPpm = errorFrames * 0.35;
    if (underrun) {
        rawPpm = -200.0;
    }
    rawPpm = std::clamp(rawPpm, -250.0, 250.0);

    const double previous = ppm_.load(std::memory_order_acquire);
    const double filtered = previous * 0.985 + rawPpm * 0.015;
    ppm_.store(filtered, std::memory_order_release);
    return filtered;
}

double SyncController::correctionPpm() const noexcept
{
    return ppm_.load(std::memory_order_acquire);
}

} // namespace asx


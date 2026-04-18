#pragma once

#include <atomic>
#include <cstddef>

namespace asx {

class SyncController {
public:
    void setTargetFillFrames(size_t frames) noexcept;
    size_t targetFillFrames() const noexcept;
    double update(size_t currentFillFrames, double sampleRate, bool underrun) noexcept;
    double correctionPpm() const noexcept;

private:
    std::atomic<size_t> targetFillFrames_ { 0 };
    std::atomic<double> ppm_ { 0.0 };
};

} // namespace asx


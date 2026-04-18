#pragma once

#include "SpscRingBuffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asx {

struct ResampleResult {
    size_t framesProduced = 0;
    bool underrun = false;
};

class AdaptiveResampler {
public:
    void configure(uint32_t inputRate, uint16_t inputChannels, uint32_t outputRate, uint16_t outputChannels);
    void setCorrectionPpm(double ppm) noexcept { correctionPpm_ = ppm; }
    ResampleResult process(SpscRingBuffer<float>& inputRing, float* output, size_t outputFrames);
    size_t bufferedInputFrames() const noexcept;

private:
    void pullInput(SpscRingBuffer<float>& inputRing, size_t targetFrames);
    float interpolatedChannel(size_t baseFrame, double frac, uint16_t sourceChannel) const noexcept;
    float mappedInterpolatedChannel(size_t baseFrame, double frac, uint16_t outputChannel) const noexcept;
    void discardConsumed();

    uint32_t inputRate_ = 0;
    uint16_t inputChannels_ = 0;
    uint32_t outputRate_ = 0;
    uint16_t outputChannels_ = 0;
    double baseRatio_ = 1.0;
    double readPos_ = 0.0;
    double correctionPpm_ = 0.0;
    std::vector<float> fifo_;
    std::vector<float> popScratch_;
};

} // namespace asx


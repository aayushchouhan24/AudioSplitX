#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace asx {

void AdaptiveResampler::configure(uint32_t inputRate, uint16_t inputChannels, uint32_t outputRate, uint16_t outputChannels)
{
    inputRate_ = inputRate;
    inputChannels_ = inputChannels;
    outputRate_ = outputRate;
    outputChannels_ = outputChannels;
    baseRatio_ = outputRate_ ? static_cast<double>(inputRate_) / static_cast<double>(outputRate_) : 1.0;
    readPos_ = 0.0;
    correctionPpm_ = 0.0;
    fifo_.clear();
    popScratch_.clear();
}

size_t AdaptiveResampler::bufferedInputFrames() const noexcept
{
    if (inputChannels_ == 0) {
        return 0;
    }
    const size_t frames = fifo_.size() / inputChannels_;
    const size_t consumed = static_cast<size_t>(std::floor(readPos_));
    return frames > consumed ? frames - consumed : 0;
}

void AdaptiveResampler::pullInput(SpscRingBuffer<float>& inputRing, size_t targetFrames)
{
    if (inputChannels_ == 0) {
        return;
    }

    const size_t have = fifo_.size() / inputChannels_;
    if (have >= targetFrames) {
        return;
    }

    const size_t availableFrames = inputRing.readAvailable() / inputChannels_;
    if (availableFrames == 0) {
        return;
    }

    const size_t toPull = std::min(availableFrames, targetFrames - have + 256);
    popScratch_.resize(toPull * inputChannels_);
    const size_t poppedSamples = inputRing.pop(popScratch_.data(), popScratch_.size());
    fifo_.insert(fifo_.end(), popScratch_.begin(), popScratch_.begin() + static_cast<std::ptrdiff_t>(poppedSamples));
}

float AdaptiveResampler::interpolatedChannel(size_t baseFrame, double frac, uint16_t sourceChannel) const noexcept
{
    const size_t a = baseFrame * inputChannels_ + sourceChannel;
    const size_t b = (baseFrame + 1) * inputChannels_ + sourceChannel;
    const float x0 = fifo_[a];
    const float x1 = fifo_[b];
    return static_cast<float>(static_cast<double>(x0) + (static_cast<double>(x1) - static_cast<double>(x0)) * frac);
}

float AdaptiveResampler::mappedInterpolatedChannel(size_t baseFrame, double frac, uint16_t outputChannel) const noexcept
{
    if (outputChannels_ == inputChannels_) {
        return interpolatedChannel(baseFrame, frac, outputChannel);
    }

    if (outputChannels_ == 1) {
        double sum = 0.0;
        for (uint16_t ch = 0; ch < inputChannels_; ++ch) {
            sum += interpolatedChannel(baseFrame, frac, ch);
        }
        return static_cast<float>(sum / static_cast<double>(inputChannels_));
    }

    if (inputChannels_ == 1) {
        return interpolatedChannel(baseFrame, frac, 0);
    }

    if (outputChannel < inputChannels_) {
        return interpolatedChannel(baseFrame, frac, outputChannel);
    }
    return 0.0f;
}

void AdaptiveResampler::discardConsumed()
{
    if (inputChannels_ == 0) {
        return;
    }

    const size_t consumed = static_cast<size_t>(std::floor(readPos_));
    if (consumed == 0) {
        return;
    }

    const size_t frames = fifo_.size() / inputChannels_;
    if (consumed >= frames) {
        fifo_.clear();
        readPos_ = 0.0;
        return;
    }

    const size_t samplesToDrop = consumed * inputChannels_;
    std::memmove(fifo_.data(), fifo_.data() + samplesToDrop, (fifo_.size() - samplesToDrop) * sizeof(float));
    fifo_.resize(fifo_.size() - samplesToDrop);
    readPos_ -= static_cast<double>(consumed);
}

ResampleResult AdaptiveResampler::process(SpscRingBuffer<float>& inputRing, float* output, size_t outputFrames)
{
    ResampleResult result;
    if (!output || outputFrames == 0 || inputChannels_ == 0 || outputChannels_ == 0) {
        return result;
    }

    const double ratio = baseRatio_ * (1.0 + correctionPpm_ * 0.000001);
    const auto needed = static_cast<size_t>(std::ceil(readPos_ + ratio * static_cast<double>(outputFrames + 1))) + 2;
    pullInput(inputRing, needed);

    size_t framesAvailable = fifo_.size() / inputChannels_;
    for (size_t frame = 0; frame < outputFrames; ++frame) {
        const size_t baseFrame = static_cast<size_t>(std::floor(readPos_));
        const double frac = readPos_ - static_cast<double>(baseFrame);
        float* out = output + frame * outputChannels_;

        if (framesAvailable < 2 || baseFrame + 1 >= framesAvailable) {
            std::fill(out, out + outputChannels_, 0.0f);
            result.underrun = true;
            continue;
        }

        for (uint16_t ch = 0; ch < outputChannels_; ++ch) {
            out[ch] = mappedInterpolatedChannel(baseFrame, frac, ch);
        }
        readPos_ += ratio;
        result.framesProduced++;

        if (baseFrame + 4 >= framesAvailable) {
            pullInput(inputRing, needed);
            framesAvailable = fifo_.size() / inputChannels_;
        }
    }

    discardConsumed();
    return result;
}

} // namespace asx


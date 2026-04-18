#include "AudioSampleConvert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace asx {

static float clampSample(float value)
{
    return std::clamp(value, -1.0f, 1.0f);
}

static int32_t read24(const uint8_t* p)
{
    int32_t value = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
    if (value & 0x00800000) {
        value |= static_cast<int32_t>(0xFF000000);
    }
    return value;
}

static void write24(uint8_t* p, int32_t value)
{
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

void convertToFloat(const uint8_t* input, size_t frames, const AudioFormat& format, float* output, bool silent)
{
    const size_t samples = frames * format.channels;
    if (silent || !input) {
        std::fill(output, output + samples, 0.0f);
        return;
    }

    switch (format.sampleType) {
    case SampleType::Float32:
        std::memcpy(output, input, samples * sizeof(float));
        break;

    case SampleType::Pcm16: {
        const auto* src = reinterpret_cast<const int16_t*>(input);
        for (size_t i = 0; i < samples; ++i) {
            output[i] = static_cast<float>(src[i]) / 32768.0f;
        }
        break;
    }

    case SampleType::Pcm24Packed: {
        for (size_t i = 0; i < samples; ++i) {
            output[i] = static_cast<float>(read24(input + i * 3)) / 8388608.0f;
        }
        break;
    }

    case SampleType::Pcm24In32:
    case SampleType::Pcm32: {
        const auto* src = reinterpret_cast<const int32_t*>(input);
        for (size_t i = 0; i < samples; ++i) {
            output[i] = static_cast<float>(src[i]) / 2147483648.0f;
        }
        break;
    }

    case SampleType::Unknown:
        std::fill(output, output + samples, 0.0f);
        break;
    }
}

void convertFromFloat(const float* input, size_t frames, const AudioFormat& format, uint8_t* output)
{
    const size_t samples = frames * format.channels;

    switch (format.sampleType) {
    case SampleType::Float32:
        std::memcpy(output, input, samples * sizeof(float));
        break;

    case SampleType::Pcm16: {
        auto* dst = reinterpret_cast<int16_t*>(output);
        for (size_t i = 0; i < samples; ++i) {
            const float x = clampSample(input[i]);
            dst[i] = static_cast<int16_t>(std::lrintf(x * 32767.0f));
        }
        break;
    }

    case SampleType::Pcm24Packed: {
        for (size_t i = 0; i < samples; ++i) {
            const float x = clampSample(input[i]);
            const int32_t v = static_cast<int32_t>(std::lrintf(x * 8388607.0f));
            write24(output + i * 3, v);
        }
        break;
    }

    case SampleType::Pcm24In32:
    case SampleType::Pcm32: {
        auto* dst = reinterpret_cast<int32_t*>(output);
        for (size_t i = 0; i < samples; ++i) {
            const float x = clampSample(input[i]);
            dst[i] = static_cast<int32_t>(std::llrint(x * 2147483647.0));
        }
        break;
    }

    case SampleType::Unknown:
        std::memset(output, 0, frames * format.bytesPerFrame());
        break;
    }
}

void mapChannels(const float* input, uint16_t inputChannels, size_t frames, uint16_t outputChannels, float* output)
{
    if (inputChannels == outputChannels) {
        std::memcpy(output, input, frames * inputChannels * sizeof(float));
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        const float* in = input + frame * inputChannels;
        float* out = output + frame * outputChannels;

        if (outputChannels == 1) {
            double sum = 0.0;
            for (uint16_t ch = 0; ch < inputChannels; ++ch) {
                sum += in[ch];
            }
            out[0] = static_cast<float>(sum / static_cast<double>(inputChannels));
            continue;
        }

        if (inputChannels == 1) {
            std::fill(out, out + outputChannels, in[0]);
            continue;
        }

        const uint16_t copyChannels = std::min(inputChannels, outputChannels);
        for (uint16_t ch = 0; ch < copyChannels; ++ch) {
            out[ch] = in[ch];
        }
        for (uint16_t ch = copyChannels; ch < outputChannels; ++ch) {
            out[ch] = 0.0f;
        }
    }
}

} // namespace asx


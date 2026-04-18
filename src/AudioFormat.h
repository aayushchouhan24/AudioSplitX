#pragma once

#include "Common.h"

#include <Audioclient.h>
#include <Mmreg.h>
#include <Windows.h>

#include <cstdint>
#include <string>

namespace asx {

enum class SampleType {
    Unknown,
    Float32,
    Pcm16,
    Pcm24Packed,
    Pcm24In32,
    Pcm32,
};

struct AudioFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t validBitsPerSample = 0;
    uint16_t blockAlign = 0;
    uint32_t avgBytesPerSec = 0;
    DWORD channelMask = 0;
    SampleType sampleType = SampleType::Unknown;

    size_t bytesPerFrame() const noexcept { return blockAlign; }
    bool isFloat32() const noexcept { return sampleType == SampleType::Float32; }
};

AudioFormat parseWaveFormat(const WAVEFORMATEX& wave);
CoMemPtr<WAVEFORMATEX> cloneWaveFormat(const WAVEFORMATEX& wave);
CoMemPtr<WAVEFORMATEX> makeFloatWaveFormat(uint32_t sampleRate, uint16_t channels, DWORD channelMask = 0);
std::string describeFormat(const AudioFormat& format);
DWORD defaultChannelMask(uint16_t channels);

} // namespace asx


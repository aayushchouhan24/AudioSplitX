#include "AudioFormat.h"

#include <Ksmedia.h>

#include <sstream>

namespace asx {

DWORD defaultChannelMask(uint16_t channels)
{
    switch (channels) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default:
        return 0;
    }
}

static SampleType classifyPcm(uint16_t bitsPerSample, uint16_t validBitsPerSample, uint16_t blockAlign, uint16_t channels)
{
    if (bitsPerSample == 16) {
        return SampleType::Pcm16;
    }
    if (bitsPerSample == 24 && blockAlign == channels * 3) {
        return SampleType::Pcm24Packed;
    }
    if (bitsPerSample == 32 && validBitsPerSample == 24) {
        return SampleType::Pcm24In32;
    }
    if (bitsPerSample == 32) {
        return SampleType::Pcm32;
    }
    return SampleType::Unknown;
}

AudioFormat parseWaveFormat(const WAVEFORMATEX& wave)
{
    AudioFormat out;
    out.sampleRate = wave.nSamplesPerSec;
    out.channels = wave.nChannels;
    out.bitsPerSample = wave.wBitsPerSample;
    out.validBitsPerSample = wave.wBitsPerSample;
    out.blockAlign = wave.nBlockAlign;
    out.avgBytesPerSec = wave.nAvgBytesPerSec;
    out.channelMask = defaultChannelMask(wave.nChannels);

    if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wave.wBitsPerSample == 32) {
        out.sampleType = SampleType::Float32;
        return out;
    }

    if (wave.wFormatTag == WAVE_FORMAT_PCM) {
        out.sampleType = classifyPcm(wave.wBitsPerSample, wave.wBitsPerSample, wave.nBlockAlign, wave.nChannels);
        return out;
    }

    if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        out.validBitsPerSample = ext.Samples.wValidBitsPerSample ? ext.Samples.wValidBitsPerSample : wave.wBitsPerSample;
        out.channelMask = ext.dwChannelMask ? ext.dwChannelMask : defaultChannelMask(wave.nChannels);

        if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wave.wBitsPerSample == 32) {
            out.sampleType = SampleType::Float32;
            return out;
        }

        if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            out.sampleType = classifyPcm(wave.wBitsPerSample, out.validBitsPerSample, wave.nBlockAlign, wave.nChannels);
            return out;
        }
    }

    out.sampleType = SampleType::Unknown;
    return out;
}

CoMemPtr<WAVEFORMATEX> cloneWaveFormat(const WAVEFORMATEX& wave)
{
    const size_t size = sizeof(WAVEFORMATEX) + wave.cbSize;
    auto* raw = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(size));
    if (!raw) {
        throw std::bad_alloc();
    }
    std::memcpy(raw, &wave, size);
    return CoMemPtr<WAVEFORMATEX>(raw);
}

CoMemPtr<WAVEFORMATEX> makeFloatWaveFormat(uint32_t sampleRate, uint16_t channels, DWORD channelMask)
{
    auto* ext = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!ext) {
        throw std::bad_alloc();
    }

    std::memset(ext, 0, sizeof(WAVEFORMATEXTENSIBLE));
    ext->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext->Format.nChannels = channels;
    ext->Format.nSamplesPerSec = sampleRate;
    ext->Format.wBitsPerSample = 32;
    ext->Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    ext->Format.nAvgBytesPerSec = sampleRate * ext->Format.nBlockAlign;
    ext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext->Samples.wValidBitsPerSample = 32;
    ext->dwChannelMask = channelMask ? channelMask : defaultChannelMask(channels);
    ext->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return CoMemPtr<WAVEFORMATEX>(&ext->Format);
}

std::string describeFormat(const AudioFormat& format)
{
    const char* type = "unknown";
    switch (format.sampleType) {
    case SampleType::Float32:
        type = "float32";
        break;
    case SampleType::Pcm16:
        type = "pcm16";
        break;
    case SampleType::Pcm24Packed:
        type = "pcm24";
        break;
    case SampleType::Pcm24In32:
        type = "pcm24-in-32";
        break;
    case SampleType::Pcm32:
        type = "pcm32";
        break;
    case SampleType::Unknown:
        break;
    }

    std::ostringstream oss;
    oss << format.sampleRate << " Hz, " << format.channels << " ch, " << type
        << ", block " << format.blockAlign;
    return oss.str();
}

} // namespace asx


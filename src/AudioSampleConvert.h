#pragma once

#include "AudioFormat.h"

#include <cstddef>
#include <cstdint>

namespace asx {

void convertToFloat(const uint8_t* input, size_t frames, const AudioFormat& format, float* output, bool silent);
void convertFromFloat(const float* input, size_t frames, const AudioFormat& format, uint8_t* output);
void mapChannels(const float* input, uint16_t inputChannels, size_t frames, uint16_t outputChannels, float* output);

} // namespace asx


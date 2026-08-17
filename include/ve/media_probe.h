#pragma once

#include "ve/media_time.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ve {

enum class MediaStreamKind { Video, Audio, Other };

struct MediaStreamInfo {
    std::int32_t index{0};
    MediaStreamKind kind{MediaStreamKind::Other};
    std::string codec;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t averageRateNumerator{0};
    std::int32_t averageRateDenominator{1};
    std::int32_t nominalRateNumerator{0};
    std::int32_t nominalRateDenominator{1};
    std::int32_t sampleRate{0};
    std::int32_t channels{0};
    std::int32_t rotationDegrees{0};
    std::int64_t frameCount{0};
    std::string pixelFormat;
    std::string colorSpace;
    std::string channelLayout;
    std::string timeBase;
    MediaTime duration{0, 1'000'000, 1};
};

struct MediaProbeResult {
    // FFprobe seconds represented exactly at microsecond precision.
    MediaTime duration{0, 1'000'000, 1};
    std::vector<MediaStreamInfo> streams;
    std::string formatName;
    std::int64_t bitRate{0};
    bool variableFrameRate{false};
    bool proxyRecommended{false};
    std::vector<std::string> warnings;
};

[[nodiscard]] MediaProbeResult parseFfprobeJson(std::string_view document);

} // namespace ve

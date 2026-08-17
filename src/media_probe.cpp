#include "ve/media_probe.h"

#include "json.h"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ve {
namespace {

std::int64_t integerString(std::string_view value) {
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid numeric FFprobe field");
    }
    return result;
}

std::pair<std::int32_t, std::int32_t> rate(std::string_view value) {
    const auto separator = value.find('/');
    if (separator == std::string_view::npos) return {0, 1};
    const auto numerator = integerString(value.substr(0, separator));
    const auto denominator = integerString(value.substr(separator + 1));
    if (numerator <= 0 || denominator <= 0 || numerator > INT32_MAX || denominator > INT32_MAX) {
        return {0, 1};
    }
    return {static_cast<std::int32_t>(numerator), static_cast<std::int32_t>(denominator)};
}

MediaTime duration(std::string_view value) {
    bool negative = false;
    if (!value.empty() && value.front() == '-') { negative = true; value.remove_prefix(1); }
    const auto separator = value.find('.');
    const auto whole = integerString(separator == std::string_view::npos ? value
                                                                         : value.substr(0, separator));
    std::int64_t fraction = 0;
    std::int64_t scale = 100'000;
    if (separator != std::string_view::npos) {
        const auto decimals = value.substr(separator + 1);
        for (std::size_t index = 0; index < 6; ++index) {
            if (index < decimals.size()) {
                const char digit = decimals[index];
                if (digit < '0' || digit > '9') throw std::runtime_error("invalid FFprobe duration");
                fraction += static_cast<std::int64_t>(digit - '0') * scale;
            }
            scale /= 10;
        }
    }
    auto units = whole * 1'000'000 + fraction;
    if (negative) units = -units;
    return {units, 1'000'000, 1};
}

std::string stringValue(const json::Value& object, std::string_view name,
                        std::string fallback = {}) {
    const auto* value = object.find(name);
    return value ? value->string() : std::move(fallback);
}

std::int32_t intValue(const json::Value& object, std::string_view name,
                      std::int32_t fallback = 0) {
    const auto* value = object.find(name);
    return value ? static_cast<std::int32_t>(value->integer()) : fallback;
}

bool sameRate(const MediaStreamInfo& stream) {
    if (stream.averageRateNumerator == 0 || stream.nominalRateNumerator == 0) return true;
    return static_cast<std::int64_t>(stream.averageRateNumerator) * stream.nominalRateDenominator ==
           static_cast<std::int64_t>(stream.nominalRateNumerator) * stream.averageRateDenominator;
}

} // namespace

MediaProbeResult parseFfprobeJson(std::string_view document) {
    const auto root = json::parse(document);
    MediaProbeResult result;
    if (const auto* format = root.find("format")) {
        const auto value = stringValue(*format, "duration");
        if (!value.empty() && value != "N/A") result.duration = duration(value);
        result.formatName = stringValue(*format, "format_name");
        const auto bitRate = stringValue(*format, "bit_rate");
        if (!bitRate.empty() && bitRate != "N/A") result.bitRate = integerString(bitRate);
    }
    const auto* streams = root.find("streams");
    if (!streams) throw std::runtime_error("FFprobe response has no streams");
    for (const auto& value : streams->array()) {
        MediaStreamInfo stream;
        stream.index = intValue(value, "index");
        const auto type = stringValue(value, "codec_type");
        stream.kind = type == "video" ? MediaStreamKind::Video
                     : type == "audio" ? MediaStreamKind::Audio
                                       : MediaStreamKind::Other;
        stream.codec = stringValue(value, "codec_name", "unknown");
        stream.width = intValue(value, "width");
        stream.height = intValue(value, "height");
        const auto [averageNumerator, averageDenominator] = rate(stringValue(value, "avg_frame_rate"));
        stream.averageRateNumerator = averageNumerator;
        stream.averageRateDenominator = averageDenominator;
        const auto [nominalNumerator, nominalDenominator] = rate(stringValue(value, "r_frame_rate"));
        stream.nominalRateNumerator = nominalNumerator;
        stream.nominalRateDenominator = nominalDenominator;
        const auto sampleRate = stringValue(value, "sample_rate");
        if (!sampleRate.empty()) stream.sampleRate = static_cast<std::int32_t>(integerString(sampleRate));
        stream.channels = intValue(value, "channels");
        const auto frameCount = stringValue(value, "nb_frames");
        if (!frameCount.empty() && frameCount != "N/A") stream.frameCount = integerString(frameCount);
        stream.channelLayout = stringValue(value, "channel_layout");
        stream.timeBase = stringValue(value, "time_base");
        const auto streamDuration = stringValue(value, "duration");
        if (!streamDuration.empty() && streamDuration != "N/A") {
            stream.duration = duration(streamDuration);
            if (result.duration.units == 0 || result.duration < stream.duration) {
                result.duration = stream.duration;
            }
        }
        stream.pixelFormat = stringValue(value, "pix_fmt");
        stream.colorSpace = stringValue(value, "color_space");
        if (const auto* tags = value.find("tags")) {
            const auto rotation = stringValue(*tags, "rotate");
            if (!rotation.empty()) stream.rotationDegrees = static_cast<std::int32_t>(integerString(rotation));
        }
        if (const auto* sideData = value.find("side_data_list")) {
            for (const auto& side : sideData->array()) {
                if (const auto* rotation = side.find("rotation"))
                    stream.rotationDegrees = static_cast<std::int32_t>(rotation->number());
            }
        }
        if (stream.kind == MediaStreamKind::Video) {
            const bool vfr = !sameRate(stream);
            result.variableFrameRate = result.variableFrameRate || vfr;
            result.proxyRecommended = result.proxyRecommended || stream.width > 1920 ||
                stream.height > 1080 || stream.codec == "hevc" || stream.codec == "h265" || vfr;
            if (stream.averageRateNumerator == 0)
                result.warnings.push_back("Video stream has no usable average frame rate");
        }
        result.streams.push_back(std::move(stream));
    }
    if (result.streams.empty()) throw std::runtime_error("FFprobe response contains no streams");
    return result;
}

} // namespace ve

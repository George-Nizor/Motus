#pragma once

#include "ve/project.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ve {

// The first executable media slice intentionally supports one gapless visible video lane and,
// optionally, its exactly mirrored unmuted audio lane. Rejecting everything else prevents an
// export from silently dropping edits the renderer does not understand yet.
struct SimpleMediaSegment {
    Id clipId;
    Id assetId;
    std::filesystem::path sourcePath;
    std::int64_t timelineStartUs{0};
    std::int64_t sourceInUs{0};
    std::int64_t durationUs{0};
};

struct SimpleTimelinePlan {
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t frameRateNumerator{0};
    std::int32_t frameRateDenominator{1};
    std::int32_t audioSampleRate{0};
    std::int32_t audioChannels{0};
    bool hasAudio{false};
    std::int64_t durationUs{0};
    std::vector<SimpleMediaSegment> segments;
};

[[nodiscard]] SimpleTimelinePlan buildSimpleTimelinePlan(const Project& project,
                                                         const Sequence& sequence);

// Returns argv only (the caller chooses the bundled ffmpeg executable). The output is H.264/AAC
// MP4 at the project profile, rendered from original files with an atomic staging path supplied
// by the caller.
[[nodiscard]] std::vector<std::string> buildFfmpegExportArguments(
    const SimpleTimelinePlan& plan, const std::filesystem::path& stagedOutputPath);

[[nodiscard]] std::filesystem::path stagedExportPath(const std::filesystem::path& outputPath);

// FFmpeg's machine-readable progress protocol reports out_time_us/out_time_ms in microseconds.
[[nodiscard]] std::optional<std::int64_t> parseFfmpegProgressMicroseconds(std::string_view line);

} // namespace ve

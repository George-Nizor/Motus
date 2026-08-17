#include "ve/native_media.h"

#include "ve/media_integrity.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ve {
namespace {

std::int64_t microseconds(const MediaTime& value, Rounding rounding = Rounding::Nearest) {
    return value.rescaled(1'000'000, 1, rounding).units;
}

bool sameEdit(const Clip& video, const Clip& audio) {
    return !video.linkedGroupId.empty() && video.linkedGroupId == audio.linkedGroupId &&
           video.assetId == audio.assetId && video.timelineStart == audio.timelineStart &&
           video.sourceIn == audio.sourceIn && video.duration == audio.duration &&
           std::abs(video.speed - audio.speed) < 0.000001;
}

std::string seconds(std::int64_t value) {
    const bool negative = value < 0;
    const auto magnitude = negative ? -value : value;
    std::ostringstream output;
    if (negative) output << '-';
    output << magnitude / 1'000'000 << '.' << std::setw(6) << std::setfill('0')
           << magnitude % 1'000'000;
    return output.str();
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void validateClipFeatures(const Clip& clip) {
    if (std::abs(clip.speed - 1.0) > 0.000001) {
        throw std::runtime_error("simple export does not support clip speed changes");
    }
    if (!clip.effects.empty()) {
        throw std::runtime_error("simple export does not support clip effects");
    }
    if (clip.audioFadeIn.units != 0 || clip.audioFadeOut.units != 0) {
        throw std::runtime_error("simple export does not support audio fades");
    }
}

} // namespace

SimpleTimelinePlan buildSimpleTimelinePlan(const Project& project, const Sequence& sequence) {
    project.validate();
    if (!sequence.transitions.empty()) {
        throw std::runtime_error("simple export does not support transitions");
    }
    if (project.profile.width <= 0 || project.profile.height <= 0 ||
        project.profile.width % 2 != 0 || project.profile.height % 2 != 0) {
        throw std::runtime_error("simple export requires an even-sized video profile");
    }

    const Track* videoTrack = nullptr;
    const Track* audioTrack = nullptr;
    for (const auto& track : sequence.tracks) {
        if (track.clips.empty()) continue;
        if (track.kind == TrackKind::Video && track.visible) {
            if (videoTrack) throw std::runtime_error("simple export supports one visible video track");
            videoTrack = &track;
        } else if (track.kind == TrackKind::Audio && !track.muted) {
            if (audioTrack) throw std::runtime_error("simple export supports one unmuted audio track");
            audioTrack = &track;
        }
    }
    if (!videoTrack || videoTrack->clips.empty()) {
        throw std::runtime_error("simple export requires a visible video track with clips");
    }
    if (audioTrack && audioTrack->clips.size() != videoTrack->clips.size()) {
        throw std::runtime_error("simple export requires audio edits to mirror the video lane");
    }

    SimpleTimelinePlan plan;
    plan.width = project.profile.width;
    plan.height = project.profile.height;
    plan.frameRateNumerator = project.profile.frameRateNumerator;
    plan.frameRateDenominator = project.profile.frameRateDenominator;
    plan.audioSampleRate = project.profile.audioSampleRate;
    plan.audioChannels = project.profile.audioChannels;
    plan.hasAudio = audioTrack != nullptr;

    MediaTime cursor = MediaTime::frames(0, project.profile.frameRateNumerator,
                                         project.profile.frameRateDenominator);
    for (std::size_t index = 0; index < videoTrack->clips.size(); ++index) {
        const auto& clip = videoTrack->clips[index];
        validateClipFeatures(clip);
        if (audioTrack) {
            const auto& audio = audioTrack->clips[index];
            validateClipFeatures(audio);
            if (!sameEdit(clip, audio)) {
                throw std::runtime_error("simple export requires linked audio/video edits to match");
            }
        }
        if (clip.timelineStart != cursor) {
            throw std::runtime_error("simple export requires a gapless rough cut starting at zero");
        }
        const auto timelineStartUs = microseconds(clip.timelineStart);
        const auto timelineEndUs = microseconds(clip.timelineEnd());
        const auto sourceInUs = microseconds(clip.sourceIn, Rounding::Down);
        const auto sourceOutUs = microseconds(clip.sourceOut(), Rounding::Down);
        const auto durationUs = std::min(timelineEndUs - timelineStartUs,
                                         sourceOutUs - sourceInUs);
        if (sourceInUs < 0 || durationUs <= 0) {
            throw std::runtime_error("simple export encountered an invalid source range");
        }
        const auto* asset = project.findAsset(clip.assetId);
        if (!asset) throw std::runtime_error("simple export references a missing asset record");
        if (asset->status != AssetStatus::Online) {
            throw std::runtime_error("simple export requires every source to be online");
        }
        if (!asset->probe || !asset->hasVideo) {
            throw std::runtime_error("simple export requires durable FFprobe metadata for every video source");
        }
        if (audioTrack && !asset->hasAudio) {
            throw std::runtime_error("simple export audio lane references a source without audio");
        }
        if (clip.sourceOut() > asset->duration) {
            throw std::runtime_error("simple export clip extends beyond its probed source duration");
        }
        const auto path = resolveAssetPath(project, *asset);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            throw std::runtime_error("simple export source file is missing or unreadable");
        }
        plan.segments.push_back({clip.id, clip.assetId, path, timelineStartUs, sourceInUs,
                                 durationUs});
        cursor = clip.timelineEnd();
    }
    plan.durationUs = microseconds(cursor);
    return plan;
}

std::vector<std::string> buildFfmpegExportArguments(
    const SimpleTimelinePlan& plan, const std::filesystem::path& stagedOutputPath) {
    if (plan.segments.empty() || plan.durationUs <= 0) {
        throw std::invalid_argument("FFmpeg export plan is empty");
    }
    if (stagedOutputPath.empty()) throw std::invalid_argument("FFmpeg output path is empty");
    if (plan.hasAudio && plan.audioChannels != 1 && plan.audioChannels != 2) {
        throw std::runtime_error("simple export currently supports mono or stereo output");
    }

    std::vector<std::string> arguments{"-hide_banner", "-y"};
    for (const auto& segment : plan.segments) {
        arguments.emplace_back("-i");
        arguments.push_back(utf8Path(segment.sourcePath));
    }

    std::ostringstream filter;
    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        const auto& segment = plan.segments[index];
        filter << '[' << index << ":v:0]trim=start=" << seconds(segment.sourceInUs)
               << ":duration=" << seconds(segment.durationUs)
               << ",setpts=PTS-STARTPTS,scale=" << plan.width << ':' << plan.height
               << ":force_original_aspect_ratio=decrease,pad=" << plan.width << ':' << plan.height
               << ":(ow-iw)/2:(oh-ih)/2,setsar=1,fps=" << plan.frameRateNumerator << '/'
               << plan.frameRateDenominator << ",format=yuv420p[v" << index << "];";
        if (plan.hasAudio) {
            filter << '[' << index << ":a:0]atrim=start=" << seconds(segment.sourceInUs)
                   << ":duration=" << seconds(segment.durationUs)
                   << ",asetpts=PTS-STARTPTS,aresample=" << plan.audioSampleRate
                   << ",aformat=channel_layouts=" << (plan.audioChannels == 1 ? "mono" : "stereo")
                   << "[a" << index << "];";
        }
    }
    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        filter << "[v" << index << ']';
        if (plan.hasAudio) filter << "[a" << index << ']';
    }
    filter << "concat=n=" << plan.segments.size() << ":v=1:a=" << (plan.hasAudio ? 1 : 0)
           << "[v]" << (plan.hasAudio ? "[a]" : "");

    arguments.emplace_back("-filter_complex");
    arguments.push_back(filter.str());
    arguments.insert(arguments.end(), {"-map", "[v]"});
    if (plan.hasAudio) arguments.insert(arguments.end(), {"-map", "[a]"});
    arguments.insert(arguments.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", "18",
                                       "-pix_fmt", "yuv420p"});
    if (plan.hasAudio) {
        arguments.insert(arguments.end(), {"-c:a", "aac", "-b:a", "192k"});
    }
    arguments.insert(arguments.end(), {"-movflags", "+faststart", "-progress", "pipe:1",
                                       "-nostats", "-loglevel", "warning",
                                       utf8Path(stagedOutputPath)});
    return arguments;
}

std::filesystem::path stagedExportPath(const std::filesystem::path& outputPath) {
    if (outputPath.empty()) throw std::invalid_argument("export output path is empty");
    const auto extension = outputPath.extension().empty() ? std::filesystem::path(".mp4")
                                                          : outputPath.extension();
    auto staged = outputPath.parent_path() / outputPath.stem();
    staged += ".motus-partial";
    staged += extension;
    return staged;
}

std::optional<std::int64_t> parseFfmpegProgressMicroseconds(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    constexpr std::string_view outTimeUs = "out_time_us=";
    constexpr std::string_view outTimeMs = "out_time_ms="; // FFmpeg names this ms but reports us.
    if (line.starts_with(outTimeUs)) line.remove_prefix(outTimeUs.size());
    else if (line.starts_with(outTimeMs)) line.remove_prefix(outTimeMs.size());
    else return std::nullopt;
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), value);
    if (error != std::errc{} || end != line.data() + line.size() || value < 0) return std::nullopt;
    return value;
}

} // namespace ve

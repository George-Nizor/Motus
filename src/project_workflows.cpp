#include "ve/project_workflows.h"

#include "ve/media_integrity.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace ve {
namespace {

MediaTime sequenceEnd(const Sequence& sequence, const ProjectProfile& profile) {
    MediaTime end = MediaTime::frames(0, profile.frameRateNumerator, profile.frameRateDenominator);
    for (const auto& track : sequence.tracks)
        for (const auto& clip : track.clips) end = std::max(end, clip.timelineEnd());
    return end;
}

std::pair<bool, bool> streamKinds(const MediaProbeResult& probe) {
    bool video = false;
    bool audio = false;
    for (const auto& stream : probe.streams) {
        video = video || stream.kind == MediaStreamKind::Video;
        audio = audio || stream.kind == MediaStreamKind::Audio;
    }
    return {video, audio};
}

MediaTime probedDuration(const ProjectProfile& profile, const MediaProbeResult& probe) {
    const auto frames = probe.duration.rescaled(profile.frameRateNumerator,
                                                profile.frameRateDenominator,
                                                Rounding::Up).units;
    if (frames <= 0) throw std::invalid_argument("media probe reported no usable duration");
    return MediaTime::frames(frames, profile.frameRateNumerator, profile.frameRateDenominator);
}

std::int64_t nowUtcMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

Project makeNewProject(std::string name, const ProjectProfile& profile) {
    Project project;
    project.id = makeId();
    project.name = std::move(name);
    project.profile = profile;
    Sequence sequence;
    sequence.id = makeId();
    sequence.name = "Rough Cut";
    sequence.tracks.push_back({makeId(), "Video 1", TrackKind::Video,
                               false, false, true, {}});
    sequence.tracks.push_back({makeId(), "Audio 1", TrackKind::Audio,
                               false, false, true, {}});
    project.activeSequenceId = sequence.id;
    project.sequences.push_back(std::move(sequence));
    project.validate();
    return project;
}

AppendedMedia appendMediaReference(Project& project, const std::filesystem::path& mediaPath,
                                   std::int64_t durationFrames, std::string displayName) {
    if (!std::filesystem::is_regular_file(mediaPath)) {
        throw std::invalid_argument("media file does not exist");
    }
    if (durationFrames <= 0) throw std::invalid_argument("durationFrames must be positive");
    if (!project.activeSequenceId) throw std::runtime_error("project has no active sequence");
    auto* sequence = project.findSequence(*project.activeSequenceId);
    if (!sequence || sequence->tracks.size() < 2) {
        throw std::runtime_error("project has no linked A/V tracks");
    }
    const auto videoTrack = std::ranges::find(sequence->tracks, TrackKind::Video, &Track::kind);
    const auto audioTrack = std::ranges::find(sequence->tracks, TrackKind::Audio, &Track::kind);
    if (videoTrack == sequence->tracks.end() || audioTrack == sequence->tracks.end()) {
        throw std::runtime_error("project has no linked A/V tracks");
    }

    AppendedMedia result{makeId(), makeId(), makeId(), makeId()};
    const auto start = sequenceEnd(*sequence, project.profile);
    const auto duration = MediaTime::frames(durationFrames, project.profile.frameRateNumerator,
                                            project.profile.frameRateDenominator);
    const auto requestedDisplayName = displayName;
    Asset asset;
    asset.id = result.assetId;
    asset.path = mediaPath;
    asset.displayName = std::move(displayName);
    asset.status = AssetStatus::Online;
    asset.duration = duration;
    asset.hasVideo = true;
    asset.hasAudio = true;
    asset.proxyEligible = true;
    project.assets.push_back(std::move(asset));
    try {
        (void)relinkAsset(project, result.assetId, mediaPath);
        if (!requestedDisplayName.empty()) {
            project.assets.back().displayName = requestedDisplayName;
        }
    } catch (...) {
        project.assets.pop_back();
        throw;
    }
    Clip video{result.videoClipId, result.assetId, result.linkedGroupId, start,
        MediaTime::frames(0, project.profile.frameRateNumerator,
                          project.profile.frameRateDenominator),
        duration, MediaTime{}, MediaTime{}, 1.0, {}};
    Clip audio = video;
    audio.id = result.audioClipId;
    videoTrack->clips.push_back(std::move(video));
    audioTrack->clips.push_back(std::move(audio));
    ++project.revision;
    project.validate();
    return result;
}

void applyMediaProbe(Project& project, const Id& assetId, const MediaProbeResult& probe,
                     bool resizeLegacyFullLengthClips, std::string probeBackend) {
    auto* asset = project.findAsset(assetId);
    if (!asset) throw std::invalid_argument("asset does not exist");
    const auto [hasVideo, hasAudio] = streamKinds(probe);
    if (!hasVideo && !hasAudio) throw std::invalid_argument("media probe found no audio or video streams");
    const auto priorDuration = asset->duration;
    const auto duration = probedDuration(project.profile, probe);
    asset->duration = duration;
    asset->hasVideo = hasVideo;
    asset->hasAudio = hasAudio;
    asset->proxyEligible = probe.proxyRecommended;
    asset->probe = probe;
    asset->probedUtcMs = nowUtcMilliseconds();
    asset->probeBackend = std::move(probeBackend);
    asset->status = AssetStatus::Online;
    if (resizeLegacyFullLengthClips) {
        for (auto& sequence : project.sequences) {
            for (auto& track : sequence.tracks) {
                for (auto& clip : track.clips) {
                    if (clip.assetId == assetId && clip.sourceIn.units == 0 &&
                        clip.duration == priorDuration) {
                        clip.duration = duration;
                    }
                }
            }
        }
    }
    project.validate();
}

AppendedMedia appendProbedMediaReference(Project& project,
                                         const std::filesystem::path& mediaPath,
                                         const MediaProbeResult& probe,
                                         std::string displayName,
                                         std::string probeBackend) {
    const auto [hasVideo, hasAudio] = streamKinds(probe);
    if (!hasVideo && !hasAudio) throw std::invalid_argument("media probe found no audio or video streams");
    if (!std::filesystem::is_regular_file(mediaPath)) {
        throw std::invalid_argument("media file does not exist");
    }
    if (!project.activeSequenceId) throw std::runtime_error("project has no active sequence");
    auto* sequence = project.findSequence(*project.activeSequenceId);
    if (!sequence) throw std::runtime_error("project has no active sequence");
    auto videoTrack = std::ranges::find(sequence->tracks, TrackKind::Video, &Track::kind);
    auto audioTrack = std::ranges::find(sequence->tracks, TrackKind::Audio, &Track::kind);
    if ((hasVideo && videoTrack == sequence->tracks.end()) ||
        (hasAudio && audioTrack == sequence->tracks.end())) {
        throw std::runtime_error("project has no matching media track");
    }

    AppendedMedia result{makeId(), hasVideo ? makeId() : Id{}, hasAudio ? makeId() : Id{}, makeId()};
    const auto start = sequenceEnd(*sequence, project.profile);
    const auto duration = probedDuration(project.profile, probe);
    const auto requestedDisplayName = displayName;
    Asset asset;
    asset.id = result.assetId;
    asset.path = mediaPath;
    asset.displayName = std::move(displayName);
    asset.status = AssetStatus::Online;
    asset.duration = duration;
    asset.hasVideo = hasVideo;
    asset.hasAudio = hasAudio;
    asset.proxyEligible = probe.proxyRecommended;
    project.assets.push_back(std::move(asset));
    try {
        (void)relinkAsset(project, result.assetId, mediaPath);
        if (!requestedDisplayName.empty()) project.assets.back().displayName = requestedDisplayName;
        applyMediaProbe(project, result.assetId, probe, false, std::move(probeBackend));
    } catch (...) {
        project.assets.pop_back();
        throw;
    }
    Clip clip{"", result.assetId, result.linkedGroupId, start,
        MediaTime::frames(0, project.profile.frameRateNumerator,
                          project.profile.frameRateDenominator),
        duration, MediaTime{}, MediaTime{}, 1.0, {}};
    if (hasVideo) {
        clip.id = result.videoClipId;
        videoTrack->clips.push_back(clip);
    }
    if (hasAudio) {
        clip.id = result.audioClipId;
        audioTrack->clips.push_back(clip);
    }
    ++project.revision;
    project.validate();
    return result;
}

std::int64_t snapTimelineFrame(const Project& project, const Sequence& sequence,
                               std::int64_t inputFrame, std::int64_t thresholdFrames) {
    if (thresholdFrames < 0) throw std::invalid_argument("snap threshold must not be negative");
    const auto frame = [&](const MediaTime& time) {
        return time.rescaled(project.profile.frameRateNumerator,
                             project.profile.frameRateDenominator).units;
    };
    std::int64_t sequenceDuration = 0;
    std::int64_t best = inputFrame;
    std::int64_t bestDistance = thresholdFrames + 1;
    const auto consider = [&](std::int64_t candidate) {
        const auto distance = candidate > inputFrame ? candidate - inputFrame
                                                     : inputFrame - candidate;
        if (distance <= thresholdFrames && distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    };

    consider(0);
    for (const auto& track : sequence.tracks) {
        for (const auto& clip : track.clips) {
            const auto start = frame(clip.timelineStart);
            const auto end = frame(clip.timelineEnd());
            consider(start);
            consider(end);
            sequenceDuration = std::max(sequenceDuration, end);
        }
    }
    for (const auto& marker : sequence.markers) consider(frame(marker.time));
    consider(sequenceDuration);
    return best;
}

} // namespace ve

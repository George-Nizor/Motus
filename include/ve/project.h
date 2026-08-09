#pragma once

#include "ve/media_time.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

using Id = std::string;

[[nodiscard]] Id makeId();

struct ProjectProfile {
    std::int32_t width{3840};
    std::int32_t height{2160};
    std::int32_t frameRateNumerator{30};
    std::int32_t frameRateDenominator{1};
    std::int32_t audioSampleRate{48000};
    std::int32_t audioChannels{2};
    std::string colorSpace{"Rec.709 SDR"};
};

enum class AssetStatus { Online, Missing, Modified, Unsupported };

struct AssetFingerprint {
    std::uint64_t byteSize{0};
    std::int64_t modifiedUtcMs{0};
    std::string headTailSha256;

    [[nodiscard]] std::string cacheKey() const;
    bool operator==(const AssetFingerprint&) const = default;
};

struct Asset {
    Id id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> relativePath;
    std::string displayName;
    AssetFingerprint fingerprint;
    AssetStatus status{AssetStatus::Online};
    MediaTime duration;
    bool hasVideo{true};
    bool hasAudio{true};
    bool proxyEligible{false};
};

struct Keyframe {
    enum class Interpolation { Hold, Linear, Smooth };
    MediaTime time;
    double value{0.0};
    Interpolation interpolation{Interpolation::Linear};
};

struct EffectInstance {
    Id id;
    std::string service;
    bool enabled{true};
    std::unordered_map<std::string, std::string> parameters;
    std::unordered_map<std::string, std::vector<Keyframe>> animations;
};

struct Clip {
    Id id;
    Id assetId;
    Id linkedGroupId;
    MediaTime timelineStart;
    MediaTime sourceIn;
    MediaTime duration;
    MediaTime audioFadeIn;
    MediaTime audioFadeOut;
    double speed{1.0};
    std::vector<EffectInstance> effects;

    [[nodiscard]] MediaTime timelineEnd() const { return timelineStart + duration; }
    [[nodiscard]] MediaTime sourceOut() const { return sourceIn + duration; }
};

enum class TrackKind { Video, Audio };

struct Track {
    Id id;
    std::string name;
    TrackKind kind{TrackKind::Video};
    bool locked{false};
    bool muted{false};
    bool visible{true};
    std::vector<Clip> clips;
};

struct Transition {
    Id id;
    Id fromClipId;
    Id toClipId;
    std::string service;
    MediaTime duration;
};

struct Marker {
    Id id;
    MediaTime time;
    std::string label;
    std::string color;
};

struct Sequence {
    Id id;
    std::string name;
    std::vector<Track> tracks;
    std::vector<Transition> transitions;
    std::vector<Marker> markers;
};

struct TranscriptWord {
    std::string text;
    MediaTime start;
    MediaTime end;
    double confidence{0.0};
};

enum class CleanupKind { Silence, Filler, ContextualPhrase };
enum class SuggestionState { Pending, Accepted, Rejected, Skipped, Stale, ManualOnly };

struct CleanupSuggestion {
    Id id;
    Id assetId;
    Id sequenceId;
    CleanupKind kind{CleanupKind::Silence};
    SuggestionState state{SuggestionState::Pending};
    MediaTime sourceStart;
    MediaTime sourceEnd;
    MediaTime replacementDuration;
    double confidence{0.0};
    bool speechFreeHandles{false};
    std::string transcriptContext;
    std::string analysisCacheKey;
};

struct Project {
    static constexpr std::int32_t currentSchemaVersion = 2;

    std::int32_t schemaVersion{currentSchemaVersion};
    Id id;
    std::string name{"Untitled"};
    ProjectProfile profile;
    std::filesystem::path projectPath;
    std::vector<Asset> assets;
    std::vector<Sequence> sequences;
    std::vector<CleanupSuggestion> cleanupSuggestions;
    std::optional<Id> activeSequenceId;
    std::uint64_t revision{0};

    [[nodiscard]] Asset* findAsset(const Id& assetId);
    [[nodiscard]] const Asset* findAsset(const Id& assetId) const;
    [[nodiscard]] Sequence* findSequence(const Id& sequenceId);
    [[nodiscard]] const Sequence* findSequence(const Id& sequenceId) const;
    [[nodiscard]] Track* findTrack(const Id& trackId);
    [[nodiscard]] const Track* findTrack(const Id& trackId) const;
    [[nodiscard]] Clip* findClip(const Id& clipId);
    [[nodiscard]] const Clip* findClip(const Id& clipId) const;
    void validate() const;
};

} // namespace ve


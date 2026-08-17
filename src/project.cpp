#include "ve/project.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ve {

Id makeId() {
    static std::atomic<std::uint64_t> sequence{0};
    static const std::uint64_t seed = [] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32U) ^ device();
    }();
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << (seed ^ now) << std::setw(16)
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

std::string AssetFingerprint::cacheKey() const {
    return std::to_string(byteSize) + ":" + std::to_string(modifiedUtcMs) + ":" +
           headTailSha256;
}

Asset* Project::findAsset(const Id& assetId) {
    const auto iterator = std::ranges::find(assets, assetId, &Asset::id);
    return iterator == assets.end() ? nullptr : &*iterator;
}

const Asset* Project::findAsset(const Id& assetId) const {
    const auto iterator = std::ranges::find(assets, assetId, &Asset::id);
    return iterator == assets.end() ? nullptr : &*iterator;
}

Sequence* Project::findSequence(const Id& sequenceId) {
    const auto iterator = std::ranges::find(sequences, sequenceId, &Sequence::id);
    return iterator == sequences.end() ? nullptr : &*iterator;
}

const Sequence* Project::findSequence(const Id& sequenceId) const {
    const auto iterator = std::ranges::find(sequences, sequenceId, &Sequence::id);
    return iterator == sequences.end() ? nullptr : &*iterator;
}

Track* Project::findTrack(const Id& trackId) {
    for (auto& sequence : sequences) {
        const auto iterator = std::ranges::find(sequence.tracks, trackId, &Track::id);
        if (iterator != sequence.tracks.end()) return &*iterator;
    }
    return nullptr;
}

const Track* Project::findTrack(const Id& trackId) const {
    for (const auto& sequence : sequences) {
        const auto iterator = std::ranges::find(sequence.tracks, trackId, &Track::id);
        if (iterator != sequence.tracks.end()) return &*iterator;
    }
    return nullptr;
}

Clip* Project::findClip(const Id& clipId) {
    for (auto& sequence : sequences) {
        for (auto& track : sequence.tracks) {
            const auto iterator = std::ranges::find(track.clips, clipId, &Clip::id);
            if (iterator != track.clips.end()) return &*iterator;
        }
    }
    return nullptr;
}

const Clip* Project::findClip(const Id& clipId) const {
    for (const auto& sequence : sequences) {
        for (const auto& track : sequence.tracks) {
            const auto iterator = std::ranges::find(track.clips, clipId, &Clip::id);
            if (iterator != track.clips.end()) return &*iterator;
        }
    }
    return nullptr;
}

void Project::validate() const {
    if (schemaVersion != currentSchemaVersion) throw std::runtime_error("unsupported project schema");
    if (profile.width <= 0 || profile.height <= 0 || profile.frameRateNumerator <= 0 ||
        profile.frameRateDenominator <= 0 || profile.audioSampleRate <= 0) {
        throw std::runtime_error("invalid project profile");
    }
    std::unordered_set<Id> ids;
    const auto unique = [&ids](const Id& value, const char* type) {
        if (value.empty() || !ids.insert(value).second) {
            throw std::runtime_error(std::string("missing or duplicate ") + type + " id");
        }
    };
    unique(id, "project");
    for (const auto& asset : assets) unique(asset.id, "asset");
    for (const auto& sequence : sequences) {
        unique(sequence.id, "sequence");
        std::unordered_set<Id> sequenceClipIds;
        for (const auto& track : sequence.tracks) {
            unique(track.id, "track");
            MediaTime priorEnd{0, profile.frameRateNumerator, profile.frameRateDenominator};
            for (const auto& clip : track.clips) {
                unique(clip.id, "clip");
                sequenceClipIds.insert(clip.id);
                if (!findAsset(clip.assetId)) throw std::runtime_error("clip references missing asset");
                if (clip.duration.units <= 0) throw std::runtime_error("clip duration must be positive");
                if (clip.timelineStart < priorEnd) throw std::runtime_error("overlapping or unsorted clips");
                for (const auto& effect : clip.effects) unique(effect.id, "effect");
                priorEnd = clip.timelineEnd();
            }
        }
        for (const auto& transition : sequence.transitions) {
            unique(transition.id, "transition");
            if (!sequenceClipIds.contains(transition.fromClipId) ||
                !sequenceClipIds.contains(transition.toClipId)) {
                throw std::runtime_error("transition references a clip outside its sequence");
            }
        }
        for (const auto& marker : sequence.markers) unique(marker.id, "marker");
    }
    for (const auto& suggestion : cleanupSuggestions) unique(suggestion.id, "cleanup suggestion");
    if (activeSequenceId && !findSequence(*activeSequenceId)) {
        throw std::runtime_error("active sequence does not exist");
    }
}

} // namespace ve

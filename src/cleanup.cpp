#include "ve/cleanup.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ve {
namespace {

const PacingPreset relaxed{"Relaxed", MediaTime::samples(48000, 48000),
                           MediaTime::samples(19200, 48000)};
const PacingPreset natural{"Natural but brisk", MediaTime::samples(28800, 48000),
                           MediaTime::samples(11520, 48000)};
const PacingPreset tight{"Tight", MediaTime::samples(19200, 48000),
                         MediaTime::samples(5760, 48000)};

void renewSequenceIds(Sequence& sequence) {
    sequence.id = makeId();
    std::unordered_map<Id, Id> clipIds;
    std::unordered_map<Id, Id> linkedGroupIds;
    for (auto& track : sequence.tracks) {
        track.id = makeId();
        for (auto& clip : track.clips) {
            const auto oldClipId = clip.id;
            clip.id = makeId();
            clipIds.emplace(oldClipId, clip.id);
            if (!clip.linkedGroupId.empty()) {
                const auto [entry, inserted] = linkedGroupIds.try_emplace(clip.linkedGroupId);
                if (inserted) entry->second = makeId();
                clip.linkedGroupId = entry->second;
            }
            for (auto& effect : clip.effects) effect.id = makeId();
        }
    }
    for (auto& transition : sequence.transitions) {
        transition.id = makeId();
        const auto from = clipIds.find(transition.fromClipId);
        const auto to = clipIds.find(transition.toClipId);
        if (from == clipIds.end() || to == clipIds.end()) {
            throw std::runtime_error("transition references a clip outside its sequence");
        }
        transition.fromClipId = from->second;
        transition.toClipId = to->second;
    }
    for (auto& marker : sequence.markers) marker.id = makeId();
}

std::vector<TimeInterval> timelineRemovalRanges(const Sequence& sequence,
                                                 const CleanupSuggestion& suggestion) {
    const MediaTime total = suggestion.sourceEnd - suggestion.sourceStart;
    const MediaTime replacement = suggestion.replacementDuration;
    if (replacement >= total) return {};
    const auto removal = total - replacement;
    const auto leadingKeepUnits = replacement.units / 2;
    const MediaTime removalSourceStart = suggestion.sourceStart +
        MediaTime{leadingKeepUnits, replacement.rateNumerator, replacement.rateDenominator};
    const MediaTime removalSourceEnd = removalSourceStart + removal;

    std::vector<TimeInterval> ranges;
    for (const auto& track : sequence.tracks) {
        if (track.kind != TrackKind::Video) continue; // linked tracks are removed by ripple.
        for (const auto& clip : track.clips) {
            if (clip.assetId != suggestion.assetId) continue;
            const auto overlapStart = std::max(clip.sourceIn, removalSourceStart);
            const auto overlapEnd = std::min(clip.sourceOut(), removalSourceEnd);
            if (overlapEnd <= overlapStart) continue;
            ranges.push_back({clip.timelineStart + (overlapStart - clip.sourceIn),
                              clip.timelineStart + (overlapEnd - clip.sourceIn)});
        }
    }
    return mergeIntervals(std::move(ranges), MediaTime{});
}

} // namespace

const PacingPreset& relaxedPacing() { return relaxed; }
const PacingPreset& naturalBriskPacing() { return natural; }
const PacingPreset& tightPacing() { return tight; }

std::vector<TimeInterval> mergeIntervals(std::vector<TimeInterval> intervals,
                                         MediaTime maximumGap) {
    std::erase_if(intervals, [](const TimeInterval& value) { return value.end <= value.start; });
    std::ranges::sort(intervals, [](const TimeInterval& lhs, const TimeInterval& rhs) {
        return lhs.start < rhs.start;
    });
    std::vector<TimeInterval> merged;
    for (const auto& interval : intervals) {
        if (merged.empty() || interval.start > merged.back().end + maximumGap) {
            merged.push_back(interval);
        } else if (interval.end > merged.back().end) {
            merged.back().end = interval.end;
        }
    }
    return merged;
}

bool isBulkSafe(const CleanupSuggestion& suggestion) {
    return suggestion.state == SuggestionState::Pending && suggestion.speechFreeHandles &&
           suggestion.kind != CleanupKind::ContextualPhrase && suggestion.confidence >= 0.80 &&
           suggestion.sourceEnd > suggestion.sourceStart &&
           suggestion.replacementDuration < (suggestion.sourceEnd - suggestion.sourceStart);
}

Project buildCleanedProject(const Project& source, const Id& sequenceId,
                            const std::vector<Id>& suggestionIds, bool safeOnly) {
    const auto* original = source.findSequence(sequenceId);
    if (!original) throw std::invalid_argument("source sequence does not exist");
    Project output = source;
    Sequence cleaned = *original;
    renewSequenceIds(cleaned);
    cleaned.name = original->name + " — Cleaned";
    output.sequences.push_back(std::move(cleaned));
    output.activeSequenceId = output.sequences.back().id;

    const std::unordered_set<Id> selected(suggestionIds.begin(), suggestionIds.end());
    for (auto& suggestion : output.cleanupSuggestions) {
        if (!selected.contains(suggestion.id) || suggestion.sequenceId != sequenceId) continue;
        if (safeOnly && !isBulkSafe(suggestion)) continue;
        if (!safeOnly && suggestion.state != SuggestionState::Pending &&
            suggestion.state != SuggestionState::ManualOnly) {
            continue;
        }

        auto* target = output.findSequence(*output.activeSequenceId);
        auto ranges = timelineRemovalRanges(*target, suggestion);
        std::ranges::sort(ranges, [](const TimeInterval& lhs, const TimeInterval& rhs) {
            return lhs.start > rhs.start; // right-to-left preserves earlier timeline positions.
        });
        for (const auto& range : ranges) {
            auto command = makeRippleDeleteCommand(target->id, range.start, range.end);
            command->redo(output);
        }
        if (!ranges.empty()) suggestion.state = SuggestionState::Accepted;
    }
    output.validate();
    return output;
}

} // namespace ve

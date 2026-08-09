#include "ve/commands.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ve {
namespace {

class SnapshotCommand final : public ProjectCommand {
public:
    using Mutation = std::function<void(Project&)>;

    SnapshotCommand(std::string description, Mutation mutation)
        : description_(std::move(description)), mutation_(std::move(mutation)) {}

    std::string label() const override { return description_; }

    void redo(Project& project) override {
        if (after_) {
            project = *after_;
            return;
        }
        before_ = project;
        try {
            mutation_(project);
            project.validate();
            after_ = project;
        } catch (...) {
            project = *before_;
            throw;
        }
    }

    void undo(Project& project) override {
        if (!before_) throw std::logic_error("command was not executed");
        project = *before_;
    }

private:
    std::string description_;
    Mutation mutation_;
    std::optional<Project> before_;
    std::optional<Project> after_;
};

void sortClips(Track& track) {
    std::ranges::sort(track.clips, [](const Clip& lhs, const Clip& rhs) {
        return lhs.timelineStart < rhs.timelineStart;
    });
}

void splitOne(Track& track, std::size_t index, const MediaTime& position) {
    auto& original = track.clips.at(index);
    const MediaTime offset = position - original.timelineStart;
    if (offset.units <= 0 || position >= original.timelineEnd()) {
        throw std::invalid_argument("split point must be inside clip");
    }
    Clip right = original;
    right.id = makeId();
    right.timelineStart = position.rescaled(original.timelineStart.rateNumerator,
                                             original.timelineStart.rateDenominator);
    right.sourceIn = original.sourceIn + offset;
    right.duration = original.duration - offset;
    right.audioFadeIn = {0, right.duration.rateNumerator, right.duration.rateDenominator};
    original.duration = offset.rescaled(original.duration.rateNumerator,
                                        original.duration.rateDenominator);
    original.audioFadeOut = {0, original.duration.rateNumerator, original.duration.rateDenominator};
    track.clips.insert(track.clips.begin() + static_cast<std::ptrdiff_t>(index + 1),
                       std::move(right));
}

} // namespace

UndoStack::UndoStack(std::size_t maximumDepth) : maximumDepth_(maximumDepth) {
    if (maximumDepth == 0) throw std::invalid_argument("undo depth must be positive");
}

void UndoStack::apply(Project& project, std::unique_ptr<ProjectCommand> command) {
    if (!command) throw std::invalid_argument("command is null");
    const auto revision = project.revision;
    command->redo(project);
    project.revision = revision + 1;
    redo_.clear();
    undo_.push_back(std::move(command));
    if (undo_.size() > maximumDepth_) undo_.erase(undo_.begin());
}

bool UndoStack::canUndo() const noexcept { return !undo_.empty(); }
bool UndoStack::canRedo() const noexcept { return !redo_.empty(); }
std::string UndoStack::undoLabel() const { return canUndo() ? undo_.back()->label() : std::string{}; }
std::string UndoStack::redoLabel() const { return canRedo() ? redo_.back()->label() : std::string{}; }

void UndoStack::undo(Project& project) {
    if (!canUndo()) return;
    const auto revision = project.revision;
    auto command = std::move(undo_.back());
    undo_.pop_back();
    command->undo(project);
    project.revision = revision + 1;
    redo_.push_back(std::move(command));
}

void UndoStack::redo(Project& project) {
    if (!canRedo()) return;
    const auto revision = project.revision;
    auto command = std::move(redo_.back());
    redo_.pop_back();
    command->redo(project);
    project.revision = revision + 1;
    undo_.push_back(std::move(command));
}

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

std::unique_ptr<ProjectCommand> makeAddClipCommand(Id trackId, Clip clip) {
    return std::make_unique<SnapshotCommand>("Add clip", [trackId = std::move(trackId),
                                                          clip = std::move(clip)](Project& project) {
        auto* track = project.findTrack(trackId);
        if (!track) throw std::invalid_argument("target track does not exist");
        if (track->locked) throw std::runtime_error("target track is locked");
        if (!project.findAsset(clip.assetId)) throw std::invalid_argument("clip asset does not exist");
        track->clips.push_back(clip);
        sortClips(*track);
    });
}

std::unique_ptr<ProjectCommand> makeSplitClipCommand(Id clipId, MediaTime timelinePosition) {
    return std::make_unique<SnapshotCommand>(
        "Split clip", [clipId = std::move(clipId), timelinePosition](Project& project) {
            const auto* selected = project.findClip(clipId);
            if (!selected) throw std::invalid_argument("clip does not exist");
            const auto group = selected->linkedGroupId;
            const auto selectedStart = selected->timelineStart;
            const auto selectedDuration = selected->duration;
            const MediaTime relative = timelinePosition - selectedStart;
            if (relative.units <= 0 || relative >= selectedDuration) {
                throw std::invalid_argument("split point must be inside selected clip");
            }

            for (auto& sequence : project.sequences) {
                for (auto& track : sequence.tracks) {
                    if (track.locked) continue;
                    for (std::size_t index = 0; index < track.clips.size(); ++index) {
                        const auto& candidate = track.clips[index];
                        const bool match = candidate.id == clipId ||
                                           (!group.empty() && candidate.linkedGroupId == group);
                        if (!match) continue;
                        const MediaTime candidatePoint = candidate.timelineStart + relative;
                        if (candidatePoint > candidate.timelineStart &&
                            candidatePoint < candidate.timelineEnd()) {
                            splitOne(track, index, candidatePoint);
                            ++index;
                        }
                    }
                }
            }
        });
}

std::unique_ptr<ProjectCommand> makeRippleDeleteCommand(Id sequenceId, MediaTime start,
                                                        MediaTime end) {
    return std::make_unique<SnapshotCommand>(
        "Ripple delete", [sequenceId = std::move(sequenceId), start, end](Project& project) {
            if (end <= start) throw std::invalid_argument("ripple range must be positive");
            auto* sequence = project.findSequence(sequenceId);
            if (!sequence) throw std::invalid_argument("sequence does not exist");
            const MediaTime gap = end - start;

            for (auto& track : sequence->tracks) {
                if (track.locked) continue;
                std::vector<Clip> result;
                for (const auto& input : track.clips) {
                    const auto clipStart = input.timelineStart;
                    const auto clipEnd = input.timelineEnd();
                    if (clipEnd <= start) {
                        result.push_back(input);
                    } else if (clipStart >= end) {
                        auto shifted = input;
                        shifted.timelineStart = shifted.timelineStart - gap;
                        result.push_back(std::move(shifted));
                    } else {
                        if (clipStart < start) {
                            auto left = input;
                            left.duration = start - clipStart;
                            left.audioFadeOut = MediaTime::samples(240, 48000).rescaled(
                                left.duration.rateNumerator, left.duration.rateDenominator,
                                Rounding::Up);
                            result.push_back(std::move(left));
                        }
                        if (clipEnd > end) {
                            auto right = input;
                            right.id = (clipStart < start) ? makeId() : input.id;
                            const auto removedFromClip = end - clipStart;
                            right.sourceIn = input.sourceIn + removedFromClip;
                            right.duration = clipEnd - end;
                            right.timelineStart = start.rescaled(input.timelineStart.rateNumerator,
                                                                 input.timelineStart.rateDenominator);
                            right.audioFadeIn = MediaTime::samples(240, 48000).rescaled(
                                right.duration.rateNumerator, right.duration.rateDenominator,
                                Rounding::Up);
                            result.push_back(std::move(right));
                        }
                    }
                }
                track.clips = std::move(result);
                sortClips(track);
            }
        });
}

std::unique_ptr<ProjectCommand> makeReplaceProjectCommand(std::string label, Project replacement) {
    return std::make_unique<SnapshotCommand>(
        std::move(label), [replacement = std::move(replacement)](Project& project) {
            const auto path = project.projectPath;
            project = replacement;
            if (project.projectPath.empty()) project.projectPath = path;
        });
}

} // namespace ve

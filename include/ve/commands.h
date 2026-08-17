#pragma once

#include "ve/project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class ProjectCommand {
public:
    virtual ~ProjectCommand() = default;
    [[nodiscard]] virtual std::string label() const = 0;
    virtual void redo(Project& project) = 0;
    virtual void undo(Project& project) = 0;
};

class UndoStack {
public:
    explicit UndoStack(std::size_t maximumDepth = 200);

    void apply(Project& project, std::unique_ptr<ProjectCommand> command);
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;
    void undo(Project& project);
    void redo(Project& project);
    void clear() noexcept;

private:
    std::size_t maximumDepth_;
    std::vector<std::unique_ptr<ProjectCommand>> undo_;
    std::vector<std::unique_ptr<ProjectCommand>> redo_;
};

// Adds a clip while preserving timeline ordering.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeAddClipCommand(Id trackId, Clip clip);

// Splits every member of the selected clip's linked group at the equivalent timeline point.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeSplitClipCommand(Id clipId,
                                                                   MediaTime timelinePosition);

// Moves one clip and only its contemporaneous linked counterparts. The command is confined to
// the sequence containing clipId and fails atomically if the move would overlap another clip.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeMoveClipCommand(Id clipId,
                                                                  MediaTime timelinePosition);

// Trims one edge of a clip and its contemporaneous linked counterparts. Positions are expressed
// on the timeline and must fall strictly inside the selected clip.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeTrimClipStartCommand(Id clipId,
                                                                       MediaTime timelinePosition);
[[nodiscard]] std::unique_ptr<ProjectCommand> makeTrimClipEndCommand(Id clipId,
                                                                     MediaTime timelinePosition);

// Removes [start, end) from every unlocked track in a sequence and closes the gap.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeRippleDeleteCommand(Id sequenceId,
                                                                      MediaTime start,
                                                                      MediaTime end);

// Repoints one asset without replacing its id or any timeline references. The source file is
// sampled read-only on first execution; undo/redo then use the captured project snapshots.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeRelinkAssetCommand(
    Id assetId, std::filesystem::path replacementPath,
    std::uint64_t sampleBytesPerEnd = 512U * 1024U);

// Replaces the project in one reversible transaction. Used for bulk operations.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeReplaceProjectCommand(std::string label,
                                                                        Project replacement);

} // namespace ve

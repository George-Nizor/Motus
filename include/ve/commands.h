#pragma once

#include "ve/project.h"

#include <cstddef>
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

// Removes [start, end) from every unlocked track in a sequence and closes the gap.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeRippleDeleteCommand(Id sequenceId,
                                                                      MediaTime start,
                                                                      MediaTime end);

// Replaces the project in one reversible transaction. Used for bulk operations.
[[nodiscard]] std::unique_ptr<ProjectCommand> makeReplaceProjectCommand(std::string label,
                                                                        Project replacement);

} // namespace ve


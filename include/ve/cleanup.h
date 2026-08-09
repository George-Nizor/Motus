#pragma once

#include "ve/commands.h"

#include <string_view>
#include <vector>

namespace ve {

struct PacingPreset {
    std::string_view name;
    MediaTime minimumSilence;
    MediaTime targetSilence;
};

[[nodiscard]] const PacingPreset& relaxedPacing();
[[nodiscard]] const PacingPreset& naturalBriskPacing();
[[nodiscard]] const PacingPreset& tightPacing();

struct TimeInterval {
    MediaTime start;
    MediaTime end;
};

[[nodiscard]] std::vector<TimeInterval> mergeIntervals(std::vector<TimeInterval> intervals,
                                                        MediaTime maximumGap);
[[nodiscard]] bool isBulkSafe(const CleanupSuggestion& suggestion);

// Builds a replacement project in which the source sequence remains untouched and a new
// "Cleaned" sequence receives the accepted removals. The result is intended for one
// makeReplaceProjectCommand transaction.
[[nodiscard]] Project buildCleanedProject(const Project& source, const Id& sequenceId,
                                           const std::vector<Id>& suggestionIds,
                                           bool safeOnly = true);

} // namespace ve


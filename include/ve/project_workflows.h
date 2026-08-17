#pragma once

#include "ve/project.h"

#include <filesystem>
#include <cstdint>
#include <string>

namespace ve {

struct AppendedMedia {
    Id assetId;
    Id videoClipId;
    Id audioClipId;
    Id linkedGroupId;
};

[[nodiscard]] Project makeNewProject(std::string name, const ProjectProfile& profile = {});

// Legacy/MCP append seam. Adds a reference with a real bounded integrity fingerprint, but timing
// remains provisional until applyMediaProbe is called.
[[nodiscard]] AppendedMedia appendMediaReference(Project& project,
                                                 const std::filesystem::path& mediaPath,
                                                 std::int64_t durationFrames,
                                                 std::string displayName = {});

// Adds only the lanes present in an FFprobe result and stores the complete probe record in the
// project. The source is fingerprinted read-only before the project is mutated.
[[nodiscard]] AppendedMedia appendProbedMediaReference(Project& project,
                                                       const std::filesystem::path& mediaPath,
                                                       const MediaProbeResult& probe,
                                                       std::string displayName = {},
                                                       std::string probeBackend = "ffprobe");

// Replaces an asset's durable probe metadata. Optionally expands untouched legacy full-length
// references that still exactly match the asset's former provisional duration.
void applyMediaProbe(Project& project, const Id& assetId, const MediaProbeResult& probe,
                     bool resizeLegacyFullLengthClips = false,
                     std::string probeBackend = "ffprobe");

// Returns the nearest clip edge, marker, or sequence edge inside thresholdFrames. If no edit
// boundary is close enough, inputFrame is returned unchanged.
[[nodiscard]] std::int64_t snapTimelineFrame(const Project& project, const Sequence& sequence,
                                             std::int64_t inputFrame,
                                             std::int64_t thresholdFrames);

} // namespace ve

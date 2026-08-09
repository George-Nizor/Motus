#pragma once

#include "ve/project.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace ve {

enum class GraphPurpose { Preview, Render };

struct MltGraphOptions {
    GraphPurpose purpose{GraphPurpose::Preview};
    // Keyed by asset id. Ignored for render by design.
    std::unordered_map<Id, std::filesystem::path> proxyPaths;
};

struct MltGraph {
    std::string xml;
    std::int64_t frameCount{0};
};

// Converts an immutable canonical snapshot into disposable MLT XML. Clips use inclusive MLT
// out-points, while the editor model remains half-open [start,end).
[[nodiscard]] MltGraph buildMltGraph(const Project& project, const Sequence& sequence,
                                     const MltGraphOptions& options = {});

} // namespace ve


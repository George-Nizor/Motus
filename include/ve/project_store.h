#pragma once

#include "ve/project.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ve {

class ProjectStore {
public:
    [[nodiscard]] static std::string serialize(const Project& project);
    [[nodiscard]] static Project deserialize(std::string_view document,
                                             const std::filesystem::path& projectPath = {});
    static void saveAtomically(const Project& project, const std::filesystem::path& path);
    [[nodiscard]] static Project load(const std::filesystem::path& path);

    // Writes a recovery copy and keeps the newest maxSnapshots files.
    static std::filesystem::path saveRecoverySnapshot(const Project& project,
                                                       const std::filesystem::path& directory,
                                                       std::size_t maxSnapshots = 10);
};

} // namespace ve


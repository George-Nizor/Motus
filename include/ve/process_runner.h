#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ve {

struct ProcessResult {
    int exitCode{-1};
    bool timedOut{false};
    bool stdoutTruncated{false};
    bool stderrTruncated{false};
    std::string stdoutText;
    std::string stderrText;
};

// Executes an executable directly with an argv vector: no command shell, globbing,
// interpolation, or user-provided environment expansion is involved. Output is
// always drained but retained only up to maxOutputBytes per channel.
[[nodiscard]] ProcessResult runProcess(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout = std::chrono::minutes(10),
    std::size_t maxOutputBytes = 16U * 1024U * 1024U);

} // namespace ve

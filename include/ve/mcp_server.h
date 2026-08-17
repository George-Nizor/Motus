#pragma once

#include "ve/process_runner.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ve {

struct McpServerOptions {
    // Installed builds place ffprobe/ffmpeg beside motus-mcp. Source builds fall
    // back to PATH only when no sibling tool exists.
    std::filesystem::path executableDirectory;
    // Test/embedding overrides. They are host configuration, never tool-call arguments.
    std::filesystem::path ffprobeExecutable;
    std::filesystem::path ffmpegExecutable;
    std::function<ProcessResult(const std::filesystem::path&,
                                const std::vector<std::string>&,
                                std::chrono::milliseconds)> processRunner;
};

// Dependency-free JSON-RPC/MCP request handler. Transport is newline-delimited stdio in
// motus-mcp; keeping the handler separate makes every operation deterministic and testable.
class McpServer {
public:
    explicit McpServer(McpServerOptions options = {});
    [[nodiscard]] std::string handle(std::string_view request) const;

private:
    McpServerOptions options_;
};

} // namespace ve

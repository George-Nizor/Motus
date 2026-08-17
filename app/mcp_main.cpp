#include "ve/mcp_server.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::error_code pathError;
    const auto executable = argc > 0 ? std::filesystem::absolute(argv[0], pathError)
                                     : std::filesystem::path{};
    ve::McpServerOptions options;
    if (!pathError) options.executableDirectory = executable.parent_path();
    ve::McpServer server(std::move(options));
    std::string request;
    while (std::getline(std::cin, request)) {
        try {
            const auto response = server.handle(request);
            if (!response.empty()) {
                std::cout << response;
                if (!response.ends_with('\n')) std::cout << '\n';
                std::cout.flush();
            }
        } catch (const std::exception& caught) {
            std::cerr << "motus-mcp: " << caught.what() << '\n';
        }
    }
    return 0;
}

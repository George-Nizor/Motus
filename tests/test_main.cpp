#include "test.h"

int main() {
    int failed = 0;
    for (const auto& value : test::cases()) {
        try {
            value.run();
            std::cout << "[PASS] " << value.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << value.name << "\n       " << error.what() << '\n';
        }
    }
    std::cout << test::cases().size() - static_cast<std::size_t>(failed) << "/"
              << test::cases().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}


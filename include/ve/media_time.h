#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace ve {

enum class Rounding { Down, Nearest, Up };

// Exact media time. A rate of 30000/1001 means 30,000 units span 1,001 seconds.
// Consequently, seconds = units * rateDenominator / rateNumerator.
struct MediaTime {
    std::int64_t units{0};
    std::int32_t rateNumerator{1};
    std::int32_t rateDenominator{1};

    MediaTime() = default;
    MediaTime(std::int64_t value, std::int32_t numerator, std::int32_t denominator);

    [[nodiscard]] static MediaTime frames(std::int64_t count, std::int32_t fpsNumerator,
                                          std::int32_t fpsDenominator = 1);
    [[nodiscard]] static MediaTime samples(std::int64_t count, std::int32_t sampleRate);
    [[nodiscard]] MediaTime rescaled(std::int32_t numerator, std::int32_t denominator,
                                     Rounding rounding = Rounding::Nearest) const;
    [[nodiscard]] double secondsForDisplay() const noexcept;
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const MediaTime& lhs, const MediaTime& rhs) noexcept;
    friend std::strong_ordering operator<=>(const MediaTime& lhs,
                                            const MediaTime& rhs) noexcept;
};

[[nodiscard]] MediaTime operator+(const MediaTime& lhs, const MediaTime& rhs);
[[nodiscard]] MediaTime operator-(const MediaTime& lhs, const MediaTime& rhs);

} // namespace ve

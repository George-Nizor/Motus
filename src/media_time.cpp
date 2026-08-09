#include "ve/media_time.h"

#include <limits>
#include <numeric>
#include <stdexcept>

namespace ve {
namespace {

using Wide = __int128_t;

void validateRate(std::int32_t numerator, std::int32_t denominator) {
    if (numerator <= 0 || denominator <= 0) {
        throw std::invalid_argument("media time rate must be positive");
    }
}

std::int64_t narrow(Wide value) {
    if (value > static_cast<Wide>(std::numeric_limits<std::int64_t>::max()) ||
        value < static_cast<Wide>(std::numeric_limits<std::int64_t>::min())) {
        throw std::overflow_error("media time overflow");
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t divideRounded(Wide numerator, Wide denominator, Rounding rounding) {
    if (denominator <= 0) {
        throw std::logic_error("non-positive internal denominator");
    }
    Wide quotient = numerator / denominator;
    const Wide remainder = numerator % denominator;
    if (remainder == 0) {
        return narrow(quotient);
    }
    if (rounding == Rounding::Down && numerator < 0) {
        --quotient;
    } else if (rounding == Rounding::Up && numerator > 0) {
        ++quotient;
    } else if (rounding == Rounding::Nearest) {
        const Wide magnitude = remainder < 0 ? -remainder : remainder;
        if (magnitude * 2 >= denominator) {
            quotient += numerator < 0 ? -1 : 1;
        }
    }
    return narrow(quotient);
}

} // namespace

MediaTime::MediaTime(std::int64_t value, std::int32_t numerator, std::int32_t denominator)
    : units(value), rateNumerator(numerator), rateDenominator(denominator) {
    validateRate(numerator, denominator);
    const auto divisor = std::gcd(rateNumerator, rateDenominator);
    rateNumerator /= divisor;
    rateDenominator /= divisor;
}

MediaTime MediaTime::frames(std::int64_t count, std::int32_t fpsNumerator,
                            std::int32_t fpsDenominator) {
    return {count, fpsNumerator, fpsDenominator};
}

MediaTime MediaTime::samples(std::int64_t count, std::int32_t sampleRate) {
    return {count, sampleRate, 1};
}

MediaTime MediaTime::rescaled(std::int32_t numerator, std::int32_t denominator,
                              Rounding rounding) const {
    validateRate(numerator, denominator);
    // newUnits / newRate = oldUnits / oldRate
    const Wide value = static_cast<Wide>(units) * rateDenominator * numerator;
    const Wide divisor = static_cast<Wide>(rateNumerator) * denominator;
    return {divideRounded(value, divisor, rounding), numerator, denominator};
}

double MediaTime::secondsForDisplay() const noexcept {
    return static_cast<double>(units) * static_cast<double>(rateDenominator) /
           static_cast<double>(rateNumerator);
}

std::string MediaTime::toString() const {
    return std::to_string(units) + "@" + std::to_string(rateNumerator) + "/" +
           std::to_string(rateDenominator);
}

bool operator==(const MediaTime& lhs, const MediaTime& rhs) noexcept {
    return static_cast<Wide>(lhs.units) * lhs.rateDenominator * rhs.rateNumerator ==
           static_cast<Wide>(rhs.units) * rhs.rateDenominator * lhs.rateNumerator;
}

std::strong_ordering operator<=>(const MediaTime& lhs, const MediaTime& rhs) noexcept {
    const Wide left = static_cast<Wide>(lhs.units) * lhs.rateDenominator * rhs.rateNumerator;
    const Wide right = static_cast<Wide>(rhs.units) * rhs.rateDenominator * lhs.rateNumerator;
    if (left < right) return std::strong_ordering::less;
    if (left > right) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

MediaTime operator+(const MediaTime& lhs, const MediaTime& rhs) {
    const auto converted = rhs.rescaled(lhs.rateNumerator, lhs.rateDenominator);
    return {narrow(static_cast<Wide>(lhs.units) + converted.units), lhs.rateNumerator,
            lhs.rateDenominator};
}

MediaTime operator-(const MediaTime& lhs, const MediaTime& rhs) {
    const auto converted = rhs.rescaled(lhs.rateNumerator, lhs.rateDenominator);
    return {narrow(static_cast<Wide>(lhs.units) - converted.units), lhs.rateNumerator,
            lhs.rateDenominator};
}

} // namespace ve

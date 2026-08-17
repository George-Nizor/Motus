#include "ve/cache.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ve {
namespace {

// Stable non-cryptographic digest for cache addressing. The source fingerprint itself contains
// the SHA-256 media sample; this digest only compacts the composite identity.
std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

std::string CacheIdentity::key() const {
    const auto material = source.cacheKey() + "\n" + std::to_string(streamIndex) + "\n" +
                          component + "\n" + componentVersion + "\n" + settingsJson;
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << fnv1a(material);
    return stream.str();
}

bool reconcileAsset(Project& project, const Id& assetId, const AssetFingerprint& observed,
                    bool exists) {
    auto* asset = project.findAsset(assetId);
    if (!asset) throw std::invalid_argument("asset does not exist");
    const auto oldStatus = asset->status;
    if (!exists) {
        asset->status = AssetStatus::Missing;
    } else if (asset->fingerprint == observed) {
        // Integrity refresh does not claim that an independently established codec/runtime
        // incompatibility has disappeared merely because the bytes are unchanged.
        if (asset->status != AssetStatus::Unsupported) asset->status = AssetStatus::Online;
    } else {
        asset->status = AssetStatus::Modified;
        for (auto& suggestion : project.cleanupSuggestions) {
            if (suggestion.assetId == assetId && suggestion.state != SuggestionState::Stale) {
                suggestion.state = SuggestionState::Stale;
            }
        }
    }
    return oldStatus != asset->status;
}

} // namespace ve

#pragma once

#include "ve/project.h"

#include <string>
#include <string_view>

namespace ve {

struct CacheIdentity {
    AssetFingerprint source;
    std::int32_t streamIndex{0};
    std::string component;
    std::string componentVersion;
    std::string settingsJson;

    [[nodiscard]] std::string key() const;
};

// Updates source status and marks only dependent suggestions stale. Returns true if changed.
bool reconcileAsset(Project& project, const Id& assetId, const AssetFingerprint& observed,
                    bool exists);

} // namespace ve


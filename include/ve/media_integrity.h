#pragma once

#include "ve/project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ve {

// At most this many bytes are read from each end of a source file. Keeping the bound explicit
// makes opening a project predictable even when it references very large camera originals.
inline constexpr std::uint64_t defaultMediaFingerprintSampleBytes = 512U * 1024U;
inline constexpr std::uint64_t maximumMediaFingerprintSampleBytes = 8U * 1024U * 1024U;

struct MediaFingerprintResult {
    AssetFingerprint fingerprint;
    std::uint64_t sampledByteCount{0};
};

struct MediaIntegrityIssue {
    Id assetId;
    std::filesystem::path path;
    std::string message;
};

struct MediaIntegrityReport {
    std::size_t checked{0};
    std::size_t online{0};
    std::size_t missing{0};
    std::size_t modified{0};
    std::size_t unsupported{0};
    std::size_t changed{0};
    std::size_t baselineUpgrades{0};
    std::uint64_t sampledByteCount{0};
    std::vector<MediaIntegrityIssue> issues;
};

struct RelinkResult {
    AssetFingerprint fingerprint;
    std::size_t staleAnalysisCount{0};
};

// Computes size, portable UTC mtime, and SHA-256 over non-overlapping head/tail samples. The
// source is opened read-only and never altered. sampleBytesPerEnd is strictly capped.
[[nodiscard]] MediaFingerprintResult fingerprintMediaFile(
    const std::filesystem::path& path,
    std::uint64_t sampleBytesPerEnd = defaultMediaFingerprintSampleBytes);

// Resolves the path used by rendering: a project-relative media path wins when it exists,
// followed by the stored path. If neither exists, the best expected path is returned.
[[nodiscard]] std::filesystem::path resolveAssetPath(const Project& project, const Asset& asset);

// Reconciles every asset against disk. Legacy pending fingerprints are safely established as a
// baseline; established fingerprints are never silently replaced when content changes.
[[nodiscard]] MediaIntegrityReport refreshMediaIntegrity(
    Project& project,
    std::uint64_t sampleBytesPerEnd = defaultMediaFingerprintSampleBytes);

// Repoints one existing asset while retaining its id and every referring clip. Metadata probing
// is intentionally out of scope: duration and stream flags remain unchanged.
[[nodiscard]] RelinkResult relinkAsset(
    Project& project, const Id& assetId, const std::filesystem::path& replacementPath,
    std::uint64_t sampleBytesPerEnd = defaultMediaFingerprintSampleBytes);

} // namespace ve

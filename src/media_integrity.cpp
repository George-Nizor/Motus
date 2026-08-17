#include "ve/media_integrity.h"

#include "ve/cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ve {
namespace {

constexpr std::array<std::uint32_t, 64> sha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotateRight(std::uint32_t value, std::uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
}

class Sha256 final {
public:
    void update(const unsigned char* bytes, std::size_t size) {
        if (size > (std::numeric_limits<std::uint64_t>::max() - totalBytes_)) {
            throw std::overflow_error("SHA-256 input is too large");
        }
        totalBytes_ += static_cast<std::uint64_t>(size);
        for (std::size_t index = 0; index < size; ++index) {
            block_[blockSize_++] = bytes[index];
            if (blockSize_ == block_.size()) {
                transform();
                blockSize_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const auto bitLength = totalBytes_ * 8U;
        block_[blockSize_++] = 0x80U;
        if (blockSize_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_), block_.end(), 0U);
            transform();
            blockSize_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_),
                  block_.begin() + 56, 0U);
        for (std::size_t index = 0; index < 8U; ++index) {
            const auto shift = static_cast<std::uint32_t>((7U - index) * 8U);
            block_[56U + index] = static_cast<unsigned char>((bitLength >> shift) & 0xffU);
        }
        transform();

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_) output << std::setw(8) << word;
        return output.str();
    }

private:
    void transform() noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block_[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block_[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block_[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto s0 = rotateRight(words[index - 15U], 7U) ^
                            rotateRight(words[index - 15U], 18U) ^
                            (words[index - 15U] >> 3U);
            const auto s1 = rotateRight(words[index - 2U], 17U) ^
                            rotateRight(words[index - 2U], 19U) ^
                            (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + sum1 + choose + sha256Constants[index] + words[index];
            const auto sum0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                        0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                        0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t blockSize_{0};
    std::uint64_t totalBytes_{0};
};

std::int64_t portableModifiedUtcMilliseconds(const std::filesystem::path& path) {
    std::error_code error;
    const auto fileTime = std::filesystem::last_write_time(path, error);
    if (error) {
        throw std::runtime_error("cannot read media modification time: " + error.message());
    }
    using FileClock = std::filesystem::file_time_type::clock;
    const auto systemTime = [&] {
        if constexpr (requires { FileClock::to_sys(fileTime); }) {
            return FileClock::to_sys(fileTime);
        } else {
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fileTime - FileClock::now() + std::chrono::system_clock::now());
        }
    }();
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        systemTime.time_since_epoch()).count());
}

struct FileMetadata {
    std::uint64_t byteSize;
    std::int64_t modifiedUtcMs;
};

FileMetadata readMetadata(const std::filesystem::path& path) {
    std::error_code error;
    const auto regular = std::filesystem::is_regular_file(path, error);
    if (error) throw std::runtime_error("cannot inspect media file: " + error.message());
    if (!regular) throw std::invalid_argument("media path is not a regular file");
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("cannot read media file size: " + error.message());
    return {static_cast<std::uint64_t>(size), portableModifiedUtcMilliseconds(path)};
}

void hashRange(std::ifstream& input, std::uint64_t offset, std::uint64_t byteCount,
               Sha256& digest) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("media file offset exceeds stream limits");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) throw std::runtime_error("cannot seek while fingerprinting media");

    std::array<unsigned char, 64U * 1024U> buffer{};
    auto remaining = byteCount;
    while (remaining > 0U) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(requested));
        const auto read = input.gcount();
        if (read != static_cast<std::streamsize>(requested)) {
            throw std::runtime_error("media file changed or became unreadable while fingerprinting");
        }
        digest.update(buffer.data(), requested);
        remaining -= static_cast<std::uint64_t>(requested);
    }
}

bool isLegacyPendingFingerprint(const AssetFingerprint& fingerprint) {
    return fingerprint.headTailSha256.empty() || fingerprint.headTailSha256 == "pending-probe";
}

bool isRegularFile(const std::filesystem::path& path, std::error_code& error) {
    error.clear();
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

bool isAbsentPathError(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory ||
           error == std::errc::not_a_directory;
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) throw std::runtime_error("cannot resolve media path: " + error.message());
    return absolute.lexically_normal();
}

std::optional<std::filesystem::path> projectRelativePath(
    const Project& project, const std::filesystem::path& absoluteMediaPath) {
    if (project.projectPath.empty()) return std::nullopt;
    const auto projectFile = normalizedAbsolute(project.projectPath);
    const auto relative = absoluteMediaPath.lexically_relative(projectFile.parent_path());
    if (relative.empty() || relative.is_absolute()) return std::nullopt;
    return relative.lexically_normal();
}

std::string filenameUtf8(const std::filesystem::path& path) {
    const auto encoded = path.filename().generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::size_t staleDependentAnalysis(Project& project, const Id& assetId) {
    std::size_t changed = 0;
    for (auto& suggestion : project.cleanupSuggestions) {
        if (suggestion.assetId != assetId || suggestion.state == SuggestionState::Stale) continue;
        suggestion.state = SuggestionState::Stale;
        ++changed;
    }
    return changed;
}

void countStatus(MediaIntegrityReport& report, AssetStatus status) {
    switch (status) {
    case AssetStatus::Online: ++report.online; break;
    case AssetStatus::Missing: ++report.missing; break;
    case AssetStatus::Modified: ++report.modified; break;
    case AssetStatus::Unsupported: ++report.unsupported; break;
    }
}

} // namespace

MediaFingerprintResult fingerprintMediaFile(const std::filesystem::path& path,
                                            std::uint64_t sampleBytesPerEnd) {
    if (sampleBytesPerEnd == 0U || sampleBytesPerEnd > maximumMediaFingerprintSampleBytes) {
        throw std::invalid_argument("media fingerprint sample bound is out of range");
    }
    const auto before = readMetadata(path);
    if (before.byteSize > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("media file is too large for this platform's stream API");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open media for read-only fingerprinting");
    Sha256 digest;
    const auto headBytes = std::min(before.byteSize, sampleBytesPerEnd);
    const auto tailBytes = std::min(before.byteSize - headBytes, sampleBytesPerEnd);
    hashRange(input, 0U, headBytes, digest);
    hashRange(input, before.byteSize - tailBytes, tailBytes, digest);

    const auto after = readMetadata(path);
    if (after.byteSize != before.byteSize || after.modifiedUtcMs != before.modifiedUtcMs) {
        throw std::runtime_error("media changed while its fingerprint was being calculated");
    }
    return {{before.byteSize, before.modifiedUtcMs, digest.finish()}, headBytes + tailBytes};
}

std::filesystem::path resolveAssetPath(const Project& project, const Asset& asset) {
    std::filesystem::path relativeCandidate;
    if (asset.relativePath && !project.projectPath.empty()) {
        relativeCandidate = (project.projectPath.parent_path() / *asset.relativePath).lexically_normal();
        std::error_code relativeError;
        if (isRegularFile(relativeCandidate, relativeError)) return relativeCandidate;
    }
    std::error_code storedError;
    if (isRegularFile(asset.path, storedError)) return asset.path;
    return !relativeCandidate.empty() ? relativeCandidate : asset.path;
}

MediaIntegrityReport refreshMediaIntegrity(Project& project, std::uint64_t sampleBytesPerEnd) {
    if (sampleBytesPerEnd == 0U || sampleBytesPerEnd > maximumMediaFingerprintSampleBytes) {
        throw std::invalid_argument("media fingerprint sample bound is out of range");
    }
    MediaIntegrityReport report;
    for (auto& asset : project.assets) {
        ++report.checked;
        const auto expectedPath = resolveAssetPath(project, asset);
        std::error_code fileError;
        const bool exists = isRegularFile(expectedPath, fileError);
        if (!exists) {
            const auto prior = asset.status;
            if (fileError && !isAbsentPathError(fileError)) {
                asset.status = AssetStatus::Unsupported;
                report.issues.push_back({asset.id, expectedPath,
                    "cannot inspect media file: " + fileError.message()});
            } else {
                (void)reconcileAsset(project, asset.id, {}, false);
            }
            if (asset.status != prior) ++report.changed;
            countStatus(report, asset.status);
            continue;
        }

        try {
            const auto observed = fingerprintMediaFile(expectedPath, sampleBytesPerEnd);
            if (report.sampledByteCount > std::numeric_limits<std::uint64_t>::max() -
                                              observed.sampledByteCount) {
                report.sampledByteCount = std::numeric_limits<std::uint64_t>::max();
            } else {
                report.sampledByteCount += observed.sampledByteCount;
            }
            const auto priorStatus = asset.status;
            const auto priorPath = asset.path;
            const auto priorFingerprint = asset.fingerprint;
            if (isLegacyPendingFingerprint(asset.fingerprint)) {
                asset.fingerprint = observed.fingerprint;
                asset.status = AssetStatus::Online;
                ++report.baselineUpgrades;
            } else {
                (void)reconcileAsset(project, asset.id, observed.fingerprint, true);
            }
            asset.path = expectedPath.lexically_normal();
            if (asset.status != priorStatus || asset.path != priorPath ||
                asset.fingerprint != priorFingerprint) {
                ++report.changed;
            }
        } catch (const std::exception& error) {
            const auto prior = asset.status;
            asset.status = AssetStatus::Unsupported;
            if (asset.status != prior) ++report.changed;
            report.issues.push_back({asset.id, expectedPath, error.what()});
        }
        countStatus(report, asset.status);
    }
    return report;
}

RelinkResult relinkAsset(Project& project, const Id& assetId,
                         const std::filesystem::path& replacementPath,
                         std::uint64_t sampleBytesPerEnd) {
    auto* asset = project.findAsset(assetId);
    if (!asset) throw std::invalid_argument("asset does not exist");
    const auto absolutePath = normalizedAbsolute(replacementPath);
    const auto observed = fingerprintMediaFile(absolutePath, sampleBytesPerEnd);
    const auto relativePath = projectRelativePath(project, absolutePath);
    const auto displayName = filenameUtf8(absolutePath);
    if (displayName.empty()) throw std::invalid_argument("replacement media has no filename");

    asset->path = absolutePath;
    asset->relativePath = relativePath;
    asset->displayName = displayName;
    asset->fingerprint = observed.fingerprint;
    asset->status = AssetStatus::Online;
    // A replacement may have different streams or timing. Keeping the former probe record would
    // make preview/export trust metadata for a file that is no longer referenced.
    asset->probe.reset();
    asset->probedUtcMs = 0;
    asset->probeBackend.clear();
    const auto staleCount = staleDependentAnalysis(project, assetId);
    project.validate();
    return {observed.fingerprint, staleCount};
}

} // namespace ve

#include "test.h"
#include "ve/commands.h"
#include "ve/media_integrity.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() / ("motus-integrity-" + ve::makeId())) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("could not write media integrity fixture");
}

ve::Project projectFor(const std::filesystem::path& projectPath,
                       const std::filesystem::path& mediaPath) {
    auto project = test::sampleProject();
    project.projectPath = projectPath;
    project.assets[0].path = mediaPath;
    project.assets[0].relativePath.reset();
    project.assets[0].fingerprint = ve::fingerprintMediaFile(mediaPath, 64).fingerprint;
    project.assets[0].status = ve::AssetStatus::Online;
    return project;
}

} // namespace

TEST("Media fingerprint is stable and uses standard SHA-256") {
    TemporaryDirectory directory;
    const auto mediaPath = directory.path() / "identical source.bin";
    writeBytes(mediaPath, "abc");
    const auto first = ve::fingerprintMediaFile(mediaPath, 64);
    const auto second = ve::fingerprintMediaFile(mediaPath, 64);
    CHECK(first.fingerprint == second.fingerprint);
    CHECK(first.sampledByteCount == 3);
    CHECK(first.fingerprint.headTailSha256 ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    auto project = projectFor(directory.path() / "identical.veproj", mediaPath);
    project.assets[0].status = ve::AssetStatus::Modified;
    const auto report = ve::refreshMediaIntegrity(project, 64);
    CHECK(project.assets[0].status == ve::AssetStatus::Online);
    CHECK(report.online == 1);
    CHECK(report.modified == 0);
}

TEST("Same-size sampled content changes are detected even when mtime is preserved") {
    TemporaryDirectory directory;
    const auto mediaPath = directory.path() / "same-size.mov";
    writeBytes(mediaPath, std::string(1024, 'a'));
    auto project = projectFor(directory.path() / "edit.veproj", mediaPath);
    const auto originalFingerprint = project.assets[0].fingerprint;
    const auto originalWriteTime = std::filesystem::last_write_time(mediaPath);
    project.cleanupSuggestions.push_back({"dependent", "asset", "sequence",
        ve::CleanupKind::Silence, ve::SuggestionState::Pending, {}, {}, {}, 1.0, true, {}, {}});

    std::fstream media(mediaPath, std::ios::binary | std::ios::in | std::ios::out);
    media.seekp(0);
    media.put('b'); // The bounded head sample changes; the source size does not.
    media.close();
    std::filesystem::last_write_time(mediaPath, originalWriteTime);

    const auto report = ve::refreshMediaIntegrity(project, 64);
    CHECK(project.assets[0].fingerprint == originalFingerprint);
    CHECK(project.assets[0].status == ve::AssetStatus::Modified);
    CHECK(project.cleanupSuggestions[0].state == ve::SuggestionState::Stale);
    CHECK(report.modified == 1);
    CHECK(report.sampledByteCount == 128);
}

TEST("Portable mtime changes remain visible even when sampled bytes are identical") {
    TemporaryDirectory directory;
    const auto mediaPath = directory.path() / "touched.mxf";
    writeBytes(mediaPath, std::string(512, 'x'));
    auto project = projectFor(directory.path() / "touch.veproj", mediaPath);
    const auto originalWriteTime = std::filesystem::last_write_time(mediaPath);
    std::filesystem::last_write_time(mediaPath, originalWriteTime + std::chrono::seconds(2));

    const auto report = ve::refreshMediaIntegrity(project, 64);
    CHECK(project.assets[0].status == ve::AssetStatus::Modified);
    CHECK(report.modified == 1);
}

TEST("Missing media is reconciled without reading or changing source files") {
    TemporaryDirectory directory;
    auto project = test::sampleProject();
    project.projectPath = directory.path() / "missing.veproj";
    project.assets[0].path = directory.path() / "camera original.mov";
    project.assets[0].relativePath.reset();
    project.assets[0].status = ve::AssetStatus::Online;

    const auto report = ve::refreshMediaIntegrity(project, 64);
    CHECK(project.assets[0].status == ve::AssetStatus::Missing);
    CHECK(report.checked == 1);
    CHECK(report.missing == 1);
    CHECK(report.sampledByteCount == 0);
}

TEST("Relink keeps asset and clip IDs, updates filenames, and is undoable") {
    TemporaryDirectory directory;
    const auto oldPath = directory.path() / "offline.mov";
    const auto replacement = directory.path() / "new take [final].mov";
    writeBytes(replacement, "replacement media bytes");
    auto project = test::sampleProject();
    project.projectPath = directory.path() / "project.veproj";
    project.assets[0].path = oldPath;
    project.assets[0].relativePath = "media/offline.mov";
    project.assets[0].status = ve::AssetStatus::Missing;
    project.cleanupSuggestions.push_back({"dependent", "asset", "sequence",
        ve::CleanupKind::Filler, ve::SuggestionState::Rejected, {}, {}, {}, 0.9, true, {}, {}});
    const auto videoClipId = project.sequences[0].tracks[0].clips[0].id;
    const auto audioClipId = project.sequences[0].tracks[1].clips[0].id;

    ve::UndoStack history;
    history.apply(project, ve::makeRelinkAssetCommand("asset", replacement, 64));
    CHECK(project.assets[0].id == "asset");
    CHECK(project.assets[0].status == ve::AssetStatus::Online);
    CHECK(project.assets[0].displayName == "new take [final].mov");
    CHECK(project.assets[0].relativePath.has_value());
    CHECK(project.assets[0].relativePath->generic_string() == "new take [final].mov");
    CHECK(project.sequences[0].tracks[0].clips[0].id == videoClipId);
    CHECK(project.sequences[0].tracks[1].clips[0].id == audioClipId);
    CHECK(project.sequences[0].tracks[0].clips[0].assetId == "asset");
    CHECK(project.cleanupSuggestions[0].state == ve::SuggestionState::Stale);

    history.undo(project);
    CHECK(project.assets[0].path == oldPath);
    CHECK(project.assets[0].status == ve::AssetStatus::Missing);
    CHECK(project.cleanupSuggestions[0].state == ve::SuggestionState::Rejected);
    history.redo(project);
    CHECK(project.assets[0].path.filename() == replacement.filename());
    CHECK(project.assets[0].status == ve::AssetStatus::Online);
}

TEST("Fingerprint IO obeys strict bounded head-tail sampling") {
    TemporaryDirectory directory;
    const auto mediaPath = directory.path() / "large-source.raw";
    writeBytes(mediaPath, std::string(4096, 'q'));
    const auto fingerprint = ve::fingerprintMediaFile(mediaPath, 128);
    CHECK(fingerprint.fingerprint.byteSize == 4096);
    CHECK(fingerprint.sampledByteCount == 256);
    CHECK_THROWS(ve::fingerprintMediaFile(mediaPath, 0));
    CHECK_THROWS(ve::fingerprintMediaFile(
        mediaPath, ve::maximumMediaFingerprintSampleBytes + 1));
}

TEST("Legacy pending fingerprints establish one baseline instead of reporting modification") {
    TemporaryDirectory directory;
    const auto mediaPath = directory.path() / "legacy clip.mp4";
    writeBytes(mediaPath, "legacy bytes");
    auto project = test::sampleProject();
    project.projectPath = directory.path() / "legacy.veproj";
    project.assets[0].path = mediaPath;
    project.assets[0].relativePath.reset();
    project.assets[0].fingerprint.headTailSha256 = "pending-probe";

    const auto report = ve::refreshMediaIntegrity(project, 64);
    CHECK(project.assets[0].status == ve::AssetStatus::Online);
    CHECK(project.assets[0].fingerprint.headTailSha256 != "pending-probe");
    CHECK(report.baselineUpgrades == 1);
    CHECK(report.modified == 0);
}

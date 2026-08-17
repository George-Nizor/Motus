#include "test.h"
#include "ve/project_store.h"

#include <filesystem>

TEST("Project JSON round-trips exact time and Unicode paths") {
    auto project = test::sampleProject();
    project.assets[0].path = std::filesystem::path("D:/動画/撮影 01.mp4");
    project.sequences[0].markers.push_back({"marker", ve::MediaTime::frames(42, 30), "Good take", "blue"});
    ve::MediaProbeResult probe;
    probe.duration = {10'000'000, 1'000'000, 1};
    probe.formatName = "mov,mp4";
    probe.bitRate = 8'000'000;
    probe.streams.push_back({0, ve::MediaStreamKind::Video, "h264", 1920, 1080,
        30, 1, 30, 1, 0, 0, 0, 300, "yuv420p", "bt709", "", "1/15360",
        {10'000'000, 1'000'000, 1}});
    project.assets[0].probe = probe;
    project.assets[0].probedUtcMs = 123456789;
    project.assets[0].probeBackend = "ffprobe 8.1";
    const auto document = ve::ProjectStore::serialize(project);
    const auto loaded = ve::ProjectStore::deserialize(document);
    CHECK(loaded.assets[0].path.generic_string() == project.assets[0].path.generic_string());
    CHECK(loaded.sequences[0].markers[0].time == ve::MediaTime::frames(42, 30));
    CHECK(loaded.assets[0].probe.has_value());
    CHECK(loaded.assets[0].probe->formatName == "mov,mp4");
    CHECK(loaded.assets[0].probe->streams[0].codec == "h264");
    CHECK(loaded.assets[0].probe->streams[0].timeBase == "1/15360");
    CHECK(loaded.assets[0].probe->streams[0].frameCount == 300);
    CHECK(loaded.assets[0].probedUtcMs == 123456789);
    CHECK(loaded.assets[0].probeBackend == "ffprobe 8.1");
    CHECK(document.find("\"units\"") != std::string::npos);
}

TEST("Schema version one is migrated on load") {
    auto project = test::sampleProject();
    auto document = ve::ProjectStore::serialize(project);
    const auto position = document.find("\"schemaVersion\": 3");
    CHECK(position != std::string::npos);
    document.replace(position, std::string("\"schemaVersion\": 3").size(), "\"schemaVersion\": 1");
    const auto loaded = ve::ProjectStore::deserialize(document);
    CHECK(loaded.schemaVersion == ve::Project::currentSchemaVersion);
}

TEST("Atomic save leaves a loadable project and rotates a backup") {
    auto project = test::sampleProject();
    const auto directory = std::filesystem::temp_directory_path() / ("ve-tests-" + ve::makeId());
    const auto path = directory / "sample.veproj";
    ve::ProjectStore::saveAtomically(project, path);
    project.name = "Changed";
    ve::ProjectStore::saveAtomically(project, path);
    CHECK(ve::ProjectStore::load(path).name == "Changed");
    CHECK(std::filesystem::exists(path.string() + ".bak"));
    std::filesystem::remove_all(directory);
}

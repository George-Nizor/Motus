#include "test.h"
#include "ve/project_store.h"

#include <filesystem>

TEST("Project JSON round-trips exact time and Unicode paths") {
    auto project = test::sampleProject();
    project.assets[0].path = std::filesystem::path("D:/動画/撮影 01.mp4");
    project.sequences[0].markers.push_back({"marker", ve::MediaTime::frames(42, 30), "Good take", "blue"});
    const auto document = ve::ProjectStore::serialize(project);
    const auto loaded = ve::ProjectStore::deserialize(document);
    CHECK(loaded.assets[0].path.generic_string() == project.assets[0].path.generic_string());
    CHECK(loaded.sequences[0].markers[0].time == ve::MediaTime::frames(42, 30));
    CHECK(document.find("\"units\"") != std::string::npos);
}

TEST("Schema version one is migrated on load") {
    auto project = test::sampleProject();
    auto document = ve::ProjectStore::serialize(project);
    const auto position = document.find("\"schemaVersion\": 2");
    CHECK(position != std::string::npos);
    document.replace(position, std::string("\"schemaVersion\": 2").size(), "\"schemaVersion\": 1");
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


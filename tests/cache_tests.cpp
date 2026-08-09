#include "test.h"
#include "ve/cache.h"

TEST("Cache identity includes settings and model version") {
    ve::CacheIdentity first{{10, 20, "hash"}, 0, "transcript", "small-v1", "{\"vad\":1}"};
    auto second = first;
    second.componentVersion = "small-v2";
    CHECK(first.key() != second.key());
    second = first;
    second.settingsJson = "{\"vad\":2}";
    CHECK(first.key() != second.key());
}

TEST("Modified source invalidates only dependent cleanup suggestions") {
    auto project = test::sampleProject();
    project.cleanupSuggestions.push_back({"s1", "asset", "sequence", ve::CleanupKind::Silence,
        ve::SuggestionState::Pending, {}, {}, {}, 1.0, true, {}, {}});
    project.cleanupSuggestions.push_back({"s2", "other", "sequence", ve::CleanupKind::Silence,
        ve::SuggestionState::Pending, {}, {}, {}, 1.0, true, {}, {}});
    CHECK(ve::reconcileAsset(project, "asset", {999, 20, "different"}, true));
    CHECK(project.assets[0].status == ve::AssetStatus::Modified);
    CHECK(project.cleanupSuggestions[0].state == ve::SuggestionState::Stale);
    CHECK(project.cleanupSuggestions[1].state == ve::SuggestionState::Pending);
}


#include "test.h"
#include "ve/cleanup.h"

TEST("Intervals merge with a configurable tolerance") {
    std::vector<ve::TimeInterval> intervals{
        {ve::MediaTime::samples(0, 1000), ve::MediaTime::samples(100, 1000)},
        {ve::MediaTime::samples(110, 1000), ve::MediaTime::samples(200, 1000)},
        {ve::MediaTime::samples(300, 1000), ve::MediaTime::samples(400, 1000)}};
    const auto merged = ve::mergeIntervals(intervals, ve::MediaTime::samples(20, 1000));
    CHECK(merged.size() == 2);
    CHECK(merged[0].end.units == 200);
}

TEST("Bulk cleanup duplicates the sequence and leaves the original untouched") {
    auto project = test::sampleProject();
    project.cleanupSuggestions.push_back({"suggestion", "asset", "sequence", ve::CleanupKind::Silence,
        ve::SuggestionState::Pending, ve::MediaTime::samples(96000, 48000),
        ve::MediaTime::samples(144000, 48000), ve::MediaTime::samples(11520, 48000),
        0.95, true, "before [silence] after", "cache"});
    auto cleaned = ve::buildCleanedProject(project, "sequence", {"suggestion"});
    CHECK(cleaned.sequences.size() == 2);
    CHECK(cleaned.sequences[0].tracks[0].clips.size() == 1);
    CHECK(cleaned.sequences[0].tracks[0].clips[0].duration.units == 300);
    CHECK(cleaned.sequences[1].name == "Rough Cut — Cleaned");
    CHECK(cleaned.sequences[1].tracks[0].clips.size() == 2);
    CHECK(cleaned.cleanupSuggestions[0].state == ve::SuggestionState::Accepted);
}

TEST("Contextual and unsafe suggestions are excluded from Accept safe") {
    ve::CleanupSuggestion suggestion;
    suggestion.state = ve::SuggestionState::Pending;
    suggestion.kind = ve::CleanupKind::ContextualPhrase;
    suggestion.speechFreeHandles = true;
    suggestion.confidence = 0.99;
    suggestion.sourceStart = ve::MediaTime::samples(0, 48000);
    suggestion.sourceEnd = ve::MediaTime::samples(48000, 48000);
    suggestion.replacementDuration = ve::MediaTime::samples(1000, 48000);
    CHECK(!ve::isBulkSafe(suggestion));
    suggestion.kind = ve::CleanupKind::Filler;
    suggestion.speechFreeHandles = false;
    CHECK(!ve::isBulkSafe(suggestion));
}

TEST("A manual-only suggestion can be applied by an explicit user action") {
    auto project = test::sampleProject();
    project.cleanupSuggestions.push_back({"manual", "asset", "sequence", ve::CleanupKind::Filler,
        ve::SuggestionState::ManualOnly, ve::MediaTime::samples(48000, 48000),
        ve::MediaTime::samples(72000, 48000), ve::MediaTime::samples(2400, 48000),
        0.50, false, "explicit review", "cache"});
    const auto cleaned = ve::buildCleanedProject(project, "sequence", {"manual"}, false);
    CHECK(cleaned.sequences.size() == 2);
    CHECK(cleaned.cleanupSuggestions[0].state == ve::SuggestionState::Accepted);
}

TEST("A cleaned sequence owns independent clip link effect and transition identifiers") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(120, 30)));
    auto& source = project.sequences[0];
    source.tracks[0].clips[0].effects.push_back(
        {"effect-original", "affine", true, {{"opacity", "1"}}, {}});
    source.transitions.push_back({"transition-original", source.tracks[0].clips[0].id,
                                  source.tracks[0].clips[1].id, "mix",
                                  ve::MediaTime::frames(12, 30)});
    project.validate();

    const auto cleaned = ve::buildCleanedProject(project, "sequence", {});
    const auto& copy = cleaned.sequences[1];
    CHECK(copy.tracks[0].clips[0].id != source.tracks[0].clips[0].id);
    CHECK(copy.tracks[0].clips[0].linkedGroupId != source.tracks[0].clips[0].linkedGroupId);
    CHECK(copy.tracks[0].clips[0].linkedGroupId == copy.tracks[1].clips[0].linkedGroupId);
    CHECK(copy.tracks[0].clips[0].effects[0].id != "effect-original");
    CHECK(copy.transitions[0].id != "transition-original");
    CHECK(copy.transitions[0].fromClipId == copy.tracks[0].clips[0].id);
    CHECK(copy.transitions[0].toClipId == copy.tracks[0].clips[1].id);
}

#include "test.h"
#include "ve/commands.h"

TEST("Split is linked and undoable as one command") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(90, 30)));
    CHECK(project.findTrack("v1")->clips.size() == 2);
    CHECK(project.findTrack("a1")->clips.size() == 2);
    CHECK(project.findTrack("v1")->clips[0].duration.units == 90);
    CHECK(project.findTrack("v1")->clips[1].sourceIn.units == 90);
    CHECK(history.undoLabel() == "Split clip");
    history.undo(project);
    CHECK(project.findTrack("v1")->clips.size() == 1);
    CHECK(history.canRedo());
    history.redo(project);
    CHECK(project.findTrack("a1")->clips.size() == 2);
}

TEST("Ripple delete removes a middle range without losing sync") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    history.apply(project, ve::makeRippleDeleteCommand("sequence", ve::MediaTime::frames(90, 30),
                                                       ve::MediaTime::frames(150, 30)));
    for (const auto& track : project.findSequence("sequence")->tracks) {
        CHECK(track.clips.size() == 2);
        CHECK(track.clips[0].duration.units == 90);
        CHECK(track.clips[1].timelineStart.units == 90);
        CHECK(track.clips[1].sourceIn.units == 150);
        CHECK(track.clips[1].duration.units == 150);
    }
    history.undo(project);
    CHECK(project.findTrack("v1")->clips.front().duration.units == 300);
}

TEST("Invalid commands roll back atomically") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    CHECK_THROWS(history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(400, 30))));
    CHECK(project.findTrack("v1")->clips.size() == 1);
    CHECK(!history.canUndo());
}


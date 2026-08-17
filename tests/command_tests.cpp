#include "test.h"
#include "ve/commands.h"
#include "ve/project_workflows.h"

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

TEST("Linked split stays inside its sequence and creates a new right-side link") {
    auto project = test::sampleProject();
    auto duplicate = project.sequences[0];
    duplicate.id = "duplicate-sequence";
    duplicate.tracks[0].id = "duplicate-v1";
    duplicate.tracks[1].id = "duplicate-a1";
    duplicate.tracks[0].clips[0].id = "duplicate-video";
    duplicate.tracks[1].clips[0].id = "duplicate-audio";
    project.sequences.push_back(std::move(duplicate));
    project.validate();

    ve::UndoStack history;
    history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(90, 30)));
    const auto& original = project.sequences[0];
    const auto& untouched = project.sequences[1];
    CHECK(original.tracks[0].clips.size() == 2);
    CHECK(original.tracks[1].clips.size() == 2);
    CHECK(untouched.tracks[0].clips.size() == 1);
    CHECK(untouched.tracks[1].clips.size() == 1);
    CHECK(original.tracks[0].clips[0].linkedGroupId == "linked");
    CHECK(original.tracks[0].clips[1].linkedGroupId != "linked");
    CHECK(original.tracks[0].clips[1].linkedGroupId ==
          original.tracks[1].clips[1].linkedGroupId);
}

TEST("Move and trim operate on one contemporaneous linked pair") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    history.apply(project, ve::makeTrimClipStartCommand("video", ve::MediaTime::frames(30, 30)));
    CHECK(project.findClip("video")->timelineStart.units == 30);
    CHECK(project.findClip("audio")->timelineStart.units == 30);
    CHECK(project.findClip("video")->sourceIn.units == 30);
    CHECK(project.findClip("audio")->duration.units == 270);

    history.apply(project, ve::makeTrimClipEndCommand("video", ve::MediaTime::frames(240, 30)));
    CHECK(project.findClip("video")->duration.units == 210);
    CHECK(project.findClip("audio")->duration.units == 210);

    history.apply(project, ve::makeMoveClipCommand("video", ve::MediaTime::frames(60, 30)));
    CHECK(project.findClip("video")->timelineStart.units == 60);
    CHECK(project.findClip("audio")->timelineStart.units == 60);
    history.undo(project);
    CHECK(project.findClip("video")->timelineStart.units == 30);
}

TEST("Split and ripple renew copied effect identifiers") {
    auto project = test::sampleProject();
    project.findClip("video")->effects.push_back({"video-effect", "affine", true, {}, {}});
    project.findClip("audio")->effects.push_back({"audio-effect", "volume", true, {}, {}});
    project.validate();

    ve::UndoStack history;
    history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(150, 30)));
    const auto* videoTrack = project.findTrack("v1");
    const auto* audioTrack = project.findTrack("a1");
    CHECK(videoTrack->clips[0].effects[0].id == "video-effect");
    CHECK(videoTrack->clips[1].effects[0].id != "video-effect");
    CHECK(audioTrack->clips[1].effects[0].id != "audio-effect");

    history.undo(project);
    history.apply(project, ve::makeRippleDeleteCommand("sequence", ve::MediaTime::frames(90, 30),
                                                       ve::MediaTime::frames(120, 30)));
    videoTrack = project.findTrack("v1");
    audioTrack = project.findTrack("a1");
    CHECK(videoTrack->clips.size() == 2);
    CHECK(videoTrack->clips[0].effects[0].id == "video-effect");
    CHECK(videoTrack->clips[1].effects[0].id != "video-effect");
    CHECK(videoTrack->clips[1].linkedGroupId != videoTrack->clips[0].linkedGroupId);
    CHECK(videoTrack->clips[1].linkedGroupId == audioTrack->clips[1].linkedGroupId);
}

TEST("Ripple delete prunes transitions whose clips were removed") {
    auto project = test::sampleProject();
    ve::UndoStack history;
    history.apply(project, ve::makeSplitClipCommand("video", ve::MediaTime::frames(100, 30)));
    auto* track = project.findTrack("v1");
    project.findSequence("sequence")->transitions.push_back(
        {"transition", track->clips[0].id, track->clips[1].id, "mix",
         ve::MediaTime::frames(10, 30)});
    project.validate();

    history.apply(project, ve::makeRippleDeleteCommand("sequence", ve::MediaTime::frames(0, 30),
                                                       ve::MediaTime::frames(100, 30)));
    CHECK(project.findSequence("sequence")->transitions.empty());
}

TEST("Ripple middle split remaps outgoing transitions to the surviving right fragment") {
    auto project = test::sampleProject();
    auto* videoTrack = project.findTrack("v1");
    ve::Clip tail = videoTrack->clips[0];
    tail.id = "tail";
    tail.linkedGroupId.clear();
    tail.timelineStart = ve::MediaTime::frames(300, 30);
    tail.sourceIn = ve::MediaTime::frames(0, 30);
    tail.duration = ve::MediaTime::frames(60, 30);
    videoTrack->clips.push_back(tail);
    project.findSequence("sequence")->transitions.push_back(
        {"outgoing", "video", "tail", "mix", ve::MediaTime::frames(10, 30)});
    project.validate();

    ve::UndoStack history;
    history.apply(project, ve::makeRippleDeleteCommand("sequence", ve::MediaTime::frames(90, 30),
                                                       ve::MediaTime::frames(120, 30)));
    const auto& editedTrack = project.findTrack("v1")->clips;
    const auto& transition = project.findSequence("sequence")->transitions[0];
    CHECK(transition.fromClipId == editedTrack[1].id);
    CHECK(transition.fromClipId != "video");
    CHECK(transition.toClipId == "tail");
}

TEST("Timeline snapping chooses the nearest edit boundary inside its threshold") {
    auto project = test::sampleProject();
    auto& sequence = project.sequences[0];
    sequence.markers.push_back({"snap-marker", ve::MediaTime::frames(80, 30), "Beat", "cyan"});
    CHECK(ve::snapTimelineFrame(project, sequence, 76, 5) == 80);
    CHECK(ve::snapTimelineFrame(project, sequence, 76, 3) == 76);
    CHECK(ve::snapTimelineFrame(project, sequence, 297, 5) == 300);
    CHECK_THROWS(ve::snapTimelineFrame(project, sequence, 12, -1));
}

TEST("Ripple delete collapses markers inside the range and shifts later markers") {
    auto project = test::sampleProject();
    auto& markers = project.findSequence("sequence")->markers;
    markers.push_back({"before", ve::MediaTime::frames(40, 30), "Before", "cyan"});
    markers.push_back({"inside", ve::MediaTime::frames(100, 30), "Inside", "cyan"});
    markers.push_back({"after", ve::MediaTime::frames(200, 30), "After", "cyan"});
    project.validate();

    ve::UndoStack history;
    history.apply(project, ve::makeRippleDeleteCommand("sequence", ve::MediaTime::frames(90, 30),
                                                       ve::MediaTime::frames(150, 30)));
    const auto& edited = project.findSequence("sequence")->markers;
    CHECK(edited[0].time.units == 40);
    CHECK(edited[1].time.units == 90);
    CHECK(edited[2].time.units == 140);
}

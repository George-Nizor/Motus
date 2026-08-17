#include "test.h"
#include "ve/native_media.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

struct TemporaryMedia {
    std::filesystem::path directory{std::filesystem::temp_directory_path() /
                                    ("motus-native-media-" + ve::makeId())};
    std::filesystem::path path{directory / "source.mp4"};

    TemporaryMedia() {
        std::filesystem::create_directories(directory);
        std::ofstream(path, std::ios::binary).put('\0');
    }
    ~TemporaryMedia() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

ve::MediaProbeResult sampleProbe() {
    ve::MediaProbeResult probe;
    probe.duration = {10'000'000, 1'000'000, 1};
    probe.formatName = "mov,mp4,m4a,3gp,3g2,mj2";
    probe.bitRate = 4'000'000;
    probe.streams.push_back({0, ve::MediaStreamKind::Video, "h264", 1920, 1080,
                             30, 1, 30, 1, 0, 0, 0, 300, "yuv420p", "bt709", "", "1/15360",
                             {10'000'000, 1'000'000, 1}});
    probe.streams.push_back({1, ve::MediaStreamKind::Audio, "aac", 0, 0,
                             0, 1, 0, 1, 48'000, 2, 0, 0, "", "", "stereo", "1/48000",
                             {10'000'000, 1'000'000, 1}});
    return probe;
}

ve::Project renderableProject(const std::filesystem::path& path) {
    auto project = test::sampleProject();
    project.profile.width = 1920;
    project.profile.height = 1080;
    project.assets[0].path = path;
    project.assets[0].relativePath.reset();
    project.assets[0].probe = sampleProbe();
    project.assets[0].probedUtcMs = 1234;
    project.assets[0].probeBackend = "ffprobe test";
    project.validate();
    return project;
}

std::string genericUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

} // namespace

TEST("simple native-media plan preserves original source ranges") {
    TemporaryMedia media;
    const auto project = renderableProject(media.path);
    const auto plan = ve::buildSimpleTimelinePlan(project, project.sequences[0]);
    CHECK(plan.segments.size() == 1);
    CHECK(plan.segments[0].sourcePath == media.path);
    CHECK(plan.segments[0].sourceInUs == 0);
    CHECK(plan.segments[0].durationUs == 10'000'000);
    CHECK(plan.durationUs == 10'000'000);
    CHECK(plan.hasAudio);

    const auto arguments = ve::buildFfmpegExportArguments(plan, media.directory / "out.partial.mp4");
    CHECK(std::ranges::find(arguments, "-filter_complex") != arguments.end());
    CHECK(std::ranges::find(arguments, "libx264") != arguments.end());
    CHECK(std::ranges::find(arguments, genericUtf8(media.path)) != arguments.end());
    CHECK(arguments.back().ends_with("out.partial.mp4"));
}

TEST("simple native-media plan rejects edits it cannot render truthfully") {
    TemporaryMedia media;
    auto project = renderableProject(media.path);
    project.sequences[0].tracks[0].clips[0].effects.push_back(
        {"fx", "affine", true, {}, {}});
    CHECK_THROWS(ve::buildSimpleTimelinePlan(project, project.sequences[0]));

    project = renderableProject(media.path);
    project.sequences[0].tracks[1].clips[0].sourceIn = ve::MediaTime::frames(1, 30);
    CHECK_THROWS(ve::buildSimpleTimelinePlan(project, project.sequences[0]));

    project = renderableProject(media.path);
    project.assets[0].probe.reset();
    CHECK_THROWS(ve::buildSimpleTimelinePlan(project, project.sequences[0]));

    project = renderableProject(media.path);
    project.assets[0].status = ve::AssetStatus::Missing;
    CHECK_THROWS(ve::buildSimpleTimelinePlan(project, project.sequences[0]));
}

TEST("FFmpeg progress and atomic staging paths are deterministic") {
    CHECK(ve::parseFfmpegProgressMicroseconds("out_time_us=1250000\r\n") == 1'250'000);
    CHECK(ve::parseFfmpegProgressMicroseconds("out_time_ms=2500000") == 2'500'000);
    CHECK(!ve::parseFfmpegProgressMicroseconds("progress=continue"));
    CHECK(!ve::parseFfmpegProgressMicroseconds("out_time_us=-1"));
    CHECK(ve::stagedExportPath("C:/Exports/cut.mp4").generic_string() ==
          "C:/Exports/cut.motus-partial.mp4");
}

TEST("simple native-media plan keeps NTSC adjacent cuts gapless") {
    TemporaryMedia media;
    auto project = renderableProject(media.path);
    project.profile.frameRateNumerator = 30'000;
    project.profile.frameRateDenominator = 1'001;
    project.assets[0].duration = ve::MediaTime::frames(300, 30'000, 1'001);
    project.assets[0].probe->duration = project.assets[0].duration.rescaled(1'000'000, 1);
    for (auto& track : project.sequences[0].tracks) {
        auto first = track.clips[0];
        first.timelineStart = ve::MediaTime::frames(0, 30'000, 1'001);
        first.sourceIn = ve::MediaTime::frames(0, 30'000, 1'001);
        first.duration = ve::MediaTime::frames(1, 30'000, 1'001);
        auto second = first;
        second.id += "-2";
        second.linkedGroupId += "-2";
        second.timelineStart = ve::MediaTime::frames(1, 30'000, 1'001);
        second.sourceIn = ve::MediaTime::frames(1, 30'000, 1'001);
        track.clips = {first, second};
    }
    project.validate();
    const auto plan = ve::buildSimpleTimelinePlan(project, project.sequences[0]);
    CHECK(plan.segments.size() == 2);
    CHECK(plan.segments[1].timelineStartUs == 33'367);
    CHECK(plan.durationUs == 66'733);
}

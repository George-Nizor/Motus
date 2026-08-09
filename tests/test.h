#pragma once

#include "ve/project.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct Case { std::string name; std::function<void()> run; };
inline std::vector<Case>& cases() { static std::vector<Case> values; return values; }
struct Registration {
    Registration(std::string name, std::function<void()> run) { cases().push_back({std::move(name), std::move(run)}); }
};

inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                             " check failed: " + expression);
}

inline ve::Project sampleProject() {
    ve::Project project;
    project.id = "project";
    project.name = "Sample";
    project.profile = {3840, 2160, 30, 1, 48000, 2, "Rec.709 SDR"};
    project.assets.push_back({"asset", "talking-head.mp4", "media/talking-head.mp4", "Talking Head",
        {1000000, 123456789, "abc123"}, ve::AssetStatus::Online, ve::MediaTime::frames(300, 30),
        true, true, true});
    ve::Clip video{"video", "asset", "linked", ve::MediaTime::frames(0, 30),
        ve::MediaTime::frames(0, 30), ve::MediaTime::frames(300, 30),
        ve::MediaTime::frames(0, 30), ve::MediaTime::frames(0, 30), 1.0, {}};
    ve::Clip audio = video; audio.id = "audio";
    ve::Sequence sequence{"sequence", "Rough Cut",
        {{"v1", "Video 1", ve::TrackKind::Video, false, false, true, {video}},
         {"a1", "Audio 1", ve::TrackKind::Audio, false, false, true, {audio}}}, {}, {}};
    project.sequences.push_back(std::move(sequence));
    project.activeSequenceId = "sequence";
    project.validate();
    return project;
}

} // namespace test

#define VE_JOIN_IMPL(a, b) a##b
#define VE_JOIN(a, b) VE_JOIN_IMPL(a, b)
#define TEST(name) static void VE_JOIN(test_, __LINE__)(); \
    static test::Registration VE_JOIN(reg_, __LINE__)(name, VE_JOIN(test_, __LINE__)); \
    static void VE_JOIN(test_, __LINE__)()
#define CHECK(expression) test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_THROWS(expression) do { bool caught_ = false; try { (void)(expression); } \
    catch (...) { caught_ = true; } CHECK(caught_); } while (false)

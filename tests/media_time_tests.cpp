#include "test.h"
#include "ve/media_time.h"

TEST("MediaTime rescales NTSC frames exactly") {
    const auto frameTime = ve::MediaTime::frames(30, 30000, 1001);
    const auto samples = frameTime.rescaled(48000, 1);
    CHECK(samples.units == 48048);
    CHECK(samples == frameTime);
}

TEST("MediaTime compares mixed rates without floating point") {
    CHECK(ve::MediaTime::samples(48000, 48000) == ve::MediaTime::frames(30, 30));
    CHECK(ve::MediaTime::frames(1, 24) > ve::MediaTime::frames(1, 30));
    CHECK((ve::MediaTime::frames(60, 30) - ve::MediaTime::samples(48000, 48000)).units == 30);
}

TEST("MediaTime supports explicit negative rounding") {
    const ve::MediaTime value{-1, 3, 1};
    CHECK(value.rescaled(2, 1, ve::Rounding::Down).units == -1);
    CHECK(value.rescaled(2, 1, ve::Rounding::Up).units == 0);
    CHECK_THROWS(ve::MediaTime(1, 0, 1));
}


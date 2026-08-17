#include "test.h"
#include "ve/media_probe.h"

TEST("FFprobe parser preserves rational CFR and exact duration") {
    const auto result = ve::parseFfprobeJson(R"({
      "streams": [
        {"index":0,"codec_name":"h264","codec_type":"video","width":1920,"height":1080,
         "pix_fmt":"yuv420p","color_space":"bt709","time_base":"1/30000",
         "duration":"10.010000","nb_frames":"300","r_frame_rate":"30000/1001",
         "avg_frame_rate":"30000/1001","tags":{"rotate":"90"}},
        {"index":1,"codec_name":"aac","codec_type":"audio","sample_rate":"48000","channels":2,
         "channel_layout":"stereo","time_base":"1/48000","duration":"10.005000",
         "r_frame_rate":"0/0","avg_frame_rate":"0/0"}
      ],
      "format":{"duration":"10.010000","format_name":"mov,mp4","bit_rate":"8123456"}
    })");
    CHECK(result.streams.size() == 2);
    CHECK(result.duration.units == 10010000);
    CHECK(result.streams[0].averageRateNumerator == 30000);
    CHECK(result.streams[0].averageRateDenominator == 1001);
    CHECK(result.streams[0].rotationDegrees == 90);
    CHECK(result.streams[1].sampleRate == 48000);
    CHECK(result.streams[1].channelLayout == "stereo");
    CHECK(result.streams[1].timeBase == "1/48000");
    CHECK(result.streams[0].duration.units == 10010000);
    CHECK(result.streams[0].frameCount == 300);
    CHECK(result.formatName == "mov,mp4");
    CHECK(result.bitRate == 8123456);
    CHECK(!result.variableFrameRate);
    CHECK(!result.proxyRecommended);
}

TEST("FFprobe parser detects HEVC 4K VFR proxy candidates") {
    const auto result = ve::parseFfprobeJson(R"({
      "streams": [
        {"index":0,"codec_name":"hevc","codec_type":"video","width":3840,"height":2160,
         "pix_fmt":"yuv420p10le","r_frame_rate":"30/1","avg_frame_rate":"2991/100",
         "side_data_list":[{"rotation":-90}]}
      ],
      "format":{"duration":"3600.1234567"}
    })");
    CHECK(result.variableFrameRate);
    CHECK(result.proxyRecommended);
    CHECK(result.duration.units == 3600123456);
    CHECK(result.streams[0].rotationDegrees == -90);
}

TEST("FFprobe parser rejects unusable documents") {
    CHECK_THROWS(ve::parseFfprobeJson(R"({"format":{"duration":"1.0"}})"));
    CHECK_THROWS(ve::parseFfprobeJson(R"({"streams":[]})"));
}

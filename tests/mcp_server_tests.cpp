#include "test.h"
#include "ve/mcp_server.h"
#include "ve/project_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string escaped(std::string value) {
    std::string output;
    for (const char character : value) {
        if (character == '\\' || character == '"') output.push_back('\\');
        output.push_back(character);
    }
    return output;
}

std::string call(ve::McpServer& server, int id, std::string name, std::string arguments) {
    return server.handle("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
        "\",\"arguments\":" + arguments + "}}");
}

std::string object(std::initializer_list<std::pair<std::string, std::string>> values) {
    std::string output = "{";
    bool first = true;
    for (const auto& [name, value] : values) {
        if (!first) output += ',';
        first = false;
        output += "\"" + name + "\":" + value;
    }
    return output + "}";
}

std::string stringValue(const std::filesystem::path& value) {
    return "\"" + escaped(value.generic_string()) + "\"";
}

std::string stringValue(const std::string& value) {
    return "\"" + escaped(value) + "\"";
}

std::string stringValue(const char* value) {
    return stringValue(std::string(value));
}

std::string probeDocument() {
    return R"({"format":{"duration":"3.000000","format_name":"mov,mp4","bit_rate":"640000"},"streams":[{"index":0,"codec_type":"video","codec_name":"h264","width":320,"height":180,"avg_frame_rate":"30/1","r_frame_rate":"30/1","time_base":"1/15360","duration":"3.000000","nb_frames":"90","pix_fmt":"yuv420p","color_space":"bt709"},{"index":1,"codec_type":"audio","codec_name":"aac","sample_rate":"48000","channels":2,"channel_layout":"stereo","time_base":"1/48000","duration":"3.000000"}]})";
}

} // namespace

TEST("MCP negotiates only supported protocols and advertises schema-rich tools") {
    ve::McpServer server;
    const auto supported = server.handle(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    CHECK(supported.find("2025-11-25") != std::string::npos);
    CHECK(supported.find("no tool modifies source media") != std::string::npos);
    const auto unsupported = server.handle(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2099-test"}})");
    CHECK(unsupported.find("2099-test") == std::string::npos);
    CHECK(unsupported.find("2025-11-25") != std::string::npos);
    const auto malformedVersion = server.handle(
        R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":42}})");
    CHECK(malformedVersion.find("-32602") != std::string::npos);

    const auto tools = server.handle(R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{}})");
    for (const auto* name : {"motus_create_project", "motus_import_media", "motus_relink_media",
             "motus_move_clip", "motus_trim_clip_start", "motus_trim_clip_end",
             "motus_ripple_delete", "motus_add_marker", "motus_remove_marker",
             "motus_set_track_state", "motus_generate_mlt_graph", "motus_export_simple"}) {
        CHECK(tools.find(name) != std::string::npos);
    }
    CHECK(tools.find("outputSchema") != std::string::npos);
    CHECK(tools.find("structuredContent") == std::string::npos);
    CHECK(tools.find("readOnlyHint") != std::string::npos);
    CHECK(tools.find("expectedRevision") != std::string::npos);
    CHECK(tools.find("motus_append_media_reference") == std::string::npos);
}

TEST("MCP creates probes edits marks tracks inspects graphs and rejects stale writes") {
    const auto directory = std::filesystem::temp_directory_path() / ("motus-mcp-" + ve::makeId());
    std::filesystem::create_directories(directory);
    const auto projectPath = directory / "sample.veproj";
    const auto mediaPath = directory / "clip.mp4";
    const auto graphPath = directory / "graph.mlt";
    std::ofstream(mediaPath, std::ios::binary) << "synthetic-media";

    ve::McpServer server({{}, "fake-ffprobe", "fake-ffmpeg",
        [](const std::filesystem::path& executable, const std::vector<std::string>& arguments,
           std::chrono::milliseconds) {
            ve::ProcessResult result;
            if (executable.string().find("ffprobe") != std::string::npos) {
                CHECK(arguments.size() == 7);
                result.exitCode = 0;
                result.stdoutText = probeDocument();
                return result;
            }
            result.exitCode = 1;
            result.stderrText = "unexpected renderer";
            return result;
        }});

    auto result = call(server, 1, "motus_create_project", object({
        {"projectPath", stringValue(projectPath)}, {"name", stringValue("MCP Sample")}}));
    CHECK(result.find("\"ok\":true") != std::string::npos);
    CHECK(std::filesystem::exists(projectPath));
    result = call(server, 2, "motus_create_project", object({
        {"projectPath", stringValue(projectPath)}, {"name", stringValue("Overwrite")}}));
    CHECK(result.find("already_exists") != std::string::npos);

    result = call(server, 3, "motus_import_media", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "0"},
        {"mediaPath", stringValue(mediaPath)}}));
    CHECK(result.find("videoClipId") != std::string::npos);
    CHECK(result.find("channelLayout") != std::string::npos);
    auto project = ve::ProjectStore::load(projectPath);
    CHECK(project.revision == 1);
    CHECK(project.assets[0].probe.has_value());
    CHECK(project.assets[0].probe->streams[1].channelLayout == "stereo");
    const auto clipId = project.sequences[0].tracks[0].clips[0].id;
    const auto sequenceId = project.sequences[0].id;
    const auto trackId = project.sequences[0].tracks[0].id;

    result = call(server, 4, "motus_split_clip", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "0"},
        {"clipId", stringValue(clipId)}, {"timelineFrame", "30"}}));
    CHECK(result.find("revision_conflict") != std::string::npos);
    CHECK(ve::ProjectStore::load(projectPath).revision == 1);

    result = call(server, 5, "motus_split_clip", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "1"},
        {"clipId", stringValue(clipId)}, {"timelineFrame", "30"}}));
    CHECK(result.find("\"revision\":2") != std::string::npos);
    project = ve::ProjectStore::load(projectPath);
    const auto rightClipId = project.sequences[0].tracks[0].clips[1].id;

    result = call(server, 6, "motus_move_clip", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "2"},
        {"clipId", stringValue(rightClipId)}, {"timelineFrame", "45"}}));
    CHECK(result.find("\"revision\":3") != std::string::npos);
    result = call(server, 7, "motus_trim_clip_start", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "3"},
        {"clipId", stringValue(rightClipId)}, {"timelineFrame", "50"}}));
    CHECK(result.find("\"revision\":4") != std::string::npos);
    result = call(server, 8, "motus_trim_clip_end", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "4"},
        {"clipId", stringValue(rightClipId)}, {"timelineFrame", "80"}}));
    CHECK(result.find("\"revision\":5") != std::string::npos);

    result = call(server, 9, "motus_add_marker", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "5"},
        {"sequenceId", stringValue(sequenceId)}, {"timelineFrame", "20"},
        {"label", stringValue("Beat")}}));
    CHECK(result.find("markerId") != std::string::npos);
    project = ve::ProjectStore::load(projectPath);
    const auto markerId = project.sequences[0].markers[0].id;
    result = call(server, 10, "motus_remove_marker", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "6"},
        {"sequenceId", stringValue(sequenceId)}, {"markerId", stringValue(markerId)}}));
    CHECK(result.find("\"revision\":7") != std::string::npos);
    result = call(server, 11, "motus_set_track_state", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "7"},
        {"trackId", stringValue(trackId)}, {"locked", "true"}}));
    CHECK(result.find("\"locked\":true") != std::string::npos);

    result = call(server, 12, "motus_inspect_project", object({
        {"projectPath", stringValue(projectPath)}}));
    CHECK(result.find("MCP Sample") != std::string::npos);
    CHECK(result.find("sourceInFrame") != std::string::npos);

    result = call(server, 13, "motus_generate_mlt_graph", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(graphPath)}}));
    CHECK(result.find("frameCount") != std::string::npos);
    CHECK(std::filesystem::exists(graphPath));
    result = call(server, 14, "motus_generate_mlt_graph", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(graphPath)}}));
    CHECK(result.find("already_exists") != std::string::npos);
    result = call(server, 15, "motus_generate_mlt_graph", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(mediaPath)},
        {"overwrite", "true"}}));
    CHECK(result.find("invalid_path") != std::string::npos ||
          result.find("source_collision") != std::string::npos);

    std::filesystem::remove_all(directory);
}

TEST("MCP export uses private staging and protects source outputs") {
    const auto directory = std::filesystem::temp_directory_path() / ("motus-mcp-export-" + ve::makeId());
    std::filesystem::create_directories(directory);
    const auto projectPath = directory / "sample.veproj";
    const auto mediaPath = directory / "clip.mp4";
    const auto outputPath = directory / "render.mp4";
    std::ofstream(mediaPath, std::ios::binary) << "source";

    std::filesystem::path observedStaging;
    ve::McpServer server({{}, "fake-ffprobe", "fake-ffmpeg",
        [&](const std::filesystem::path& executable, const std::vector<std::string>& arguments,
            std::chrono::milliseconds) {
            ve::ProcessResult result;
            result.exitCode = 0;
            if (executable.string().find("ffprobe") != std::string::npos) {
                result.stdoutText = probeDocument();
            } else {
                observedStaging = arguments.back();
                CHECK(observedStaging != outputPath);
                CHECK(observedStaging.extension() == ".mp4");
                std::ofstream(observedStaging, std::ios::binary) << "rendered";
            }
            return result;
        }});
    (void)call(server, 1, "motus_create_project", object({
        {"projectPath", stringValue(projectPath)}, {"name", stringValue("Export")},
        {"width", "320"}, {"height", "180"}}));
    (void)call(server, 2, "motus_import_media", object({
        {"projectPath", stringValue(projectPath)}, {"expectedRevision", "0"},
        {"mediaPath", stringValue(mediaPath)}}));
    auto result = call(server, 3, "motus_export_simple", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(mediaPath)},
        {"overwrite", "true"}}));
    CHECK(result.find("source_collision") != std::string::npos);
    result = call(server, 4, "motus_export_simple", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(outputPath)}}));
    CHECK(result.find("\"ok\":true") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(outputPath));
    CHECK(!std::filesystem::exists(observedStaging));
    result = call(server, 5, "motus_export_simple", object({
        {"projectPath", stringValue(projectPath)}, {"outputPath", stringValue(outputPath)}}));
    CHECK(result.find("already_exists") != std::string::npos);
    std::filesystem::remove_all(directory);
}

TEST("MCP reports malformed JSON RPC schemas and structured tool errors") {
    ve::McpServer server;
    CHECK(server.handle("not json").find("-32700") != std::string::npos);
    CHECK(server.handle(R"({"jsonrpc":"1.0","id":1,"method":"ping"})").find("-32600") != std::string::npos);
    CHECK(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})").empty());
    const auto unknown = call(server, 2, "motus_unknown", "{}");
    CHECK(unknown.find("isError") != std::string::npos);
    CHECK(unknown.find("invalid_arguments") != std::string::npos);
    const auto missing = call(server, 3, "motus_inspect_project",
        R"({"projectPath":"/definitely/missing.veproj"})");
    CHECK(missing.find("isError") != std::string::npos);
    CHECK(missing.find("not_found") != std::string::npos);
}

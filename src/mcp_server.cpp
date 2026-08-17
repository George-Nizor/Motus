#include "ve/mcp_server.h"

#include "json.h"
#include "ve/commands.h"
#include "ve/media_integrity.h"
#include "ve/media_probe.h"
#include "ve/mlt_graph.h"
#include "ve/native_media.h"
#include "ve/project_store.h"
#include "ve/project_workflows.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ve {
namespace {

using Object = json::Value::Object;
using Array = json::Value::Array;
using Clock = std::chrono::milliseconds;

constexpr std::string_view currentProtocol = "2025-11-25";
constexpr std::array<std::string_view, 3> supportedProtocols{
    currentProtocol, "2025-06-18", "2025-03-26"};

class ToolFailure final : public std::runtime_error {
public:
    ToolFailure(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}
    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

const json::Value& nullId() {
    static const json::Value value(nullptr);
    return value;
}

json::Value rpcResponse(const json::Value& id, json::Value result) {
    return Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json::Value rpcError(const json::Value& id, std::int32_t code, std::string message) {
    return Object{{"jsonrpc", "2.0"}, {"id", id},
                  {"error", Object{{"code", code}, {"message", std::move(message)}}}};
}

json::Value textResult(const json::Value& id, json::Value structured, bool isError = false) {
    const auto text = json::dump(structured, 2);
    Object result{{"content", Array{Object{{"type", "text"}, {"text", text}}}},
                  {"structuredContent", std::move(structured)}};
    if (isError) result.emplace("isError", true);
    return rpcResponse(id, std::move(result));
}

Object stringProperty(std::string description) {
    return Object{{"type", "string"}, {"description", std::move(description)}};
}

Object integerProperty(std::string description, std::int64_t minimum = 0,
                       std::optional<std::int64_t> maximum = std::nullopt) {
    Object value{{"type", "integer"}, {"minimum", minimum},
                 {"description", std::move(description)}};
    if (maximum) value.emplace("maximum", *maximum);
    return value;
}

Object booleanProperty(std::string description) {
    return Object{{"type", "boolean"}, {"description", std::move(description)}};
}

Object revisionProperty() {
    return integerProperty("Revision returned by the latest inspect/mutation; stale edits are rejected", 0);
}

Object typed(std::string type) {
    return Object{{"type", std::move(type)}};
}

Object successSchema(Object properties, Array required) {
    properties.emplace("ok", Object{{"type", "boolean"}, {"const", true}});
    required.emplace_back("ok");
    return Object{{"type", "object"}, {"properties", std::move(properties)},
                  {"required", std::move(required)}, {"additionalProperties", false}};
}

Object errorSchema() {
    return Object{{"type", "object"},
        {"properties", Object{
            {"ok", Object{{"type", "boolean"}, {"const", false}}},
            {"error", Object{{"type", "object"},
                {"properties", Object{{"code", typed("string")}, {"message", typed("string")},
                                      {"operation", typed("string")}}},
                {"required", Array{"code", "message", "operation"}},
                {"additionalProperties", false}}}}},
        {"required", Array{"ok", "error"}}, {"additionalProperties", false}};
}

Object outputSchema(std::string_view name) {
    Object properties;
    Array required;
    const auto add = [&](std::string key, std::string type, bool isRequired = true) {
        if (isRequired) required.emplace_back(key);
        properties.emplace(std::move(key), typed(std::move(type)));
    };
    if (name == "motus_create_project" || name == "motus_inspect_project") {
        add("projectPath", "string"); add("project", "object");
    } else if (name == "motus_refresh_media_integrity") {
        add("revision", "integer"); add("report", "object");
    } else if (name == "motus_import_media") {
        add("assetId", "string"); add("videoClipId", "string"); add("audioClipId", "string");
        add("linkedGroupId", "string"); add("probe", "object"); add("revision", "integer");
    } else if (name == "motus_relink_media") {
        add("assetId", "string"); add("revision", "integer"); add("probe", "object");
    } else if (name == "motus_split_clip" || name == "motus_move_clip" ||
               name == "motus_trim_clip_start" || name == "motus_trim_clip_end" ||
               name == "motus_ripple_delete") {
        add("revision", "integer");
    } else if (name == "motus_add_marker" || name == "motus_remove_marker") {
        add("markerId", "string"); add("revision", "integer");
    } else if (name == "motus_set_track_state") {
        add("trackId", "string"); add("locked", "boolean"); add("muted", "boolean");
        add("visible", "boolean"); add("revision", "integer");
    } else if (name == "motus_generate_mlt_graph") {
        add("outputPath", "string"); add("frameCount", "integer");
    } else if (name == "motus_export_simple") {
        add("outputPath", "string"); add("byteSize", "integer"); add("durationUs", "integer");
        add("segmentCount", "integer"); add("renderer", "string");
    }
    return Object{{"oneOf", Array{successSchema(std::move(properties), std::move(required)),
                                   errorSchema()}}};
}

Object annotations(std::string title, bool readOnly, bool destructive,
                   bool idempotent, bool openWorld) {
    return Object{{"title", std::move(title)}, {"readOnlyHint", readOnly},
                  {"destructiveHint", destructive}, {"idempotentHint", idempotent},
                  {"openWorldHint", openWorld}};
}

json::Value tool(std::string name, std::string description, Object properties,
                 Array required, Object hints) {
    const auto schema = outputSchema(name);
    return Object{{"name", std::move(name)}, {"description", std::move(description)},
                  {"inputSchema", Object{{"type", "object"},
                                         {"properties", std::move(properties)},
                                         {"required", std::move(required)},
                                         {"additionalProperties", false}}},
                  {"outputSchema", schema}, {"annotations", std::move(hints)}};
}

Array toolCatalog() {
    Array values;
    values.push_back(tool("motus_create_project",
        "Create a versioned Motus .veproj. Existing files are preserved unless overwrite is true.",
        {{"projectPath", stringProperty("Destination .veproj path")},
         {"name", stringProperty("Project name")},
         {"width", integerProperty("Even frame width", 2)},
         {"height", integerProperty("Even frame height", 2)},
         {"fpsNumerator", integerProperty("Frame-rate numerator", 1)},
         {"fpsDenominator", integerProperty("Frame-rate denominator", 1)},
         {"overwrite", booleanProperty("Explicitly replace an existing project, retaining its backup")}},
        {"projectPath", "name"}, annotations("Create Motus project", false, true, false, false)));
    values.push_back(tool("motus_inspect_project",
        "Validate and return the durable project profile, media probe metadata, tracks, clips, and markers.",
        {{"projectPath", stringProperty("Existing .veproj path")}}, {"projectPath"},
        annotations("Inspect Motus project", true, false, true, false)));
    values.push_back(tool("motus_refresh_media_integrity",
        "Re-fingerprint referenced originals read-only and persist missing/modified/online states.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()}}, {"projectPath", "expectedRevision"},
        annotations("Refresh media integrity", false, false, true, true)));
    values.push_back(tool("motus_import_media",
        "Run FFprobe, fingerprint an existing local media file read-only, persist full probe metadata, and append only its real video/audio lanes.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"mediaPath", stringProperty("Existing audio/video media path")},
         {"displayName", stringProperty("Optional display name")}},
        {"projectPath", "expectedRevision", "mediaPath"}, annotations("Import probed media", false, false, false, true)));
    values.push_back(tool("motus_relink_media",
        "Replace a missing/modified asset reference, re-probe it, and preserve every asset and clip identifier.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"assetId", stringProperty("Asset identifier")},
         {"mediaPath", stringProperty("Existing replacement media path")}},
        {"projectPath", "expectedRevision", "assetId", "mediaPath"},
        annotations("Relink and re-probe media", false, true, false, true)));
    values.push_back(tool("motus_split_clip",
        "Split a clip and its contemporaneous linked counterpart at an exact project frame.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"clipId", stringProperty("Clip identifier")},
         {"timelineFrame", integerProperty("Frame strictly inside the clip", 0)}},
        {"projectPath", "expectedRevision", "clipId", "timelineFrame"},
        annotations("Split linked clip", false, true, false, false)));
    values.push_back(tool("motus_move_clip",
        "Move a clip and its contemporaneous linked counterpart to an exact project frame.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"clipId", stringProperty("Clip identifier")},
         {"timelineFrame", integerProperty("New clip start frame", 0)}},
        {"projectPath", "expectedRevision", "clipId", "timelineFrame"},
        annotations("Move linked clip", false, true, false, false)));
    values.push_back(tool("motus_trim_clip_start",
        "Trim the linked clip pair's start at an exact timeline frame.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"clipId", stringProperty("Clip identifier")},
         {"timelineFrame", integerProperty("New start frame inside the clip", 0)}},
        {"projectPath", "expectedRevision", "clipId", "timelineFrame"},
        annotations("Trim linked clip start", false, true, false, false)));
    values.push_back(tool("motus_trim_clip_end",
        "Trim the linked clip pair's end at an exact timeline frame.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"clipId", stringProperty("Clip identifier")},
         {"timelineFrame", integerProperty("New exclusive end frame inside the clip", 1)}},
        {"projectPath", "expectedRevision", "clipId", "timelineFrame"},
        annotations("Trim linked clip end", false, true, false, false)));
    values.push_back(tool("motus_ripple_delete",
        "Remove a half-open frame range from every unlocked track and close the gap; overlapping media and markers are affected.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"sequenceId", stringProperty("Sequence identifier")},
         {"startFrame", integerProperty("Inclusive first frame", 0)},
         {"endFrame", integerProperty("Exclusive end frame", 1)}},
        {"projectPath", "expectedRevision", "sequenceId", "startFrame", "endFrame"},
        annotations("Ripple-delete timeline range", false, true, false, false)));
    values.push_back(tool("motus_add_marker",
        "Add a named timeline marker to a sequence at an exact project frame.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"sequenceId", stringProperty("Optional sequence; defaults to active sequence")},
         {"timelineFrame", integerProperty("Marker frame", 0)},
         {"label", stringProperty("Optional marker label")},
         {"color", stringProperty("Optional QML color token; defaults to cyan")}},
        {"projectPath", "expectedRevision", "timelineFrame"},
        annotations("Add timeline marker", false, false, false, false)));
    values.push_back(tool("motus_remove_marker",
        "Remove one durable timeline marker by identifier.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"sequenceId", stringProperty("Optional sequence; defaults to active sequence")},
         {"markerId", stringProperty("Marker identifier")}},
        {"projectPath", "expectedRevision", "markerId"},
        annotations("Remove timeline marker", false, true, true, false)));
    values.push_back(tool("motus_set_track_state",
        "Set one or more durable track flags. Omitted flags are left unchanged.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"expectedRevision", revisionProperty()},
         {"trackId", stringProperty("Track identifier")},
         {"locked", booleanProperty("Prevent edit commands on this track")},
         {"muted", booleanProperty("Exclude audio lane from preview/export")},
         {"visible", booleanProperty("Include video lane in preview/export")}},
        {"projectPath", "expectedRevision", "trackId"},
        annotations("Set Motus track state", false, true, true, false)));
    values.push_back(tool("motus_generate_mlt_graph",
        "Write diagnostic original-media MLT XML. This is not Motus's supported rendered export.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"sequenceId", stringProperty("Optional sequence; defaults to active sequence")},
         {"outputPath", stringProperty("Destination .mlt path")},
         {"overwrite", booleanProperty("Explicitly replace an existing non-source output")}},
        {"projectPath", "outputPath"},
        annotations("Generate diagnostic MLT graph", false, true, true, true)));
    values.push_back(tool("motus_export_simple",
        "Synchronously render the supported gapless one-video-lane linked rough-cut subset to H.264/AAC MP4 from originals, using an atomic private staging file.",
        {{"projectPath", stringProperty("Existing .veproj path")},
         {"sequenceId", stringProperty("Optional sequence; defaults to active sequence")},
         {"outputPath", stringProperty("Destination .mp4 path")},
         {"overwrite", booleanProperty("Explicitly replace an existing non-source output")},
         {"timeoutSeconds", integerProperty("Hard render timeout", 1, 7200)}},
        {"projectPath", "outputPath"},
        annotations("Export supported rough cut", false, true, false, true)));
    return values;
}

void validateArguments(const Object& arguments,
                       std::initializer_list<std::string_view> allowed,
                       std::initializer_list<std::string_view> required) {
    std::unordered_set<std::string_view> accepted(allowed.begin(), allowed.end());
    for (const auto& [name, value] : arguments) {
        (void)value;
        if (!accepted.contains(name)) {
            throw ToolFailure("invalid_arguments", "unknown argument: " + name);
        }
    }
    for (const auto name : required) {
        if (!arguments.contains(std::string(name))) {
            throw ToolFailure("invalid_arguments", "missing argument: " + std::string(name));
        }
    }
}

const json::Value& argument(const Object& arguments, const std::string& name) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end()) {
        throw ToolFailure("invalid_arguments", "missing argument: " + name);
    }
    return iterator->second;
}

std::string requiredString(const Object& arguments, const std::string& name) {
    try {
        const auto value = argument(arguments, name).string();
        if (value.empty()) throw ToolFailure("invalid_arguments", name + " must not be empty");
        return value;
    } catch (const ToolFailure&) {
        throw;
    } catch (...) {
        throw ToolFailure("invalid_arguments", name + " must be a string");
    }
}

std::string optionalString(const Object& arguments, const std::string& name,
                           std::string fallback = {}) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end()) return fallback;
    try {
        return iterator->second.string();
    } catch (...) {
        throw ToolFailure("invalid_arguments", name + " must be a string");
    }
}

bool optionalBool(const Object& arguments, const std::string& name, bool fallback = false) {
    const auto iterator = arguments.find(name);
    if (iterator == arguments.end()) return fallback;
    try {
        return iterator->second.boolean();
    } catch (...) {
        throw ToolFailure("invalid_arguments", name + " must be a boolean");
    }
}

std::int64_t requiredInteger(const Object& arguments, const std::string& name,
                             std::int64_t minimum = std::numeric_limits<std::int64_t>::min(),
                             std::int64_t maximum = std::numeric_limits<std::int64_t>::max()) {
    std::int64_t value = 0;
    try {
        value = argument(arguments, name).integer();
    } catch (...) {
        throw ToolFailure("invalid_arguments", name + " must be an integer");
    }
    if (value < minimum || value > maximum) {
        throw ToolFailure("invalid_arguments", name + " is outside its supported range");
    }
    return value;
}

std::int64_t optionalInteger(const Object& arguments, const std::string& name,
                             std::int64_t fallback, std::int64_t minimum,
                             std::int64_t maximum) {
    return arguments.contains(name) ? requiredInteger(arguments, name, minimum, maximum) : fallback;
}

std::int32_t optionalInt32(const Object& arguments, const std::string& name,
                           std::int32_t fallback, std::int32_t minimum) {
    const auto value = optionalInteger(arguments, name, fallback, minimum,
                                       std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(value);
}

std::string pathText(const std::filesystem::path& value) {
    const auto encoded = value.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path absolutePath(const std::filesystem::path& value) {
    if (value.empty()) throw ToolFailure("invalid_path", "path must not be empty");
    std::error_code error;
    const auto absolute = std::filesystem::absolute(value, error).lexically_normal();
    if (error) throw ToolFailure("invalid_path", "cannot resolve path: " + error.message());
    return absolute;
}

std::string foldedPath(const std::filesystem::path& value) {
    auto text = pathText(absolutePath(value));
#ifdef _WIN32
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return text;
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (foldedPath(left) == foldedPath(right)) return true;
    std::error_code error;
    if (!std::filesystem::exists(left, error) || error) return false;
    if (!std::filesystem::exists(right, error) || error) return false;
    return std::filesystem::equivalent(left, right, error) && !error;
}

bool extensionIs(const std::filesystem::path& path, std::string expected) {
    auto extension = path.extension().string();
#ifdef _WIN32
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(expected, expected.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return extension == expected;
}

void requireRegularFile(const std::filesystem::path& path, std::string_view label) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        throw ToolFailure("not_found", std::string(label) + " does not exist or is not a file: " +
                                      pathText(path));
    }
}

Project loadProject(const std::filesystem::path& path) {
    if (!extensionIs(path, ".veproj")) {
        throw ToolFailure("invalid_path", "projectPath must end in .veproj");
    }
    requireRegularFile(path, "project");
    return ProjectStore::load(path);
}

void protectOutput(const std::filesystem::path& output, bool overwrite,
                   const std::vector<std::filesystem::path>& protectedPaths) {
    for (const auto& protectedPath : protectedPaths) {
        if (samePath(output, protectedPath)) {
            throw ToolFailure("source_collision", "output must not overwrite a project or source media file");
        }
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(output, error);
    if (error) throw ToolFailure("invalid_path", "cannot inspect output path: " + error.message());
    if (exists && !overwrite) {
        throw ToolFailure("already_exists", "output already exists; set overwrite=true to replace it");
    }
    if (exists && !std::filesystem::is_regular_file(output, error)) {
        throw ToolFailure("invalid_path", "output exists but is not a regular file");
    }
}

void writeAtomically(const std::filesystem::path& output, std::string_view contents,
                     bool overwrite, const std::vector<std::filesystem::path>& protectedPaths) {
    const auto absolute = absolutePath(output);
    protectOutput(absolute, overwrite, protectedPaths);
    const auto parent = absolute.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    auto temporary = parent / (absolute.filename().string() + ".motus-" + makeId() + ".tmp");
    try {
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw ToolFailure("io_error", "cannot open private staging file");
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.flush();
            if (!stream) throw ToolFailure("io_error", "could not finish private staging file");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), absolute.c_str(),
                         (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U) | MOVEFILE_WRITE_THROUGH)) {
            throw ToolFailure("io_error", "Windows could not publish output (error " +
                                          std::to_string(GetLastError()) + ")");
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary, absolute, error);
        if (error) throw ToolFailure("io_error", "could not publish output: " + error.message());
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void promoteExport(const std::filesystem::path& staged,
                   const std::filesystem::path& output, bool overwrite) {
#ifdef _WIN32
    if (!MoveFileExW(staged.c_str(), output.c_str(),
                     (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U) | MOVEFILE_WRITE_THROUGH)) {
        throw ToolFailure("io_error", "Windows could not publish export (error " +
                                      std::to_string(GetLastError()) + ")");
    }
#else
    (void)overwrite;
    std::error_code error;
    std::filesystem::rename(staged, output, error);
    if (error) throw ToolFailure("io_error", "could not publish export: " + error.message());
#endif
}

std::int64_t frame(const Project& project, const MediaTime& value) {
    return value.rescaled(project.profile.frameRateNumerator,
                          project.profile.frameRateDenominator).units;
}

std::string assetStatus(AssetStatus value) {
    switch (value) {
    case AssetStatus::Online: return "online";
    case AssetStatus::Missing: return "missing";
    case AssetStatus::Modified: return "modified";
    case AssetStatus::Unsupported: return "unsupported";
    }
    return "unsupported";
}

std::string streamKind(MediaStreamKind value) {
    switch (value) {
    case MediaStreamKind::Video: return "video";
    case MediaStreamKind::Audio: return "audio";
    case MediaStreamKind::Other: return "other";
    }
    return "other";
}

Object probeSummary(const MediaProbeResult& probe) {
    Array streams;
    for (const auto& stream : probe.streams) {
        streams.push_back(Object{{"index", stream.index}, {"kind", streamKind(stream.kind)},
            {"codec", stream.codec}, {"width", stream.width}, {"height", stream.height},
            {"averageRateNumerator", stream.averageRateNumerator},
            {"averageRateDenominator", stream.averageRateDenominator},
            {"nominalRateNumerator", stream.nominalRateNumerator},
            {"nominalRateDenominator", stream.nominalRateDenominator},
            {"sampleRate", stream.sampleRate}, {"channels", stream.channels},
            {"channelLayout", stream.channelLayout}, {"timeBase", stream.timeBase},
            {"frameCount", stream.frameCount}, {"pixelFormat", stream.pixelFormat},
            {"colorSpace", stream.colorSpace}, {"rotationDegrees", stream.rotationDegrees},
            {"durationUs", stream.duration.rescaled(1'000'000, 1).units}});
    }
    Array warnings;
    for (const auto& warning : probe.warnings) warnings.emplace_back(warning);
    return Object{{"durationUs", probe.duration.rescaled(1'000'000, 1).units},
                  {"formatName", probe.formatName}, {"bitRate", probe.bitRate},
                  {"variableFrameRate", probe.variableFrameRate},
                  {"proxyRecommended", probe.proxyRecommended},
                  {"streams", std::move(streams)}, {"warnings", std::move(warnings)}};
}

Object projectSummary(const Project& project) {
    Array assets;
    for (const auto& asset : project.assets) {
        Object value{{"id", asset.id}, {"name", asset.displayName},
                     {"path", pathText(asset.path)}, {"status", assetStatus(asset.status)},
                     {"durationFrames", frame(project, asset.duration)},
                     {"hasVideo", asset.hasVideo}, {"hasAudio", asset.hasAudio},
                     {"proxyEligible", asset.proxyEligible}, {"provisional", !asset.probe},
                     {"probeBackend", asset.probeBackend}, {"probedUtcMs", asset.probedUtcMs}};
        value.emplace("probe", asset.probe ? json::Value(probeSummary(*asset.probe))
                                            : json::Value(nullptr));
        assets.emplace_back(std::move(value));
    }
    Array sequences;
    for (const auto& sequence : project.sequences) {
        Array tracks;
        for (const auto& track : sequence.tracks) {
            Array clips;
            for (const auto& clip : track.clips) {
                clips.push_back(Object{{"id", clip.id}, {"assetId", clip.assetId},
                    {"startFrame", frame(project, clip.timelineStart)},
                    {"sourceInFrame", frame(project, clip.sourceIn)},
                    {"durationFrames", frame(project, clip.duration)},
                    {"endFrame", frame(project, clip.timelineEnd())},
                    {"linkedGroupId", clip.linkedGroupId},
                    {"speed", clip.speed},
                    {"effectCount", static_cast<std::int64_t>(clip.effects.size())}});
            }
            tracks.push_back(Object{{"id", track.id}, {"name", track.name},
                {"kind", track.kind == TrackKind::Video ? "video" : "audio"},
                {"locked", track.locked}, {"muted", track.muted}, {"visible", track.visible},
                {"clips", std::move(clips)}});
        }
        Array markers;
        for (const auto& marker : sequence.markers) {
            markers.push_back(Object{{"id", marker.id},
                                     {"timelineFrame", frame(project, marker.time)},
                                     {"label", marker.label}, {"color", marker.color}});
        }
        Array transitions;
        for (const auto& transition : sequence.transitions) {
            transitions.push_back(Object{{"id", transition.id},
                {"fromClipId", transition.fromClipId}, {"toClipId", transition.toClipId},
                {"service", transition.service},
                {"durationFrames", frame(project, transition.duration)}});
        }
        sequences.push_back(Object{{"id", sequence.id}, {"name", sequence.name},
                                   {"tracks", std::move(tracks)},
                                   {"markers", std::move(markers)},
                                   {"transitions", std::move(transitions)}});
    }
    return Object{{"schemaVersion", project.schemaVersion}, {"id", project.id},
        {"name", project.name}, {"revision", project.revision},
        {"activeSequenceId", project.activeSequenceId ? json::Value(*project.activeSequenceId)
                                                       : json::Value(nullptr)},
        {"profile", Object{{"width", project.profile.width}, {"height", project.profile.height},
            {"fpsNumerator", project.profile.frameRateNumerator},
            {"fpsDenominator", project.profile.frameRateDenominator},
            {"audioSampleRate", project.profile.audioSampleRate},
            {"audioChannels", project.profile.audioChannels},
            {"colorSpace", project.profile.colorSpace}}},
        {"assets", std::move(assets)}, {"sequences", std::move(sequences)},
        {"cleanupSuggestionCount", static_cast<std::int64_t>(project.cleanupSuggestions.size())}};
}

Sequence& requestedSequence(Project& project, const Object& arguments) {
    const auto requested = optionalString(arguments, "sequenceId",
                                          project.activeSequenceId.value_or(""));
    auto* sequence = project.findSequence(requested);
    if (!sequence) throw ToolFailure("not_found", "sequence does not exist");
    return *sequence;
}

MediaTime projectFrame(const Project& project, std::int64_t value) {
    return MediaTime::frames(value, project.profile.frameRateNumerator,
                             project.profile.frameRateDenominator);
}

std::filesystem::path configuredTool(const McpServerOptions& options, bool probe) {
    const auto configured = probe ? options.ffprobeExecutable : options.ffmpegExecutable;
    if (!configured.empty()) return configured;
#ifdef _WIN32
    const auto filename = probe ? std::filesystem::path("ffprobe.exe")
                                : std::filesystem::path("ffmpeg.exe");
#else
    const auto filename = probe ? std::filesystem::path("ffprobe")
                                : std::filesystem::path("ffmpeg");
#endif
    if (!options.executableDirectory.empty()) {
        const auto sibling = options.executableDirectory / filename;
        std::error_code error;
        if (std::filesystem::is_regular_file(sibling, error)) return sibling;
    }
    return filename;
}

ProcessResult execute(const McpServerOptions& options, const std::filesystem::path& executable,
                      const std::vector<std::string>& arguments, Clock timeout) {
    if (options.processRunner) return options.processRunner(executable, arguments, timeout);
    return runProcess(executable, arguments, timeout);
}

std::string diagnostic(const ProcessResult& result) {
    auto value = result.stderrText.empty() ? result.stdoutText : result.stderrText;
    if (value.size() > 4000U) value = value.substr(value.size() - 4000U);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value.empty() ? "no diagnostic output" : value;
}

MediaProbeResult probeMedia(const McpServerOptions& options,
                            const std::filesystem::path& mediaPath) {
    const auto executable = configuredTool(options, true);
    const auto result = execute(options, executable,
        {"-v", "error", "-show_streams", "-show_format", "-of", "json", pathText(mediaPath)},
        std::chrono::seconds(30));
    if (result.timedOut) throw ToolFailure("probe_timeout", "FFprobe timed out after 30 seconds");
    if (result.stdoutTruncated) {
        throw ToolFailure("probe_failed", "FFprobe JSON exceeded the bounded output limit");
    }
    if (result.exitCode != 0) {
        throw ToolFailure("probe_failed", "FFprobe exited " + std::to_string(result.exitCode) +
                                          ": " + diagnostic(result));
    }
    try {
        return parseFfprobeJson(result.stdoutText);
    } catch (const std::exception& error) {
        throw ToolFailure("probe_failed", "FFprobe returned unusable metadata: " +
                                          std::string(error.what()));
    }
}

std::string toolBackend(const std::filesystem::path& executable) {
    const auto filename = executable.filename();
    return filename.empty() ? pathText(executable) : pathText(filename);
}

void applyCommand(Project& project, const std::filesystem::path& projectPath,
                  std::unique_ptr<ProjectCommand> command) {
    UndoStack history;
    history.apply(project, std::move(command));
    ProjectStore::saveAtomically(project, projectPath);
}

std::vector<std::filesystem::path> protectedProjectPaths(const Project& project,
                                                         const std::filesystem::path& projectPath) {
    std::vector<std::filesystem::path> values{projectPath};
    values.reserve(project.assets.size() + 1U);
    for (const auto& asset : project.assets) values.push_back(resolveAssetPath(project, asset));
    return values;
}

Object performTool(const McpServerOptions& options, const std::string& name,
                   const Object& arguments) {
    if (name == "motus_create_project") {
        validateArguments(arguments,
            {"projectPath", "name", "width", "height", "fpsNumerator", "fpsDenominator", "overwrite"},
            {"projectPath", "name"});
        const auto projectPath = absolutePath(requiredString(arguments, "projectPath"));
        if (!extensionIs(projectPath, ".veproj")) {
            throw ToolFailure("invalid_path", "projectPath must end in .veproj");
        }
        const bool overwrite = optionalBool(arguments, "overwrite");
        std::error_code error;
        if (std::filesystem::exists(projectPath, error) && !overwrite) {
            throw ToolFailure("already_exists", "project already exists; set overwrite=true to replace it");
        }
        ProjectProfile profile;
        profile.width = optionalInt32(arguments, "width", 3840, 2);
        profile.height = optionalInt32(arguments, "height", 2160, 2);
        profile.frameRateNumerator = optionalInt32(arguments, "fpsNumerator", 30, 1);
        profile.frameRateDenominator = optionalInt32(arguments, "fpsDenominator", 1, 1);
        if (profile.width % 2 != 0 || profile.height % 2 != 0) {
            throw ToolFailure("invalid_arguments", "width and height must be even for native export");
        }
        auto project = makeNewProject(requiredString(arguments, "name"), profile);
        project.projectPath = projectPath;
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"projectPath", pathText(projectPath)},
                      {"project", projectSummary(project)}};
    }

    const auto projectPath = absolutePath(requiredString(arguments, "projectPath"));
    auto project = loadProject(projectPath);
    const std::array<std::string_view, 12> mutations{
        "motus_refresh_media_integrity", "motus_import_media", "motus_relink_media",
        "motus_append_media_reference", "motus_split_clip", "motus_move_clip",
        "motus_trim_clip_start", "motus_trim_clip_end", "motus_ripple_delete",
        "motus_add_marker", "motus_remove_marker", "motus_set_track_state"};
    if (std::ranges::find(mutations, name) != mutations.end()) {
        const auto expected = requiredInteger(arguments, "expectedRevision", 0);
        if (static_cast<std::uint64_t>(expected) != project.revision) {
            throw ToolFailure("revision_conflict",
                "project revision is " + std::to_string(project.revision) +
                "; inspect again before retrying this edit");
        }
    }

    if (name == "motus_inspect_project") {
        validateArguments(arguments, {"projectPath"}, {"projectPath"});
        return Object{{"ok", true}, {"projectPath", pathText(projectPath)},
                      {"project", projectSummary(project)}};
    }
    if (name == "motus_refresh_media_integrity") {
        validateArguments(arguments, {"projectPath", "expectedRevision"},
                          {"projectPath", "expectedRevision"});
        const auto report = refreshMediaIntegrity(project);
        if (report.changed > 0U) {
            ++project.revision;
            project.validate();
            ProjectStore::saveAtomically(project, projectPath);
        }
        return Object{{"ok", true}, {"revision", project.revision},
            {"report", Object{{"checked", static_cast<std::int64_t>(report.checked)},
                {"online", static_cast<std::int64_t>(report.online)},
                {"missing", static_cast<std::int64_t>(report.missing)},
                {"modified", static_cast<std::int64_t>(report.modified)},
                {"unsupported", static_cast<std::int64_t>(report.unsupported)},
                {"changed", static_cast<std::int64_t>(report.changed)},
                {"sampledByteCount", report.sampledByteCount}}}};
    }
    if (name == "motus_import_media") {
        validateArguments(arguments, {"projectPath", "expectedRevision", "mediaPath", "displayName"},
                          {"projectPath", "expectedRevision", "mediaPath"});
        const auto mediaPath = absolutePath(requiredString(arguments, "mediaPath"));
        requireRegularFile(mediaPath, "media");
        if (samePath(mediaPath, projectPath)) {
            throw ToolFailure("source_collision", "project file cannot be imported as media");
        }
        const auto probe = probeMedia(options, mediaPath);
        const auto executable = configuredTool(options, true);
        const auto appended = appendProbedMediaReference(project, mediaPath, probe,
            optionalString(arguments, "displayName", mediaPath.filename().string()),
            toolBackend(executable));
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"assetId", appended.assetId},
                      {"videoClipId", appended.videoClipId},
                      {"audioClipId", appended.audioClipId},
                      {"linkedGroupId", appended.linkedGroupId},
                      {"probe", probeSummary(probe)}, {"revision", project.revision}};
    }
    if (name == "motus_relink_media") {
        validateArguments(arguments, {"projectPath", "expectedRevision", "assetId", "mediaPath"},
                          {"projectPath", "expectedRevision", "assetId", "mediaPath"});
        const auto assetId = requiredString(arguments, "assetId");
        const auto* asset = project.findAsset(assetId);
        if (!asset) throw ToolFailure("not_found", "asset does not exist");
        if (asset->status != AssetStatus::Missing && asset->status != AssetStatus::Modified) {
            throw ToolFailure("conflict", "only missing or modified media can be relinked");
        }
        const auto mediaPath = absolutePath(requiredString(arguments, "mediaPath"));
        requireRegularFile(mediaPath, "replacement media");
        if (samePath(mediaPath, projectPath)) {
            throw ToolFailure("source_collision", "project file cannot be used as replacement media");
        }
        const auto probe = probeMedia(options, mediaPath);
        (void)relinkAsset(project, assetId, mediaPath);
        applyMediaProbe(project, assetId, probe, false,
                        toolBackend(configuredTool(options, true)));
        ++project.revision;
        project.validate();
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"assetId", assetId}, {"revision", project.revision},
                      {"probe", probeSummary(probe)}};
    }
    if (name == "motus_append_media_reference") {
        // Compatibility for pre-probe clients. Deliberately absent from tools/list.
        validateArguments(arguments,
                          {"projectPath", "expectedRevision", "mediaPath", "durationFrames", "displayName"},
                          {"projectPath", "expectedRevision", "mediaPath", "durationFrames"});
        const auto mediaPath = absolutePath(requiredString(arguments, "mediaPath"));
        const auto appended = appendMediaReference(project, mediaPath,
            requiredInteger(arguments, "durationFrames", 1),
            optionalString(arguments, "displayName", mediaPath.filename().string()));
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"provisional", true}, {"assetId", appended.assetId},
                      {"videoClipId", appended.videoClipId}, {"audioClipId", appended.audioClipId},
                      {"linkedGroupId", appended.linkedGroupId}, {"revision", project.revision}};
    }

    const auto edit = [&](std::unique_ptr<ProjectCommand> command) {
        applyCommand(project, projectPath, std::move(command));
        return Object{{"ok", true}, {"revision", project.revision}};
    };
    if (name == "motus_split_clip" || name == "motus_move_clip" ||
        name == "motus_trim_clip_start" || name == "motus_trim_clip_end") {
        validateArguments(arguments, {"projectPath", "expectedRevision", "clipId", "timelineFrame"},
                          {"projectPath", "expectedRevision", "clipId", "timelineFrame"});
        const auto clipId = requiredString(arguments, "clipId");
        const auto position = projectFrame(project,
            requiredInteger(arguments, "timelineFrame", 0));
        if (name == "motus_split_clip") return edit(makeSplitClipCommand(clipId, position));
        if (name == "motus_move_clip") return edit(makeMoveClipCommand(clipId, position));
        if (name == "motus_trim_clip_start") return edit(makeTrimClipStartCommand(clipId, position));
        return edit(makeTrimClipEndCommand(clipId, position));
    }
    if (name == "motus_ripple_delete") {
        validateArguments(arguments,
                          {"projectPath", "expectedRevision", "sequenceId", "startFrame", "endFrame"},
                          {"projectPath", "expectedRevision", "sequenceId", "startFrame", "endFrame"});
        const auto start = requiredInteger(arguments, "startFrame", 0);
        const auto end = requiredInteger(arguments, "endFrame", 1);
        if (end <= start) throw ToolFailure("invalid_arguments", "endFrame must be greater than startFrame");
        return edit(makeRippleDeleteCommand(requiredString(arguments, "sequenceId"),
                                             projectFrame(project, start), projectFrame(project, end)));
    }
    if (name == "motus_add_marker") {
        validateArguments(arguments,
                          {"projectPath", "expectedRevision", "sequenceId", "timelineFrame", "label", "color"},
                          {"projectPath", "expectedRevision", "timelineFrame"});
        auto& sequence = requestedSequence(project, arguments);
        auto label = optionalString(arguments, "label");
        auto color = optionalString(arguments, "color", "cyan");
        if (label.size() > 256U || color.empty() || color.size() > 64U) {
            throw ToolFailure("invalid_arguments", "marker label/color is outside its supported bound");
        }
        const auto markerId = makeId();
        sequence.markers.push_back({markerId,
            projectFrame(project, requiredInteger(arguments, "timelineFrame", 0)),
            std::move(label), std::move(color)});
        ++project.revision;
        project.validate();
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"markerId", markerId}, {"revision", project.revision}};
    }
    if (name == "motus_remove_marker") {
        validateArguments(arguments, {"projectPath", "expectedRevision", "sequenceId", "markerId"},
                          {"projectPath", "expectedRevision", "markerId"});
        auto& sequence = requestedSequence(project, arguments);
        const auto markerId = requiredString(arguments, "markerId");
        const auto before = sequence.markers.size();
        std::erase_if(sequence.markers, [&](const Marker& marker) { return marker.id == markerId; });
        if (sequence.markers.size() == before) throw ToolFailure("not_found", "marker does not exist");
        ++project.revision;
        project.validate();
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"markerId", markerId}, {"revision", project.revision}};
    }
    if (name == "motus_set_track_state") {
        validateArguments(arguments,
                          {"projectPath", "expectedRevision", "trackId", "locked", "muted", "visible"},
                          {"projectPath", "expectedRevision", "trackId"});
        const bool hasLocked = arguments.contains("locked");
        const bool hasMuted = arguments.contains("muted");
        const bool hasVisible = arguments.contains("visible");
        if (!hasLocked && !hasMuted && !hasVisible) {
            throw ToolFailure("invalid_arguments", "at least one track state must be supplied");
        }
        auto* track = project.findTrack(requiredString(arguments, "trackId"));
        if (!track) throw ToolFailure("not_found", "track does not exist");
        if (hasLocked) track->locked = optionalBool(arguments, "locked");
        if (hasMuted) track->muted = optionalBool(arguments, "muted");
        if (hasVisible) track->visible = optionalBool(arguments, "visible");
        ++project.revision;
        project.validate();
        ProjectStore::saveAtomically(project, projectPath);
        return Object{{"ok", true}, {"trackId", track->id}, {"locked", track->locked},
                      {"muted", track->muted}, {"visible", track->visible},
                      {"revision", project.revision}};
    }
    if (name == "motus_generate_mlt_graph") {
        validateArguments(arguments, {"projectPath", "sequenceId", "outputPath", "overwrite"},
                          {"projectPath", "outputPath"});
        const auto& sequence = requestedSequence(project, arguments);
        const auto outputPath = absolutePath(requiredString(arguments, "outputPath"));
        if (!extensionIs(outputPath, ".mlt")) {
            throw ToolFailure("invalid_path", "outputPath must end in .mlt");
        }
        const auto graph = buildMltGraph(project, sequence, {GraphPurpose::Render, {}});
        writeAtomically(outputPath, graph.xml, optionalBool(arguments, "overwrite"),
                        protectedProjectPaths(project, projectPath));
        return Object{{"ok", true}, {"outputPath", pathText(outputPath)},
                      {"frameCount", graph.frameCount}};
    }
    if (name == "motus_export_simple") {
        validateArguments(arguments,
            {"projectPath", "sequenceId", "outputPath", "overwrite", "timeoutSeconds"},
            {"projectPath", "outputPath"});
        const auto& sequence = requestedSequence(project, arguments);
        const auto plan = buildSimpleTimelinePlan(project, sequence);
        const auto outputPath = absolutePath(requiredString(arguments, "outputPath"));
        if (!extensionIs(outputPath, ".mp4")) {
            throw ToolFailure("invalid_path", "the supported native export writes .mp4 only");
        }
        const bool overwrite = optionalBool(arguments, "overwrite");
        const auto protectedPaths = protectedProjectPaths(project, projectPath);
        protectOutput(outputPath, overwrite, protectedPaths);
        const auto parent = outputPath.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        auto staged = parent / (outputPath.stem().string() + ".motus-partial-" + makeId() + ".mp4");
        for (const auto& protectedPath : protectedPaths) {
            if (samePath(staged, protectedPath)) {
                throw ToolFailure("source_collision", "private export staging path collides with source media");
            }
        }
        const auto executable = configuredTool(options, false);
        const auto timeoutSeconds = optionalInteger(arguments, "timeoutSeconds", 1800, 1, 7200);
        ProcessResult result;
        try {
            result = execute(options, executable, buildFfmpegExportArguments(plan, staged),
                             std::chrono::seconds(timeoutSeconds));
            if (result.timedOut) throw ToolFailure("export_timeout", "FFmpeg exceeded the requested timeout");
            if (result.exitCode != 0) {
                throw ToolFailure("export_failed", "FFmpeg exited " + std::to_string(result.exitCode) +
                                                  ": " + diagnostic(result));
            }
            requireRegularFile(staged, "FFmpeg staged output");
            promoteExport(staged, outputPath, overwrite);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            throw;
        }
        std::error_code sizeError;
        const auto bytes = std::filesystem::file_size(outputPath, sizeError);
        if (sizeError) throw ToolFailure("io_error", "cannot inspect completed export: " + sizeError.message());
        return Object{{"ok", true}, {"outputPath", pathText(outputPath)},
                      {"byteSize", bytes}, {"durationUs", plan.durationUs},
                      {"segmentCount", static_cast<std::int64_t>(plan.segments.size())},
                      {"renderer", toolBackend(executable)}};
    }
    throw ToolFailure("unknown_tool", "unknown Motus tool: " + name);
}

std::string negotiatedProtocol(const json::Value* params) {
    if (!params) return std::string(currentProtocol);
    const auto* requested = params->find("protocolVersion");
    if (!requested) return std::string(currentProtocol);
    std::string value;
    try {
        value = requested->string();
    } catch (...) {
        throw std::invalid_argument("protocolVersion must be a string");
    }
    const auto supported = std::ranges::find(supportedProtocols, value);
    return supported == supportedProtocols.end() ? std::string(currentProtocol) : value;
}

} // namespace

McpServer::McpServer(McpServerOptions options) : options_(std::move(options)) {}

std::string McpServer::handle(std::string_view request) const {
    json::Value root;
    try {
        root = json::parse(request);
    } catch (const std::exception& error) {
        return json::dump(rpcError(nullId(), -32700, error.what()), 0);
    }

    const auto* id = root.find("id");
    const auto& responseId = id ? *id : nullId();
    try {
        if (root.at("jsonrpc").string() != "2.0") {
            return json::dump(rpcError(responseId, -32600, "jsonrpc must be 2.0"), 0);
        }
        const auto method = root.at("method").string();
        if (method == "notifications/initialized" || method == "notifications/cancelled") return {};
        if (!id) return {}; // JSON-RPC notifications never receive responses.
        if (method == "initialize") {
            const auto protocolVersion = negotiatedProtocol(root.find("params"));
            return json::dump(rpcResponse(responseId, Object{
                {"protocolVersion", protocolVersion},
                {"capabilities", Object{{"tools", Object{{"listChanged", false}}}}},
                {"serverInfo", Object{{"name", "motus"}, {"title", "Motus native rough-cut editor"},
                                      {"version", "0.1.0"}}},
                {"instructions", "Use Motus tools for durable local .veproj inspection and supported rough-cut edits. Media is referenced and fingerprinted read-only; no tool modifies source media. create/graph/export require explicit overwrite for existing outputs. motus_export_simple supports only the truthful gapless linked rough-cut subset described by its errors."}}), 0);
        }
        if (method == "ping") return json::dump(rpcResponse(responseId, Object{}), 0);
        if (method == "tools/list") {
            return json::dump(rpcResponse(responseId, Object{{"tools", toolCatalog()}}), 0);
        }
        if (method == "tools/call") {
            const auto& params = root.at("params");
            const auto name = params.at("name").string();
            Object arguments;
            if (const auto* value = params.find("arguments")) arguments = value->object();
            try {
                return json::dump(textResult(responseId, performTool(options_, name, arguments)), 0);
            } catch (const ToolFailure& error) {
                return json::dump(textResult(responseId, Object{{"ok", false},
                    {"error", Object{{"code", error.code()}, {"message", error.what()},
                                     {"operation", name}}}}, true), 0);
            } catch (const std::exception& error) {
                return json::dump(textResult(responseId, Object{{"ok", false},
                    {"error", Object{{"code", "operation_failed"}, {"message", error.what()},
                                     {"operation", name}}}}, true), 0);
            }
        }
        return json::dump(rpcError(responseId, -32601, "method not found"), 0);
    } catch (const std::exception& error) {
        return json::dump(rpcError(responseId, -32602, error.what()), 0);
    }
}

} // namespace ve

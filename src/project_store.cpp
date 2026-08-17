#include "ve/project_store.h"

#include "json.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace ve {
namespace {

using Object = json::Value::Object;
using Array = json::Value::Array;

json::Value timeValue(const MediaTime& time) {
    return Object{{"units", time.units}, {"rateNumerator", time.rateNumerator},
                  {"rateDenominator", time.rateDenominator}};
}

MediaTime readTime(const json::Value& value) {
    return {value.at("units").integer(), static_cast<std::int32_t>(value.at("rateNumerator").integer()),
            static_cast<std::int32_t>(value.at("rateDenominator").integer())};
}

template <typename Enum>
std::int32_t enumValue(Enum value) { return static_cast<std::int32_t>(value); }

template <typename Enum>
Enum readEnum(const json::Value& value, std::int32_t maximum) {
    const auto raw = value.integer();
    if (raw < 0 || raw > maximum) throw std::runtime_error("enumeration value out of range");
    return static_cast<Enum>(raw);
}

json::Value keyframeValue(const Keyframe& keyframe) {
    return Object{{"time", timeValue(keyframe.time)}, {"value", keyframe.value},
                  {"interpolation", enumValue(keyframe.interpolation)}};
}

Keyframe readKeyframe(const json::Value& value) {
    return {readTime(value.at("time")), value.at("value").number(),
            readEnum<Keyframe::Interpolation>(value.at("interpolation"), 2)};
}

json::Value effectValue(const EffectInstance& effect) {
    Object parameters;
    for (const auto& [name, value] : effect.parameters) parameters.emplace(name, value);
    Object animations;
    for (const auto& [name, keyframes] : effect.animations) {
        Array values;
        for (const auto& keyframe : keyframes) values.push_back(keyframeValue(keyframe));
        animations.emplace(name, std::move(values));
    }
    return Object{{"id", effect.id}, {"service", effect.service}, {"enabled", effect.enabled},
                  {"parameters", std::move(parameters)}, {"animations", std::move(animations)}};
}

EffectInstance readEffect(const json::Value& value) {
    EffectInstance effect;
    effect.id = value.at("id").string();
    effect.service = value.at("service").string();
    effect.enabled = value.at("enabled").boolean();
    for (const auto& [name, item] : value.at("parameters").object()) effect.parameters[name] = item.string();
    for (const auto& [name, items] : value.at("animations").object()) {
        for (const auto& item : items.array()) effect.animations[name].push_back(readKeyframe(item));
    }
    return effect;
}

json::Value clipValue(const Clip& clip) {
    Array effects;
    for (const auto& effect : clip.effects) effects.push_back(effectValue(effect));
    return Object{{"id", clip.id}, {"assetId", clip.assetId}, {"linkedGroupId", clip.linkedGroupId},
                  {"timelineStart", timeValue(clip.timelineStart)}, {"sourceIn", timeValue(clip.sourceIn)},
                  {"duration", timeValue(clip.duration)}, {"audioFadeIn", timeValue(clip.audioFadeIn)},
                  {"audioFadeOut", timeValue(clip.audioFadeOut)}, {"speed", clip.speed},
                  {"effects", std::move(effects)}};
}

Clip readClip(const json::Value& value) {
    Clip clip;
    clip.id = value.at("id").string();
    clip.assetId = value.at("assetId").string();
    if (const auto* item = value.find("linkedGroupId")) clip.linkedGroupId = item->string();
    clip.timelineStart = readTime(value.at("timelineStart"));
    clip.sourceIn = readTime(value.at("sourceIn"));
    clip.duration = readTime(value.at("duration"));
    clip.audioFadeIn = value.find("audioFadeIn") ? readTime(*value.find("audioFadeIn")) : MediaTime{};
    clip.audioFadeOut = value.find("audioFadeOut") ? readTime(*value.find("audioFadeOut")) : MediaTime{};
    clip.speed = value.find("speed") ? value.find("speed")->number() : 1.0;
    if (const auto* effects = value.find("effects")) {
        for (const auto& effect : effects->array()) clip.effects.push_back(readEffect(effect));
    }
    return clip;
}

json::Value trackValue(const Track& track) {
    Array clips;
    for (const auto& clip : track.clips) clips.push_back(clipValue(clip));
    return Object{{"id", track.id}, {"name", track.name}, {"kind", enumValue(track.kind)},
                  {"locked", track.locked}, {"muted", track.muted}, {"visible", track.visible},
                  {"clips", std::move(clips)}};
}

Track readTrack(const json::Value& value) {
    Track track;
    track.id = value.at("id").string();
    track.name = value.at("name").string();
    track.kind = readEnum<TrackKind>(value.at("kind"), 1);
    track.locked = value.find("locked") && value.find("locked")->boolean();
    track.muted = value.find("muted") && value.find("muted")->boolean();
    track.visible = !value.find("visible") || value.find("visible")->boolean();
    for (const auto& clip : value.at("clips").array()) track.clips.push_back(readClip(clip));
    return track;
}

json::Value sequenceValue(const Sequence& sequence) {
    Array tracks;
    for (const auto& track : sequence.tracks) tracks.push_back(trackValue(track));
    Array transitions;
    for (const auto& transition : sequence.transitions) {
        transitions.push_back(Object{{"id", transition.id}, {"fromClipId", transition.fromClipId},
            {"toClipId", transition.toClipId}, {"service", transition.service},
            {"duration", timeValue(transition.duration)}});
    }
    Array markers;
    for (const auto& marker : sequence.markers) {
        markers.push_back(Object{{"id", marker.id}, {"time", timeValue(marker.time)},
                                 {"label", marker.label}, {"color", marker.color}});
    }
    return Object{{"id", sequence.id}, {"name", sequence.name}, {"tracks", std::move(tracks)},
                  {"transitions", std::move(transitions)}, {"markers", std::move(markers)}};
}

Sequence readSequence(const json::Value& value) {
    Sequence sequence;
    sequence.id = value.at("id").string();
    sequence.name = value.at("name").string();
    for (const auto& track : value.at("tracks").array()) sequence.tracks.push_back(readTrack(track));
    if (const auto* values = value.find("transitions")) for (const auto& item : values->array()) {
        sequence.transitions.push_back({item.at("id").string(), item.at("fromClipId").string(),
            item.at("toClipId").string(), item.at("service").string(), readTime(item.at("duration"))});
    }
    if (const auto* values = value.find("markers")) for (const auto& item : values->array()) {
        sequence.markers.push_back({item.at("id").string(), readTime(item.at("time")),
            item.at("label").string(), item.at("color").string()});
    }
    return sequence;
}

json::Value mediaStreamValue(const MediaStreamInfo& stream) {
    return Object{{"index", stream.index}, {"kind", enumValue(stream.kind)},
        {"codec", stream.codec}, {"width", stream.width}, {"height", stream.height},
        {"averageRateNumerator", stream.averageRateNumerator},
        {"averageRateDenominator", stream.averageRateDenominator},
        {"nominalRateNumerator", stream.nominalRateNumerator},
        {"nominalRateDenominator", stream.nominalRateDenominator},
        {"sampleRate", stream.sampleRate}, {"channels", stream.channels},
        {"rotationDegrees", stream.rotationDegrees}, {"frameCount", stream.frameCount},
        {"pixelFormat", stream.pixelFormat},
        {"colorSpace", stream.colorSpace}, {"channelLayout", stream.channelLayout},
        {"timeBase", stream.timeBase}, {"duration", timeValue(stream.duration)}};
}

MediaStreamInfo readMediaStream(const json::Value& value) {
    MediaStreamInfo stream;
    stream.index = static_cast<std::int32_t>(value.at("index").integer());
    stream.kind = readEnum<MediaStreamKind>(value.at("kind"), 2);
    stream.codec = value.at("codec").string();
    stream.width = static_cast<std::int32_t>(value.at("width").integer());
    stream.height = static_cast<std::int32_t>(value.at("height").integer());
    stream.averageRateNumerator = static_cast<std::int32_t>(value.at("averageRateNumerator").integer());
    stream.averageRateDenominator = static_cast<std::int32_t>(value.at("averageRateDenominator").integer());
    stream.nominalRateNumerator = static_cast<std::int32_t>(value.at("nominalRateNumerator").integer());
    stream.nominalRateDenominator = static_cast<std::int32_t>(value.at("nominalRateDenominator").integer());
    stream.sampleRate = static_cast<std::int32_t>(value.at("sampleRate").integer());
    stream.channels = static_cast<std::int32_t>(value.at("channels").integer());
    stream.rotationDegrees = static_cast<std::int32_t>(value.at("rotationDegrees").integer());
    if (const auto* item = value.find("frameCount")) stream.frameCount = item->integer();
    stream.pixelFormat = value.at("pixelFormat").string();
    stream.colorSpace = value.at("colorSpace").string();
    if (const auto* item = value.find("channelLayout")) stream.channelLayout = item->string();
    if (const auto* item = value.find("timeBase")) stream.timeBase = item->string();
    if (const auto* item = value.find("duration")) stream.duration = readTime(*item);
    return stream;
}

json::Value mediaProbeValue(const MediaProbeResult& probe) {
    Array streams;
    for (const auto& stream : probe.streams) streams.push_back(mediaStreamValue(stream));
    Array warnings;
    for (const auto& warning : probe.warnings) warnings.emplace_back(warning);
    return Object{{"duration", timeValue(probe.duration)}, {"streams", std::move(streams)},
        {"formatName", probe.formatName}, {"bitRate", probe.bitRate},
        {"variableFrameRate", probe.variableFrameRate},
        {"proxyRecommended", probe.proxyRecommended}, {"warnings", std::move(warnings)}};
}

MediaProbeResult readMediaProbe(const json::Value& value) {
    MediaProbeResult probe;
    probe.duration = readTime(value.at("duration"));
    for (const auto& stream : value.at("streams").array()) {
        probe.streams.push_back(readMediaStream(stream));
    }
    if (const auto* item = value.find("formatName")) probe.formatName = item->string();
    if (const auto* item = value.find("bitRate")) probe.bitRate = item->integer();
    probe.variableFrameRate = value.find("variableFrameRate") &&
                              value.find("variableFrameRate")->boolean();
    probe.proxyRecommended = value.find("proxyRecommended") &&
                             value.find("proxyRecommended")->boolean();
    if (const auto* warnings = value.find("warnings")) {
        for (const auto& warning : warnings->array()) probe.warnings.push_back(warning.string());
    }
    return probe;
}

json::Value assetValue(const Asset& asset) {
    Object value{{"id", asset.id}, {"path", asset.path.generic_string()},
        {"displayName", asset.displayName}, {"status", enumValue(asset.status)},
        {"duration", timeValue(asset.duration)}, {"hasVideo", asset.hasVideo},
        {"hasAudio", asset.hasAudio}, {"proxyEligible", asset.proxyEligible},
        {"fingerprint", Object{{"byteSize", asset.fingerprint.byteSize},
            {"modifiedUtcMs", asset.fingerprint.modifiedUtcMs},
            {"headTailSha256", asset.fingerprint.headTailSha256}}}};
    value.emplace("relativePath", asset.relativePath ? json::Value(asset.relativePath->generic_string())
                                                     : json::Value(nullptr));
    value.emplace("probe", asset.probe ? mediaProbeValue(*asset.probe) : json::Value(nullptr));
    value.emplace("probedUtcMs", asset.probedUtcMs);
    value.emplace("probeBackend", asset.probeBackend);
    return value;
}

Asset readAsset(const json::Value& value) {
    Asset asset;
    asset.id = value.at("id").string();
    asset.path = std::filesystem::path(value.at("path").string());
    if (const auto* relative = value.find("relativePath"); relative &&
        std::holds_alternative<std::string>(relative->data)) asset.relativePath = relative->string();
    asset.displayName = value.at("displayName").string();
    asset.status = value.find("status") ? readEnum<AssetStatus>(*value.find("status"), 3) : AssetStatus::Online;
    asset.duration = readTime(value.at("duration"));
    asset.hasVideo = !value.find("hasVideo") || value.find("hasVideo")->boolean();
    asset.hasAudio = !value.find("hasAudio") || value.find("hasAudio")->boolean();
    asset.proxyEligible = value.find("proxyEligible") && value.find("proxyEligible")->boolean();
    if (const auto* probe = value.find("probe"); probe &&
        !std::holds_alternative<std::nullptr_t>(probe->data)) {
        asset.probe = readMediaProbe(*probe);
    }
    if (const auto* item = value.find("probedUtcMs")) asset.probedUtcMs = item->integer();
    if (const auto* item = value.find("probeBackend")) asset.probeBackend = item->string();
    const auto& fingerprint = value.at("fingerprint");
    asset.fingerprint = {static_cast<std::uint64_t>(fingerprint.at("byteSize").integer()),
        fingerprint.at("modifiedUtcMs").integer(), fingerprint.at("headTailSha256").string()};
    return asset;
}

json::Value suggestionValue(const CleanupSuggestion& suggestion) {
    return Object{{"id", suggestion.id}, {"assetId", suggestion.assetId},
        {"sequenceId", suggestion.sequenceId}, {"kind", enumValue(suggestion.kind)},
        {"state", enumValue(suggestion.state)}, {"sourceStart", timeValue(suggestion.sourceStart)},
        {"sourceEnd", timeValue(suggestion.sourceEnd)},
        {"replacementDuration", timeValue(suggestion.replacementDuration)},
        {"confidence", suggestion.confidence}, {"speechFreeHandles", suggestion.speechFreeHandles},
        {"transcriptContext", suggestion.transcriptContext},
        {"analysisCacheKey", suggestion.analysisCacheKey}};
}

CleanupSuggestion readSuggestion(const json::Value& value) {
    CleanupSuggestion result;
    result.id = value.at("id").string(); result.assetId = value.at("assetId").string();
    result.sequenceId = value.at("sequenceId").string();
    result.kind = readEnum<CleanupKind>(value.at("kind"), 2);
    result.state = readEnum<SuggestionState>(value.at("state"), 5);
    result.sourceStart = readTime(value.at("sourceStart")); result.sourceEnd = readTime(value.at("sourceEnd"));
    result.replacementDuration = readTime(value.at("replacementDuration"));
    result.confidence = value.at("confidence").number(); result.speechFreeHandles = value.at("speechFreeHandles").boolean();
    result.transcriptContext = value.at("transcriptContext").string();
    result.analysisCacheKey = value.at("analysisCacheKey").string();
    return result;
}

Project fromJson(const json::Value& root, const std::filesystem::path& path) {
    const auto inputVersion = static_cast<std::int32_t>(root.at("schemaVersion").integer());
    if (inputVersion < 1 || inputVersion > Project::currentSchemaVersion) {
        throw std::runtime_error("project was created by an unsupported schema version");
    }
    Project project;
    project.schemaVersion = Project::currentSchemaVersion;
    project.id = root.find("id") ? root.find("id")->string() : makeId();
    project.name = root.at("name").string();
    project.projectPath = path;
    project.revision = root.find("revision") ? static_cast<std::uint64_t>(root.find("revision")->integer()) : 0;
    const auto& profile = root.at("profile");
    project.profile.width = static_cast<std::int32_t>(profile.at("width").integer());
    project.profile.height = static_cast<std::int32_t>(profile.at("height").integer());
    project.profile.frameRateNumerator = static_cast<std::int32_t>(profile.at("frameRateNumerator").integer());
    project.profile.frameRateDenominator = profile.find("frameRateDenominator")
        ? static_cast<std::int32_t>(profile.find("frameRateDenominator")->integer()) : 1;
    project.profile.audioSampleRate = static_cast<std::int32_t>(profile.at("audioSampleRate").integer());
    project.profile.audioChannels = profile.find("audioChannels")
        ? static_cast<std::int32_t>(profile.find("audioChannels")->integer()) : 2;
    project.profile.colorSpace = profile.find("colorSpace") ? profile.find("colorSpace")->string() : "Rec.709 SDR";
    for (const auto& item : root.at("assets").array()) project.assets.push_back(readAsset(item));
    for (const auto& item : root.at("sequences").array()) project.sequences.push_back(readSequence(item));
    if (const auto* values = root.find("cleanupSuggestions"))
        for (const auto& item : values->array()) project.cleanupSuggestions.push_back(readSuggestion(item));
    if (const auto* active = root.find("activeSequenceId"); active &&
        std::holds_alternative<std::string>(active->data)) project.activeSequenceId = active->string();
    else if (!project.sequences.empty()) project.activeSequenceId = project.sequences.front().id;
    project.validate();
    return project;
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open project for writing: " + path.string());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) throw std::runtime_error("failed while writing project: " + path.string());
}

} // namespace

std::string ProjectStore::serialize(const Project& project) {
    project.validate();
    Array assets; for (const auto& asset : project.assets) assets.push_back(assetValue(asset));
    Array sequences; for (const auto& sequence : project.sequences) sequences.push_back(sequenceValue(sequence));
    Array suggestions; for (const auto& suggestion : project.cleanupSuggestions) suggestions.push_back(suggestionValue(suggestion));
    const auto& profile = project.profile;
    Object root{{"schemaVersion", Project::currentSchemaVersion}, {"id", project.id},
        {"name", project.name}, {"revision", project.revision},
        {"profile", Object{{"width", profile.width}, {"height", profile.height},
            {"frameRateNumerator", profile.frameRateNumerator},
            {"frameRateDenominator", profile.frameRateDenominator},
            {"audioSampleRate", profile.audioSampleRate}, {"audioChannels", profile.audioChannels},
            {"colorSpace", profile.colorSpace}}}, {"assets", std::move(assets)},
        {"sequences", std::move(sequences)}, {"cleanupSuggestions", std::move(suggestions)},
        {"activeSequenceId", project.activeSequenceId ? json::Value(*project.activeSequenceId) : json::Value(nullptr)}};
    return json::dump(root);
}

Project ProjectStore::deserialize(std::string_view document, const std::filesystem::path& projectPath) {
    return fromJson(json::parse(document), projectPath);
}

void ProjectStore::saveAtomically(const Project& project, const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("project path is empty");
    std::filesystem::create_directories(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    const auto backup = std::filesystem::path(path.string() + ".bak");
    writeFile(temporary, serialize(project));
    std::error_code error;
    std::filesystem::remove(backup, error);
    error.clear();
    if (std::filesystem::exists(path)) {
        std::filesystem::rename(path, backup, error);
        if (error) { std::filesystem::remove(temporary); throw std::runtime_error("cannot rotate project backup: " + error.message()); }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        if (std::filesystem::exists(backup)) { std::error_code ignored; std::filesystem::rename(backup, path, ignored); }
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot commit project save: " + error.message());
    }
}

Project ProjectStore::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open project: " + path.string());
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return deserialize(contents, path);
}

std::filesystem::path ProjectStore::saveRecoverySnapshot(const Project& project,
                                                         const std::filesystem::path& directory,
                                                         std::size_t maxSnapshots) {
    if (maxSnapshots == 0) throw std::invalid_argument("recovery snapshot count must be positive");
    std::filesystem::create_directories(directory);
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = directory / (project.id + "-" + std::to_string(stamp) + ".recovery.veproj");
    saveAtomically(project, path);
    std::vector<std::filesystem::directory_entry> snapshots;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".veproj" &&
            entry.path().filename().string().starts_with(project.id + "-")) snapshots.push_back(entry);
    }
    std::ranges::sort(snapshots, [](const auto& lhs, const auto& rhs) {
        return lhs.last_write_time() > rhs.last_write_time();
    });
    for (std::size_t index = maxSnapshots; index < snapshots.size(); ++index) {
        std::error_code ignored; std::filesystem::remove(snapshots[index], ignored);
    }
    return path;
}

} // namespace ve

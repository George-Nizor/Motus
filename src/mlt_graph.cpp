#include "ve/mlt_graph.h"

#include "ve/media_integrity.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ve {
namespace {

std::string xmlEscape(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char value : input) {
        switch (value) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&apos;"; break;
        default: output += value;
        }
    }
    return output;
}

std::int64_t framesAtProfile(const ProjectProfile& profile, const MediaTime& time,
                             Rounding rounding = Rounding::Nearest) {
    return time.rescaled(profile.frameRateNumerator, profile.frameRateDenominator, rounding).units;
}

} // namespace

MltGraph buildMltGraph(const Project& project, const Sequence& sequence,
                       const MltGraphOptions& options) {
    project.validate();
    const auto& profile = project.profile;
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<mlt LC_NUMERIC=\"C\" version=\"7.40.0\" root=\""
        << xmlEscape(project.projectPath.parent_path().generic_string()) << "\">\n"
        << "  <profile width=\"" << profile.width << "\" height=\"" << profile.height
        << "\" frame_rate_num=\"" << profile.frameRateNumerator
        << "\" frame_rate_den=\"" << profile.frameRateDenominator
        << "\" colorspace=\"709\" progressive=\"1\"/>\n";

    std::unordered_set<Id> usedAssets;
    for (const auto& track : sequence.tracks)
        for (const auto& clip : track.clips) usedAssets.insert(clip.assetId);
    for (const auto& assetId : usedAssets) {
        const auto* asset = project.findAsset(assetId);
        if (!asset) throw std::runtime_error("MLT graph references missing asset");
        auto resource = resolveAssetPath(project, *asset);
        if (options.purpose == GraphPurpose::Preview) {
            const auto proxy = options.proxyPaths.find(assetId);
            if (proxy != options.proxyPaths.end()) resource = proxy->second;
        }
        xml << "  <producer id=\"asset_" << xmlEscape(assetId) << "\">\n"
            << "    <property name=\"mlt_service\">avformat-novalidate</property>\n"
            << "    <property name=\"resource\">" << xmlEscape(resource.generic_string())
            << "</property>\n  </producer>\n";
    }

    std::int64_t totalFrames = 0;
    for (std::size_t trackIndex = 0; trackIndex < sequence.tracks.size(); ++trackIndex) {
        const auto& track = sequence.tracks[trackIndex];
        xml << "  <playlist id=\"playlist_" << trackIndex << "\">\n";
        std::int64_t cursor = 0;
        for (const auto& clip : track.clips) {
            const auto start = framesAtProfile(profile, clip.timelineStart);
            const auto duration = framesAtProfile(profile, clip.duration);
            if (duration <= 0) throw std::runtime_error("clip collapses below one output frame");
            if (start < cursor) throw std::runtime_error("MLT graph cannot contain overlapping playlist clips");
            if (start > cursor) xml << "    <blank length=\"" << (start - cursor) << "\"/>\n";
            const auto sourceIn = framesAtProfile(profile, clip.sourceIn, Rounding::Down);
            const auto sourceOut = sourceIn + duration - 1; // MLT out is inclusive.
            xml << "    <entry producer=\"asset_" << xmlEscape(clip.assetId) << "\" in=\""
                << sourceIn << "\" out=\"" << sourceOut << "\"/>\n";
            cursor = start + duration;
        }
        totalFrames = std::max(totalFrames, cursor);
        xml << "  </playlist>\n";
    }
    xml << "  <tractor id=\"sequence_" << xmlEscape(sequence.id) << "\" in=\"0\" out=\""
        << (totalFrames > 0 ? totalFrames - 1 : 0) << "\">\n";
    for (std::size_t trackIndex = 0; trackIndex < sequence.tracks.size(); ++trackIndex) {
        const auto& track = sequence.tracks[trackIndex];
        std::string_view hidden;
        if (track.kind == TrackKind::Video) hidden = track.visible ? "audio" : "both";
        else hidden = track.muted ? "both" : "video";
        xml << "    <track producer=\"playlist_" << trackIndex << "\" hide=\"" << hidden
            << "\"/>\n";
    }
    xml << "  </tractor>\n</mlt>\n";
    return {xml.str(), totalFrames};
}

} // namespace ve

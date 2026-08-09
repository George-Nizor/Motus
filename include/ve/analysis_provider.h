#pragma once

#include "ve/project.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ve {

inline constexpr std::int32_t analysisProtocolVersion = 1;

struct AnalysisRequest {
    std::string requestId;
    AssetFingerprint assetFingerprint;
    std::int32_t audioStreamIndex{0};
    std::string language{"en"};
    std::string model;
    std::string settingsJson{"{}"};
};

struct AnalysisEvent {
    std::string kind;
    std::string text;
    MediaTime start;
    MediaTime end;
    double confidence{0.0};
};

struct AnalysisResult {
    std::int32_t protocolVersion{analysisProtocolVersion};
    std::string requestId;
    std::string provider;
    std::string modelVersion;
    std::string cacheKey;
    std::vector<TranscriptWord> words;
    std::vector<AnalysisEvent> events;
};

// Provider processes live out-of-process. On Windows, implementations use a local named pipe;
// messages are newline-delimited JSON-RPC 2.0 and never embed source media.
class IAnalysisProvider {
public:
    virtual ~IAnalysisProvider() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::vector<std::string> availableModels() const = 0;
    virtual AnalysisResult analyze(const AnalysisRequest& request,
                                   const std::function<bool()>& cancelled) = 0;
};

} // namespace ve


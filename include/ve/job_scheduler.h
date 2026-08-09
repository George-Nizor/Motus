#pragma once

#include "ve/project.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ve {

enum class JobKind { Probe, Proxy, Waveform, Transcription, FaceAnalysis, Render };
enum class JobState { Queued, Running, Completed, Failed, Cancelled, Interrupted };
enum class JobResource { Cpu, Gpu };

struct BackgroundJob {
    Id id;
    JobKind kind{JobKind::Probe};
    JobState state{JobState::Queued};
    JobResource resource{JobResource::Cpu};
    std::string label;
    double progress{0.0};
    std::string error;
};

using JobWork = std::function<void(std::stop_token, const std::function<void(double)>&)>;

class JobScheduler {
public:
    explicit JobScheduler(std::size_t cpuConcurrency = 2,
                          std::filesystem::path journalPath = {});
    ~JobScheduler();
    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    [[nodiscard]] Id enqueue(JobKind kind, JobResource resource, std::string label, JobWork work);
    bool cancel(const Id& jobId);
    [[nodiscard]] std::optional<BackgroundJob> job(const Id& jobId) const;
    [[nodiscard]] std::vector<BackgroundJob> jobs() const;
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ve


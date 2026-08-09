#include "test.h"
#include "ve/job_scheduler.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST("Scheduler completes jobs and serializes GPU work") {
    ve::JobScheduler scheduler(3);
    std::atomic<int> gpuRunning{0};
    std::atomic<int> maximumGpu{0};
    const auto work = [&](std::stop_token, const auto& progress) {
        const int now = ++gpuRunning;
        maximumGpu.store(std::max(maximumGpu.load(), now));
        progress(0.5);
        std::this_thread::sleep_for(15ms);
        --gpuRunning;
    };
    const auto first = scheduler.enqueue(ve::JobKind::Transcription, ve::JobResource::Gpu, "one", work);
    const auto second = scheduler.enqueue(ve::JobKind::FaceAnalysis, ve::JobResource::Gpu, "two", work);
    CHECK(scheduler.waitUntilIdle(2s));
    CHECK(maximumGpu.load() == 1);
    CHECK(scheduler.job(first)->state == ve::JobState::Completed);
    CHECK(scheduler.job(second)->progress == 1.0);
}

TEST("Queued jobs can be cancelled") {
    ve::JobScheduler scheduler(1);
    std::atomic<bool> release{false};
    const auto blocker = scheduler.enqueue(ve::JobKind::Render, ve::JobResource::Cpu, "blocker",
        [&](std::stop_token token, const auto&) { while (!release.load() && !token.stop_requested()) std::this_thread::yield(); });
    (void)blocker;
    const auto cancelled = scheduler.enqueue(ve::JobKind::Proxy, ve::JobResource::Cpu, "cancel", [](auto, const auto&) {});
    CHECK(scheduler.cancel(cancelled));
    CHECK(scheduler.job(cancelled)->state == ve::JobState::Cancelled);
    release = true;
    CHECK(scheduler.waitUntilIdle(2s));
}

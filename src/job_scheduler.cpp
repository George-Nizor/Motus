#include "ve/job_scheduler.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace ve {

struct JobScheduler::Impl {
    struct Item {
        BackgroundJob view;
        JobWork work;
        std::stop_source stop;
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable idleCondition;
    std::vector<std::shared_ptr<Item>> queue;
    std::unordered_map<Id, std::shared_ptr<Item>> all;
    std::vector<std::jthread> workers;
    std::filesystem::path journalPath;
    bool stopping{false};
    bool gpuBusy{false};
    std::size_t running{0};

    explicit Impl(std::size_t concurrency, std::filesystem::path journal)
        : journalPath(std::move(journal)) {
        if (concurrency == 0) throw std::invalid_argument("CPU concurrency must be positive");
        for (std::size_t index = 0; index < concurrency; ++index) {
            workers.emplace_back([this](std::stop_token token) { worker(token); });
        }
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (auto& [id, item] : all) {
                (void)id;
                if (item->view.state == JobState::Queued || item->view.state == JobState::Running)
                    item->stop.request_stop();
            }
        }
        condition.notify_all();
    }

    void journalLocked() const {
        if (journalPath.empty()) return;
        const auto temporary = std::filesystem::path(journalPath.string() + ".tmp");
        std::filesystem::create_directories(journalPath.parent_path().empty()
                                                ? std::filesystem::path(".")
                                                : journalPath.parent_path());
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << "VEJOBS\t1\n";
        for (const auto& [id, item] : all) {
            output << id << '\t' << static_cast<int>(item->view.kind) << '\t'
                   << static_cast<int>(item->view.state) << '\t'
                   << static_cast<int>(item->view.resource) << '\t' << item->view.progress << '\t';
            for (char value : item->view.label) output << (value == '\t' || value == '\n' ? ' ' : value);
            output << '\n';
        }
        output.close();
        std::error_code ignored;
        std::filesystem::remove(journalPath, ignored);
        std::filesystem::rename(temporary, journalPath, ignored);
    }

    std::shared_ptr<Item> nextLocked() {
        const auto iterator = std::ranges::find_if(queue, [this](const auto& item) {
            return item->view.resource == JobResource::Cpu || !gpuBusy;
        });
        if (iterator == queue.end()) return {};
        auto item = *iterator;
        queue.erase(iterator);
        if (item->view.resource == JobResource::Gpu) gpuBusy = true;
        ++running;
        item->view.state = JobState::Running;
        journalLocked();
        return item;
    }

    void worker(std::stop_token workerStop) {
        while (!workerStop.stop_requested()) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this, &workerStop] {
                    if (stopping || workerStop.stop_requested()) return true;
                    return std::ranges::any_of(queue, [this](const auto& candidate) {
                        return candidate->view.resource == JobResource::Cpu || !gpuBusy;
                    });
                });
                if (stopping || workerStop.stop_requested()) return;
                item = nextLocked();
            }
            try {
                item->work(item->stop.get_token(), [this, weak = std::weak_ptr<Item>(item)](double progress) {
                    std::lock_guard lock(mutex);
                    if (const auto target = weak.lock()) target->view.progress = std::clamp(progress, 0.0, 1.0);
                });
                std::lock_guard lock(mutex);
                item->view.state = item->stop.stop_requested() ? JobState::Cancelled : JobState::Completed;
                if (item->view.state == JobState::Completed) item->view.progress = 1.0;
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex);
                item->view.state = item->stop.stop_requested() ? JobState::Cancelled : JobState::Failed;
                item->view.error = error.what();
            } catch (...) {
                std::lock_guard lock(mutex);
                item->view.state = JobState::Failed;
                item->view.error = "unknown job failure";
            }
            {
                std::lock_guard lock(mutex);
                if (item->view.resource == JobResource::Gpu) gpuBusy = false;
                --running;
                journalLocked();
                if (queue.empty() && running == 0) idleCondition.notify_all();
            }
            condition.notify_all();
        }
    }
};

JobScheduler::JobScheduler(std::size_t cpuConcurrency, std::filesystem::path journalPath)
    : impl_(std::make_unique<Impl>(cpuConcurrency, std::move(journalPath))) {}
JobScheduler::~JobScheduler() = default;

Id JobScheduler::enqueue(JobKind kind, JobResource resource, std::string label, JobWork work) {
    if (!work) throw std::invalid_argument("job work is empty");
    auto item = std::make_shared<Impl::Item>();
    item->view = {makeId(), kind, JobState::Queued, resource, std::move(label), 0.0, {}};
    item->work = std::move(work);
    const auto id = item->view.id;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->all.emplace(id, item);
        impl_->queue.push_back(std::move(item));
        impl_->journalLocked();
    }
    impl_->condition.notify_one();
    return id;
}

bool JobScheduler::cancel(const Id& jobId) {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->all.find(jobId);
    if (iterator == impl_->all.end()) return false;
    auto& item = iterator->second;
    if (item->view.state == JobState::Queued) {
        std::erase(impl_->queue, item);
        item->view.state = JobState::Cancelled;
        impl_->journalLocked();
        if (impl_->queue.empty() && impl_->running == 0) impl_->idleCondition.notify_all();
        return true;
    }
    if (item->view.state == JobState::Running) {
        item->stop.request_stop();
        return true;
    }
    return false;
}

std::optional<BackgroundJob> JobScheduler::job(const Id& jobId) const {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->all.find(jobId);
    return iterator == impl_->all.end() ? std::nullopt : std::optional(iterator->second->view);
}

std::vector<BackgroundJob> JobScheduler::jobs() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<BackgroundJob> result;
    result.reserve(impl_->all.size());
    for (const auto& [id, item] : impl_->all) { (void)id; result.push_back(item->view); }
    return result;
}

bool JobScheduler::waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->idleCondition.wait_for(lock, timeout,
        [this] { return impl_->queue.empty() && impl_->running == 0; });
}

} // namespace ve

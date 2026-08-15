// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SharedAnalysisScheduler.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace audio_insight
{

struct SharedAnalysisScheduler::Client::State final
{
    explicit State(std::weak_ptr<JobClient> targetToUse) noexcept
        : target(std::move(targetToUse))
    {
    }

    std::weak_ptr<JobClient> target;

    // The following scheduling fields are protected by Impl::mutex.
    bool accepting { true };
    bool queued { false };
    bool running { false };
    bool followUpRequested { false };
    Generation queuedGeneration { 1 };
    Generation followUpGeneration { 1 };
    std::shared_ptr<std::atomic<bool>> runningStopFlag;
    std::thread::id runningThread;

    std::atomic<Generation> currentGeneration { 1 };
    std::atomic<std::uint64_t> submitted { 0 };
    std::atomic<std::uint64_t> executed { 0 };
    std::atomic<std::uint64_t> cancelled { 0 };
};

struct SharedAnalysisScheduler::Impl final
{
    explicit Impl(const std::size_t requestedWorkerCount)
        : configuredWorkerCount(requestedWorkerCount),
          schedulerStopFlag(std::make_shared<std::atomic<bool>>(false))
    {
        workers.reserve(configuredWorkerCount);

        try
        {
            for (std::size_t index = 0; index < configuredWorkerCount; ++index)
                workers.emplace_back([this] { workerLoop(); });
        }
        catch (...)
        {
            {
                std::lock_guard lock(mutex);
                stopping = true;
                schedulerStopFlag->store(true, std::memory_order_release);
            }

            readyCondition.notify_all();
            for (auto& worker : workers)
                worker.join();
            throw;
        }
    }

    ~Impl()
    {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void eraseQueuedEntriesFor(const std::shared_ptr<Client::State>& state)
    {
        std::erase_if(readyQueue, [&state](const auto& queuedState) {
            const auto candidate = queuedState.lock();
            return ! candidate || candidate == state;
        });
    }

    static void cancelQueued(Client::State& state) noexcept
    {
        if (state.queued)
        {
            state.queued = false;
            state.cancelled.fetch_add(1, std::memory_order_relaxed);
        }

        if (state.followUpRequested)
        {
            state.followUpRequested = false;
            state.cancelled.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] static bool isIdle(const Client::State& state) noexcept
    {
        return ! state.queued && ! state.running && ! state.followUpRequested;
    }

    void workerLoop()
    {
#if defined(__APPLE__)
        // Analysis needs timely service but must remain below the host's audio
        // callback. This is intentionally not a real-time scheduling policy.
        static_cast<void>(pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0));
#endif

        for (;;)
        {
            std::shared_ptr<Client::State> state;
            std::shared_ptr<JobClient> target;
            Generation jobGeneration = 0;
            std::shared_ptr<std::atomic<bool>> generationStopFlag;

            {
                std::unique_lock lock(mutex);
                readyCondition.wait(lock, [this] {
                    return stopping || ! readyQueue.empty();
                });

                if (stopping)
                    return;

                while (! readyQueue.empty() && ! state)
                {
                    state = readyQueue.front().lock();
                    readyQueue.pop_front();

                    if (! state || ! state->queued)
                        state.reset();
                }

                if (! state)
                    continue;

                state->queued = false;
                jobGeneration = state->queuedGeneration;

                if (! state->accepting
                    || jobGeneration
                           != state->currentGeneration.load(std::memory_order_acquire))
                {
                    state->cancelled.fetch_add(1, std::memory_order_relaxed);
                    idleCondition.notify_all();
                    continue;
                }

                target = state->target.lock();
                if (! target)
                {
                    state->accepting = false;
                    state->cancelled.fetch_add(1, std::memory_order_relaxed);
                    idleCondition.notify_all();
                    continue;
                }

                state->running = true;
                state->runningThread = std::this_thread::get_id();
                state->runningStopFlag =
                    std::make_shared<std::atomic<bool>>(false);
                generationStopFlag = state->runningStopFlag;
                state->executed.fetch_add(1, std::memory_order_relaxed);
            }

            const JobContext context(
                jobGeneration, schedulerStopFlag, generationStopFlag);

            try
            {
                target->execute(context);
            }
            catch (...)
            {
                // A plugin analysis failure must not silently retire a shared
                // worker and starve every other plugin instance.
            }

            {
                std::lock_guard lock(mutex);
                state->running = false;
                state->runningThread = {};
                state->runningStopFlag.reset();

                if (! stopping && state->accepting && state->followUpRequested)
                {
                    const auto followUpGeneration = state->followUpGeneration;
                    state->followUpRequested = false;

                    if (followUpGeneration
                        == state->currentGeneration.load(std::memory_order_acquire))
                    {
                        state->queued = true;
                        state->queuedGeneration = followUpGeneration;
                        readyQueue.emplace_back(state);
                        readyCondition.notify_one();
                    }
                    else
                    {
                        state->cancelled.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else if (state->followUpRequested)
                {
                    state->followUpRequested = false;
                    state->cancelled.fetch_add(1, std::memory_order_relaxed);
                }

                idleCondition.notify_all();
            }

            // Releasing a user's last target reference may run arbitrary target
            // destruction. Keep that outside the scheduler mutex.
            target.reset();
        }
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard lock(mutex);
            if (stopping)
                return;

            stopping = true;
            schedulerStopFlag->store(true, std::memory_order_release);

            for (auto& weakState : clients)
            {
                if (const auto state = weakState.lock())
                {
                    state->accepting = false;
                    cancelQueued(*state);

                    if (state->runningStopFlag)
                        state->runningStopFlag->store(true, std::memory_order_release);
                }
            }

            readyQueue.clear();
        }

        readyCondition.notify_all();
        idleCondition.notify_all();

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        workers.clear();
    }

    const std::size_t configuredWorkerCount;
    const std::shared_ptr<std::atomic<bool>> schedulerStopFlag;
    std::mutex mutex;
    std::condition_variable readyCondition;
    std::condition_variable idleCondition;
    std::deque<std::weak_ptr<Client::State>> readyQueue;
    std::vector<std::weak_ptr<Client::State>> clients;
    std::vector<std::thread> workers;
    bool stopping { false };
};

namespace
{

struct ServiceRegistry final
{
    std::mutex mutex;
    std::weak_ptr<SharedAnalysisScheduler> service;
};

ServiceRegistry& serviceRegistry()
{
    static ServiceRegistry registry;
    return registry;
}

} // namespace

SharedAnalysisScheduler::Client::Client(
    std::weak_ptr<SharedAnalysisScheduler> scheduler,
    std::shared_ptr<State> state) noexcept
    : scheduler_(std::move(scheduler)), state_(std::move(state))
{
}

SharedAnalysisScheduler::Client::~Client()
{
    static_cast<void>(cancelAndWait());
}

bool SharedAnalysisScheduler::Client::request()
{
    if (const auto scheduler = scheduler_.lock())
        return scheduler->request(state_);

    return false;
}

SharedAnalysisScheduler::Generation
SharedAnalysisScheduler::Client::cancelAndAdvanceGeneration()
{
    if (const auto scheduler = scheduler_.lock())
        return scheduler->cancelAndAdvanceGeneration(state_);

    return state_->currentGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool SharedAnalysisScheduler::Client::waitUntilIdle()
{
    if (const auto scheduler = scheduler_.lock())
        return scheduler->waitUntilIdle(state_);

    return true;
}

bool SharedAnalysisScheduler::Client::cancelAndWait()
{
    if (const auto scheduler = scheduler_.lock())
        return scheduler->cancelAndWait(state_);

    return true;
}

SharedAnalysisScheduler::Generation
SharedAnalysisScheduler::Client::generation() const noexcept
{
    return state_->currentGeneration.load(std::memory_order_acquire);
}

SharedAnalysisScheduler::Counters
SharedAnalysisScheduler::Client::counters() const noexcept
{
    return {
        state_->submitted.load(std::memory_order_relaxed),
        state_->executed.load(std::memory_order_relaxed),
        state_->cancelled.load(std::memory_order_relaxed),
    };
}

SharedAnalysisScheduler::Ptr
SharedAnalysisScheduler::acquire(const std::size_t workerCount)
{
    if (workerCount == 0)
        throw std::invalid_argument("SharedAnalysisScheduler needs at least one worker");

    auto& registry = serviceRegistry();
    std::lock_guard lock(registry.mutex);

    if (auto existing = registry.service.lock())
        return existing;

    auto service = Ptr(new SharedAnalysisScheduler(workerCount));
    registry.service = service;
    return service;
}

SharedAnalysisScheduler::SharedAnalysisScheduler(const std::size_t workerCount)
    : impl_(std::make_unique<Impl>(workerCount))
{
}

SharedAnalysisScheduler::~SharedAnalysisScheduler() = default;

std::shared_ptr<SharedAnalysisScheduler::Client>
SharedAnalysisScheduler::createClient(std::weak_ptr<JobClient> target)
{
    if (target.expired())
        throw std::invalid_argument("analysis job target must still be alive");

    auto state = std::make_shared<Client::State>(std::move(target));

    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping)
            throw std::runtime_error("analysis scheduler is stopping");

        std::erase_if(impl_->clients, [](const auto& client) {
            return client.expired();
        });
        impl_->clients.emplace_back(state);
    }

    return std::shared_ptr<Client>(
        new Client(weak_from_this(), std::move(state)));
}

std::size_t SharedAnalysisScheduler::workerCount() const noexcept
{
    return impl_->configuredWorkerCount;
}

bool SharedAnalysisScheduler::request(const std::shared_ptr<Client::State>& state)
{
    std::lock_guard lock(impl_->mutex);

    if (impl_->stopping || ! state->accepting || state->target.expired())
    {
        if (state->target.expired())
            state->accepting = false;
        return false;
    }

    const auto requestGeneration =
        state->currentGeneration.load(std::memory_order_acquire);
    state->submitted.fetch_add(1, std::memory_order_relaxed);

    if (state->running)
    {
        if (state->followUpRequested)
            state->cancelled.fetch_add(1, std::memory_order_relaxed);

        state->followUpRequested = true;
        state->followUpGeneration = requestGeneration;
        return true;
    }

    if (state->queued)
    {
        state->cancelled.fetch_add(1, std::memory_order_relaxed);
        state->queuedGeneration = requestGeneration;
        return true;
    }

    state->queued = true;
    state->queuedGeneration = requestGeneration;
    impl_->readyQueue.emplace_back(state);
    impl_->readyCondition.notify_one();
    return true;
}

SharedAnalysisScheduler::Generation
SharedAnalysisScheduler::cancelAndAdvanceGeneration(
    const std::shared_ptr<Client::State>& state)
{
    std::lock_guard lock(impl_->mutex);

    const auto newGeneration =
        state->currentGeneration.load(std::memory_order_relaxed) + 1;
    state->currentGeneration.store(newGeneration, std::memory_order_release);

    Impl::cancelQueued(*state);
    impl_->eraseQueuedEntriesFor(state);

    if (state->runningStopFlag)
        state->runningStopFlag->store(true, std::memory_order_release);

    impl_->idleCondition.notify_all();
    return newGeneration;
}

bool SharedAnalysisScheduler::waitUntilIdle(
    const std::shared_ptr<Client::State>& state)
{
    std::unique_lock lock(impl_->mutex);

    if (state->running && state->runningThread == std::this_thread::get_id())
        return false;

    impl_->idleCondition.wait(lock, [&state] {
        return Impl::isIdle(*state);
    });
    return true;
}

bool SharedAnalysisScheduler::cancelAndWait(
    const std::shared_ptr<Client::State>& state)
{
    std::unique_lock lock(impl_->mutex);
    state->accepting = false;
    Impl::cancelQueued(*state);
    impl_->eraseQueuedEntriesFor(state);

    if (state->runningStopFlag)
        state->runningStopFlag->store(true, std::memory_order_release);

    impl_->readyCondition.notify_all();
    impl_->idleCondition.notify_all();

    if (state->running && state->runningThread == std::this_thread::get_id())
        return false;

    impl_->idleCondition.wait(lock, [&state] {
        return Impl::isIdle(*state);
    });
    return true;
}

} // namespace audio_insight

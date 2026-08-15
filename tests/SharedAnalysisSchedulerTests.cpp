// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/SharedAnalysisScheduler.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace audio_insight::tests {
namespace {

using namespace std::chrono_literals;
using Scheduler = SharedAnalysisScheduler;

template <typename Predicate>
bool waitFor(Predicate&& predicate, const std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        std::this_thread::sleep_for(1ms);
    }

    return true;
}

class BlockingFirstJob final : public Scheduler::JobClient {
public:
    explicit BlockingFirstJob(std::string labelToUse = { },
        std::shared_ptr<std::vector<std::string>> orderToUse = { },
        std::shared_ptr<std::mutex> orderMutexToUse = { })
        : label(std::move(labelToUse)), order(std::move(orderToUse)),
          orderMutex(std::move(orderMutexToUse))
    {
    }

    void execute(const Scheduler::JobContext& context) override
    {
        const auto invocation = calls.fetch_add(1, std::memory_order_relaxed) + 1;

        {
            std::lock_guard lock(mutex);
            generations.push_back(context.generation());
            if (invocation == 1)
                firstStarted = true;
        }

        if (order && orderMutex) {
            std::lock_guard lock(*orderMutex);
            order->push_back(label);
        }

        condition.notify_all();

        if (invocation == 1) {
            std::unique_lock lock(mutex);
            condition.wait(lock, [this] { return releaseFirst; });
        }

        condition.notify_all();
    }

    [[nodiscard]] bool waitForFirstStart()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return firstStarted; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex);
            releaseFirst = true;
        }
        condition.notify_all();
    }

    [[nodiscard]] std::uint64_t callCount() const noexcept
    {
        return calls.load(std::memory_order_relaxed);
    }

private:
    const std::string label;
    const std::shared_ptr<std::vector<std::string>> order;
    const std::shared_ptr<std::mutex> orderMutex;
    std::atomic<std::uint64_t> calls { 0 };
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<Scheduler::Generation> generations;
    bool firstStarted { false };
    bool releaseFirst { false };
};

class RecordingJob final : public Scheduler::JobClient {
public:
    RecordingJob(std::string labelToUse, std::shared_ptr<std::vector<std::string>> orderToUse,
        std::shared_ptr<std::mutex> orderMutexToUse)
        : label(std::move(labelToUse)), order(std::move(orderToUse)),
          orderMutex(std::move(orderMutexToUse))
    {
    }

    void execute(const Scheduler::JobContext&) override
    {
        {
            std::lock_guard lock(*orderMutex);
            order->push_back(label);
        }
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t callCount() const noexcept
    {
        return calls.load(std::memory_order_relaxed);
    }

private:
    const std::string label;
    const std::shared_ptr<std::vector<std::string>> order;
    const std::shared_ptr<std::mutex> orderMutex;
    std::atomic<std::uint64_t> calls { 0 };
};

class CooperativeGenerationJob final : public Scheduler::JobClient {
public:
    void execute(const Scheduler::JobContext& context) override
    {
        const auto invocation = calls.fetch_add(1, std::memory_order_relaxed) + 1;

        {
            std::lock_guard lock(mutex);
            generations.push_back(context.generation());
            condition.notify_all();
        }

        if (invocation != 1)
            return;

        while (!context.stopRequested()) {
            std::unique_lock lock(mutex);
            condition.wait_for(lock, 1ms);
        }

        stopObserved.store(true, std::memory_order_release);
        condition.notify_all();
    }

    [[nodiscard]] bool waitForCalls(const std::uint64_t expected)
    {
        return waitFor(
            [this, expected] { return calls.load(std::memory_order_relaxed) >= expected; });
    }

    [[nodiscard]] std::vector<Scheduler::Generation> recordedGenerations() const
    {
        std::lock_guard lock(mutex);
        return generations;
    }

    [[nodiscard]] bool observedStop() const noexcept
    {
        return stopObserved.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> calls { 0 };
    std::atomic<bool> stopObserved { false };
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<Scheduler::Generation> generations;
};

class CountingJob final : public Scheduler::JobClient {
public:
    void execute(const Scheduler::JobContext&) override
    {
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t callCount() const noexcept
    {
        return calls.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> calls { 0 };
};

class ShutdownJob final : public Scheduler::JobClient {
public:
    void execute(const Scheduler::JobContext& context) override
    {
        started.store(true, std::memory_order_release);

        while (!context.stopRequested())
            std::this_thread::sleep_for(1ms);

        stopped.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool hasStarted() const noexcept
    {
        return started.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool hasStopped() const noexcept
    {
        return stopped.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> started { false };
    std::atomic<bool> stopped { false };
};

class SharedAnalysisSchedulerTests final : public juce::UnitTest {
public:
    SharedAnalysisSchedulerTests() : juce::UnitTest("Shared analysis scheduler", "audio-insight")
    {
    }

    void runTest() override
    {
        testModuleServiceAcquisition();
        testCoalescing();
        testFairFifoRequeue();
        testGenerationCancellationAndDestruction();
        testExpiredTargetIsNeverCalled();
        testCleanLastOwnerShutdown();
    }

private:
    void testModuleServiceAcquisition()
    {
        beginTest("Module acquisitions share the two-worker default service");

        auto first = Scheduler::acquire();
        auto second = Scheduler::acquire(7);

        expect(first == second);
        expect(first->workerCount() == 2);

        second.reset();
        first.reset();
    }

    void testCoalescing()
    {
        beginTest("Coalesces repeated work while a client is running");

        auto scheduler = Scheduler::acquire(1);
        auto job = std::make_shared<BlockingFirstJob>();
        auto client = scheduler->createClient(job);

        expect(client->request());
        expect(job->waitForFirstStart(), "The first job did not start");

        constexpr std::uint64_t repeatedRequests = 100;
        for (std::uint64_t index = 0; index < repeatedRequests; ++index)
            expect(client->request());

        job->release();
        expect(waitFor([&job] { return job->callCount() == 2; }),
            "The coalesced follow-up did not execute");
        expect(client->waitUntilIdle());

        const auto counters = client->counters();
        expect(counters.submitted == repeatedRequests + 1);
        expect(counters.executed == 2);
        expect(counters.cancelled == repeatedRequests - 1);

        expect(client->cancelAndWait());
        client.reset();
        job.reset();
        scheduler.reset();
    }

    void testFairFifoRequeue()
    {
        beginTest("A hot client requeues behind an already-waiting peer");

        auto scheduler = Scheduler::acquire(1);
        auto order = std::make_shared<std::vector<std::string>>();
        auto orderMutex = std::make_shared<std::mutex>();
        auto hotJob = std::make_shared<BlockingFirstJob>("hot", order, orderMutex);
        auto peerJob = std::make_shared<RecordingJob>("peer", order, orderMutex);
        auto hotClient = scheduler->createClient(hotJob);
        auto peerClient = scheduler->createClient(peerJob);

        expect(hotClient->request());
        expect(hotJob->waitForFirstStart(), "The hot client's first job did not start");
        expect(peerClient->request());
        expect(hotClient->request());
        hotJob->release();

        expect(waitFor([&hotJob, &peerJob] {
            return hotJob->callCount() == 2 && peerJob->callCount() == 1;
        }),
            "Both clients did not receive worker time");
        expect(hotClient->waitUntilIdle());
        expect(peerClient->waitUntilIdle());

        std::vector<std::string> observedOrder;
        {
            std::lock_guard lock(*orderMutex);
            observedOrder = *order;
        }

        const std::vector<std::string> expectedOrder { "hot", "peer", "hot" };
        expect(observedOrder == expectedOrder,
            "A follow-up must join the FIFO tail instead of starving peers");

        expect(hotClient->cancelAndWait());
        expect(peerClient->cancelAndWait());
        hotClient.reset();
        peerClient.reset();
        hotJob.reset();
        peerJob.reset();
        scheduler.reset();
    }

    void testGenerationCancellationAndDestruction()
    {
        beginTest("Generation changes stop stale work and client destruction waits");

        auto scheduler = Scheduler::acquire(1);
        auto generationJob = std::make_shared<CooperativeGenerationJob>();
        auto generationClient = scheduler->createClient(generationJob);

        expect(generationClient->request());
        expect(generationJob->waitForCalls(1), "The stale-generation job did not start");
        expect(generationClient->cancelAndAdvanceGeneration() == 2);
        expect(generationClient->waitUntilIdle());
        expect(generationJob->observedStop(),
            "The running job did not observe generation cancellation");

        expect(generationClient->request());
        expect(generationJob->waitForCalls(2), "The new-generation job did not execute");
        expect(generationClient->waitUntilIdle());

        const auto generations = generationJob->recordedGenerations();
        expect(generations == std::vector<Scheduler::Generation> { 1, 2 });

        expect(generationClient->cancelAndWait());
        generationClient.reset();
        generationJob.reset();

        auto blockingJob = std::make_shared<BlockingFirstJob>();
        auto blockingClient = scheduler->createClient(blockingJob);
        expect(blockingClient->request());
        expect(blockingJob->waitForFirstStart(), "The destruction test job did not start");

        std::atomic<bool> destructionFinished { false };
        std::thread destroyer(
            [ownedClient = std::move(blockingClient), &destructionFinished]() mutable {
                ownedClient.reset();
                destructionFinished.store(true, std::memory_order_release);
            });

        std::this_thread::sleep_for(20ms);
        expect(!destructionFinished.load(std::memory_order_acquire),
            "Client destruction returned while its job still used client state");

        blockingJob->release();
        destroyer.join();
        expect(destructionFinished.load(std::memory_order_acquire));

        blockingJob.reset();
        scheduler.reset();
    }

    void testExpiredTargetIsNeverCalled()
    {
        beginTest("A destroyed weak target is cancelled without invocation");

        auto scheduler = Scheduler::acquire(1);
        auto blocker = std::make_shared<BlockingFirstJob>();
        auto blockerClient = scheduler->createClient(blocker);
        expect(blockerClient->request());
        expect(blocker->waitForFirstStart(), "The queue blocker did not start");

        auto expiredTarget = std::make_shared<CountingJob>();
        std::weak_ptr<CountingJob> expiredTargetObserver = expiredTarget;
        auto expiredClient = scheduler->createClient(expiredTarget);
        expect(expiredClient->request());
        expiredTarget.reset();
        expect(expiredTargetObserver.expired());

        blocker->release();
        expect(blockerClient->waitUntilIdle());
        expect(expiredClient->waitUntilIdle());

        const auto counters = expiredClient->counters();
        expect(counters.submitted == 1);
        expect(counters.executed == 0);
        expect(counters.cancelled == 1);

        expect(blockerClient->cancelAndWait());
        expect(expiredClient->cancelAndWait());
        blockerClient.reset();
        expiredClient.reset();
        blocker.reset();
        scheduler.reset();
    }

    void testCleanLastOwnerShutdown()
    {
        beginTest("The final service owner cancels queued work and joins workers");

        auto scheduler = Scheduler::acquire(1);
        auto runningJob = std::make_shared<ShutdownJob>();
        auto queuedJob = std::make_shared<CountingJob>();
        auto runningClient = scheduler->createClient(runningJob);
        auto queuedClient = scheduler->createClient(queuedJob);

        expect(runningClient->request());
        expect(waitFor([&runningJob] { return runningJob->hasStarted(); }),
            "The shutdown test job did not start");
        expect(queuedClient->request());

        scheduler.reset();

        expect(runningJob->hasStopped(),
            "Service destruction returned before the running job stopped");
        expect(queuedJob->callCount() == 0, "Queued work ran during final-owner shutdown");
        expect(queuedClient->counters().cancelled == 1);
        expect(!runningClient->request());
        expect(!queuedClient->request());

        runningClient.reset();
        queuedClient.reset();
        runningJob.reset();
        queuedJob.reset();
    }
};

SharedAnalysisSchedulerTests sharedAnalysisSchedulerTests;

} // namespace
} // namespace audio_insight::tests

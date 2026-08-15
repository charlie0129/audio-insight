// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace audio_insight {

/**
 * A process-local, non-real-time worker pool shared by plugin instances.
 *
 * Plugin instances acquire the reference-counted module service with acquire()
 * and must retain that shared_ptr for as long as any of their clients can make
 * requests. The final service owner synchronously cancels outstanding work and
 * joins every worker in the destructor.
 *
 * None of this class's API is intended for processBlock() or any other
 * real-time thread. request(), cancellation, registration, and destruction may
 * all take locks, notify condition variables, or wait for worker threads.
 */
class SharedAnalysisScheduler final : public std::enable_shared_from_this<SharedAnalysisScheduler> {
private:
    struct Impl;

public:
    using Generation = std::uint64_t;

    class JobContext final {
    public:
        [[nodiscard]] Generation generation() const noexcept
        {
            return generation_;
        }

        [[nodiscard]] bool stopRequested() const noexcept
        {
            return schedulerStop_->load(std::memory_order_acquire)
                || generationStop_->load(std::memory_order_acquire);
        }

    private:
        friend struct SharedAnalysisScheduler::Impl;

        JobContext(Generation generation, std::shared_ptr<const std::atomic<bool>> schedulerStop,
            std::shared_ptr<const std::atomic<bool>> generationStop) noexcept
            : generation_(generation), schedulerStop_(std::move(schedulerStop)),
              generationStop_(std::move(generationStop))
        {
        }

        Generation generation_ { 0 };
        std::shared_ptr<const std::atomic<bool>> schedulerStop_;
        std::shared_ptr<const std::atomic<bool>> generationStop_;
    };

    /**
     * Lifetime-safe target for analysis work.
     *
     * The scheduler stores this target as a weak_ptr and obtains a temporary
     * shared_ptr only while execute() is running. Implementations should keep
     * all job state inside this object (or in other reference-counted state),
     * must not capture raw processor/editor pointers, and should check
     * context.stopRequested() at sensible interruption points.
     */
    class JobClient {
    public:
        virtual ~JobClient() = default;
        virtual void execute(const JobContext& context) = 0;
    };

    struct Counters final {
        // Accepted request() calls, including requests later coalesced.
        std::uint64_t submitted { 0 };

        // JobClient::execute() invocations. A running invocation remains
        // executed even if it later observes cooperative cancellation.
        std::uint64_t executed { 0 };

        // Accepted submissions discarded before execute(), including replaced
        // coalesced work, queued cancellation, and expired weak targets.
        std::uint64_t cancelled { 0 };

        // Successful monotonic queue-wait measurements. Queue wait begins at
        // the latest retained latest-wins request and ends when its worker
        // starts the corresponding execute() invocation.
        std::uint64_t queueWaitSamples { 0 };
        std::uint64_t lastQueueWaitNanoseconds { 0 };
        std::uint64_t maximumQueueWaitNanoseconds { 0 };

        // Queue-wait samples strictly greater than their optional relative
        // deadline budget. Requests without a deadline never increment this.
        std::uint64_t queueWaitDeadlineMisses { 0 };

        // Successful monotonic request-to-completion measurements. The start
        // is the same latest retained request used by queue-wait telemetry.
        std::uint64_t jobTurnaroundSamples { 0 };
        std::uint64_t lastJobTurnaroundNanoseconds { 0 };
        std::uint64_t maximumJobTurnaroundNanoseconds { 0 };

        // Turnaround samples strictly greater than their optional relative
        // deadline budget. Requests without a deadline never increment this.
        std::uint64_t jobDeadlineMisses { 0 };

        // Queue-wait or turnaround measurements rejected because the clock was
        // not steady, a timestamp was unavailable, or time moved backwards.
        // One job can contribute up to two unavailable measurements.
        std::uint64_t timingUnavailable { 0 };
    };

    class Client final {
    public:
        ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        Client(Client&&) = delete;
        Client& operator=(Client&&) = delete;

        /**
         * Requests analysis for the current generation.
         *
         * If this client already has queued work, that work is replaced in
         * place. If work is running, at most one follow-up request is retained.
         * Replaced requests contribute to the cancelled counter. The optional
         * relative deadline is diagnostic only; it never cancels or delays
         * work. Non-positive budgets mean an immediate deadline. Returns false
         * when the client, target, or scheduler is no longer available.
         *
         * This is a non-real-time operation and may take a mutex. Never call it
         * from processBlock(). A message-thread/coordinator timer should observe
         * the audio handoff and call request() instead.
         */
        [[nodiscard]] bool request(
            std::optional<std::chrono::nanoseconds> relativeDeadline = std::nullopt);

        /**
         * Invalidates queued work and cooperatively stops running work, then
         * returns the new generation. The client remains reusable.
         *
         * This call does not wait for a running job. Follow it with
         * waitUntilIdle() when the caller must release generation-owned state.
         * It is strictly non-real-time and may take a mutex.
         */
        [[nodiscard]] Generation cancelAndAdvanceGeneration();

        /**
         * Waits until this client has no queued, pending, or running work.
         * Returns false rather than deadlocking if called by this client's own
         * execute() invocation.
         * This is a strictly non-real-time, potentially blocking operation.
         */
        [[nodiscard]] bool waitUntilIdle();

        /**
         * Permanently rejects new work, cancels outstanding work, and waits for
         * a running invocation to return. Returns false only when invoked from
         * this client's own execute() call, where waiting would deadlock.
         * This is a strictly non-real-time, potentially blocking operation.
         */
        [[nodiscard]] bool cancelAndWait();

        [[nodiscard]] Generation generation() const noexcept;
        [[nodiscard]] Counters counters() const noexcept;

    private:
        friend class SharedAnalysisScheduler;
        struct State;

        Client(std::weak_ptr<SharedAnalysisScheduler> scheduler,
            std::shared_ptr<State> state) noexcept;

        std::weak_ptr<SharedAnalysisScheduler> scheduler_;
        std::shared_ptr<State> state_;
    };

    using Ptr = std::shared_ptr<SharedAnalysisScheduler>;

    /**
     * Acquires this plugin module's shared service. The first acquisition
     * starts workerCount workers; later acquisitions reuse that service and its
     * existing worker count. The product default is two workers.
     */
    [[nodiscard]] static Ptr acquire(std::size_t workerCount = 2);

    ~SharedAnalysisScheduler();

    SharedAnalysisScheduler(const SharedAnalysisScheduler&) = delete;
    SharedAnalysisScheduler& operator=(const SharedAnalysisScheduler&) = delete;
    SharedAnalysisScheduler(SharedAnalysisScheduler&&) = delete;
    SharedAnalysisScheduler& operator=(SharedAnalysisScheduler&&) = delete;

    /**
     * Registers a weak job target. The returned client does not own the module
     * service; the plugin instance must retain the Ptr returned by acquire().
     */
    [[nodiscard]] std::shared_ptr<Client> createClient(std::weak_ptr<JobClient> target);

    [[nodiscard]] std::size_t workerCount() const noexcept;

private:
    explicit SharedAnalysisScheduler(std::size_t workerCount);

    [[nodiscard]] bool request(const std::shared_ptr<Client::State>& state,
        std::optional<std::chrono::nanoseconds> relativeDeadline);
    [[nodiscard]] Generation cancelAndAdvanceGeneration(
        const std::shared_ptr<Client::State>& state);
    [[nodiscard]] bool waitUntilIdle(const std::shared_ptr<Client::State>& state);
    [[nodiscard]] bool cancelAndWait(const std::shared_ptr<Client::State>& state);

    std::unique_ptr<Impl> impl_;
};

} // namespace audio_insight

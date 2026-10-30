//
// RxPullHook.hpp
// ASFWDriver
//
// The BeginRead pull. CoreAudio's BeginRead runs on AudioDriverKit's IO
// thread, which is real-time. Received packets reach the capture ring from
// the controller's interrupt handler, which DriverKit runs at fixed priority
// 63, below every real-time thread: a busy real-time load (Ableton Live with
// many tracks) holds it off for milliseconds (b178: 22 ms), and the read
// finds frames missing. So a read that finds the capture ring short drains
// the receive ring itself, on the IO thread, through this hook.
//
// The master stream's receive consumer publishes its receive context here
// while it is bound to this control block and withdraws it when the context
// quiesces; Withdraw returns only once no pull is inside the context.
//

#pragma once

#include "../../../Isoch/Core/IsochReceivePollSource.hpp"

#include <atomic>
#include <cstdint>

#include <DriverKit/IOLib.h>

namespace ASFW::Audio::Runtime {

struct RxPullHook final {
    std::atomic<::ASFW::Isoch::IIsochReceivePollSource*> source{nullptr};
    std::atomic<uint32_t> inFlight{0};
    // Since the hook was built: reads that pulled, the packets they drained,
    // pulls that gave up waiting for a poll already in progress, and pulls
    // whose own drain stopped at the deadline with the read not covered.
    std::atomic<uint64_t> pulls{0};
    std::atomic<uint64_t> pulledPackets{0};
    std::atomic<uint64_t> busy{0};
    std::atomic<uint64_t> cut{0};

    void Publish(::ASFW::Isoch::IIsochReceivePollSource* pollSource) noexcept {
        source.store(pollSource, std::memory_order_seq_cst);
    }

    // Stops new pulls into `pollSource`, without waiting for one in progress:
    // for a consumer that moves to another control block, whose context stays.
    void Clear(::ASFW::Isoch::IIsochReceivePollSource* pollSource) noexcept {
        auto* expected = pollSource;
        (void)source.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
    }

    // Clear, then wait out a pull in progress. A pull is short: it drains a
    // few packets, or waits at most its budget for a poll already in progress.
    void Withdraw(::ASFW::Isoch::IIsochReceivePollSource* pollSource) noexcept {
        Clear(pollSource);
        while (inFlight.load(std::memory_order_seq_cst) != 0) {
            IODelay(10);
        }
    }

    // On the IO thread: drain until `limit` says the read is covered, or its
    // deadline. When another poll holds the context's gate (the interrupt is
    // draining the same ring), spin until that poll has covered the read or
    // the deadline passes, rather than read a short ring. Without a deadline
    // the pull tries once.
    uint32_t Pull(const ::ASFW::Isoch::IsochReceiveDrainLimit& limit) noexcept {
        inFlight.fetch_add(1, std::memory_order_seq_cst);
        uint32_t drained = 0;
        if (auto* pollSource = source.load(std::memory_order_seq_cst)) {
            bool polled = pollSource->TryPoll(drained, limit);
            while (!polled && limit.deadlineTicks != 0 && !limit.Reached(mach_absolute_time())) {
                CpuRelax();
                polled = pollSource->TryPoll(drained, limit);
            }
            pulls.fetch_add(1, std::memory_order_relaxed);
            if (polled) {
                pulledPackets.fetch_add(drained, std::memory_order_relaxed);
                if (limit.deadlineTicks != 0 && !limit.Enough() &&
                    mach_absolute_time() >= limit.deadlineTicks) {
                    cut.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (!limit.Enough()) {
                busy.fetch_add(1, std::memory_order_relaxed);
            }
        }
        inFlight.fetch_sub(1, std::memory_order_seq_cst);
        return drained;
    }

  private:
    static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __builtin_arm_yield();
#endif
    }
};

} // namespace ASFW::Audio::Runtime

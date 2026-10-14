// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// PublicationGate.hpp
// Combined atomic admission gate synchronizing device/nub publication with
// driver teardown. Admission check and in-flight tracking are performed
// in a single atomic CAS so teardown and publication cannot race.
//
// The counters live in a shared block each admission co-owns, so an
// admission that outlives a timed-out CloseAndWait (and the gate's owner)
// can still exit safely and learn that its owner is gone.

#pragma once

#include <DriverKit/IOLib.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace ASFW::Audio {

class PublicationGate {
public:
    static constexpr int32_t kClosedSentinel = 1'000'000;
    /// Default bound on CloseAndWait.
    static constexpr uint32_t kCloseTimeoutMs = 2000;

private:
    struct Shared {
        std::atomic<int32_t> state{0};
        std::atomic<uint64_t> rejectCount{0};
        std::atomic<bool> abandoned{false};
    };

public:

    /// RAII guard representing an active publication admission.
    class AdmissionScope {
    public:
        AdmissionScope() noexcept = default;

        explicit AdmissionScope(PublicationGate& gate) noexcept
            : shared_(gate.shared_) {
            admitted_ = TryEnter(*shared_);
        }

        ~AdmissionScope() noexcept {
            ExitIfAdmitted();
        }

        AdmissionScope(const AdmissionScope&) = delete;
        AdmissionScope& operator=(const AdmissionScope&) = delete;

        AdmissionScope(AdmissionScope&& other) noexcept
            : shared_(std::move(other.shared_)), admitted_(other.admitted_) {
            other.admitted_ = false;
        }

        AdmissionScope& operator=(AdmissionScope&& other) noexcept {
            if (this != &other) {
                ExitIfAdmitted();
                shared_ = std::move(other.shared_);
                admitted_ = other.admitted_;
                other.admitted_ = false;
            }
            return *this;
        }

        [[nodiscard]] bool IsAdmitted() const noexcept { return admitted_; }

        /// Returns true if teardown closed the gate while this admission was in flight.
        [[nodiscard]] bool IsStopping() const noexcept {
            return shared_ && shared_->state.load(std::memory_order_acquire) < 0;
        }

        /// Returns true if CloseAndWait gave up on in-flight admissions. The
        /// gate's owner may be gone; touch nothing but this scope.
        [[nodiscard]] bool IsAbandoned() const noexcept {
            return shared_ && shared_->abandoned.load(std::memory_order_acquire);
        }

        /// Aborts the admission if stopping, records the rejection, and immediately
        /// exits the gate to unblock teardown.
        [[nodiscard]] bool AbortIfStopping() noexcept {
            if (IsStopping()) {
                if (admitted_) {
                    admitted_ = false;
                    shared_->rejectCount.fetch_add(1, std::memory_order_relaxed);
                    shared_->state.fetch_sub(1, std::memory_order_acq_rel);
                }
                return true;
            }
            return false;
        }

    private:
        void ExitIfAdmitted() noexcept {
            if (admitted_ && shared_) {
                admitted_ = false;
                shared_->state.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        std::shared_ptr<Shared> shared_{};
        bool admitted_{false};
    };

    PublicationGate() noexcept = default;

    /// Atomically admits one publication if gate is open (state >= 0).
    [[nodiscard]] bool TryEnter() noexcept { return TryEnter(*shared_); }

    /// Exits one active admission.
    void Exit() noexcept {
        shared_->state.fetch_sub(1, std::memory_order_acq_rel);
    }

    /// Returns true if teardown has closed admission.
    [[nodiscard]] bool IsClosed() const noexcept {
        return shared_->state.load(std::memory_order_acquire) < 0;
    }

    void RecordReject() noexcept {
        shared_->rejectCount.fetch_add(1, std::memory_order_relaxed);
    }

    /// Atomically closes admission so no future TryEnter can succeed,
    /// and waits up to `timeoutMs` for all in-flight admissions to exit.
    /// Returns false if some were still in flight; those are then marked
    /// abandoned (AdmissionScope::IsAbandoned) and exit on their own.
    [[nodiscard]] bool CloseAndWait(uint32_t timeoutMs = kCloseTimeoutMs) noexcept {
        auto& state = shared_->state;
        int32_t cur = state.load(std::memory_order_relaxed);
        while (cur >= 0) {
            if (state.compare_exchange_weak(cur, cur - kClosedSentinel,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                break;
            }
        }

#ifdef ASFW_HOST_TEST
        if (onGateClosedForTesting_) {
            onGateClosedForTesting_();
        }
#endif

        for (uint32_t waitedMs = 0; state.load(std::memory_order_acquire) > -kClosedSentinel;
             ++waitedMs) {
            if (waitedMs >= timeoutMs) {
                shared_->abandoned.store(true, std::memory_order_release);
                return false;
            }
            IOSleep(1);
        }
        return true;
    }

    [[nodiscard]] uint64_t RejectCount() const noexcept {
        return shared_->rejectCount.load(std::memory_order_relaxed);
    }

#ifdef ASFW_HOST_TEST
    void SetOnGateClosedForTesting(std::function<void()> hook) noexcept {
        onGateClosedForTesting_ = std::move(hook);
    }
#endif

private:
    [[nodiscard]] static bool TryEnter(Shared& shared) noexcept {
        int32_t cur = shared.state.load(std::memory_order_relaxed);
        while (cur >= 0) {
            if (shared.state.compare_exchange_weak(cur, cur + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                return true;
            }
        }
        shared.rejectCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::shared_ptr<Shared> shared_{std::make_shared<Shared>()};
#ifdef ASFW_HOST_TEST
    std::function<void()> onGateClosedForTesting_{};
#endif
};

} // namespace ASFW::Audio

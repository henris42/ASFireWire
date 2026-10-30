#pragma once

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

#include "HardwareAccessScope.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Driver {

class HardwareInterface;

/// Serializes local OHCI access with provider revocation.
/// See documentation:
///   - ASFWDriver/Service/Lifecycle/RUNTIME_LIFECYCLE_CONTRACT.md
///   - documentation/TOKEN_BASED_LIFECYCLE.md
class HardwareAccessGate {
  public:
    HardwareAccessGate() : lock_(IOLockAlloc()) {}
    ~HardwareAccessGate() {
        if (lock_) {
            IOLockFree(lock_);
        }
    }

    HardwareAccessGate(const HardwareAccessGate&) = delete;
    HardwareAccessGate& operator=(const HardwareAccessGate&) = delete;

    [[nodiscard]] HardwareAccessScope TryBeginAccess(HardwareInterface& hardware) noexcept {
        if (!lock_) {
            return {};
        }
        IOLockLock(lock_);
        if (!open_) {
            IOLockUnlock(lock_);
            return {};
        }
        return HardwareAccessScope(&hardware, lock_);
    }

    /// One register read without the lock, for a thread that must not wait
    /// for it (the BeginRead pull reads the cycle timer on AudioDriverKit's
    /// real-time IO thread). Admitted while the gate is open, counted instead
    /// of locked, and drained by RevokeAndDrain like a scope. `read` performs
    /// one MMIO read and nothing that waits. False when the gate is closed.
    template <typename Read>
    [[nodiscard]] bool ReadWithoutLock(Read&& read) noexcept {
        lockFreeReaders_.fetch_add(1, std::memory_order_seq_cst);
        const bool admitted = lockFreeOpen_.load(std::memory_order_seq_cst);
        if (admitted) {
            read();
        }
        lockFreeReaders_.fetch_sub(1, std::memory_order_release);
        return admitted;
    }

    /// Prevent new MMIO scopes and wait for the active batch, if any, to end.
    void RevokeAndDrain() noexcept {
        if (!lock_) {
            return;
        }
        IOLockLock(lock_);
        open_ = false;
        lockFreeOpen_.store(false, std::memory_order_seq_cst);
        IOLockUnlock(lock_);
        while (lockFreeReaders_.load(std::memory_order_seq_cst) != 0) {
            IODelay(1);
        }
    }

    /// Closes the gate while an already-admitted HardwareAccessScope holds
    /// `lock_`. This must not acquire the lock again: a 0xffffffff read is
    /// detected from inside that scope, and recursive locking would deadlock
    /// the only thread that can drain it.
    void RevokeFromAdmittedScope() noexcept {
        open_ = false;
        lockFreeOpen_.store(false, std::memory_order_seq_cst);
    }

    /// Opens access only after the new provider is fully initialized.
    void Open() noexcept {
        if (!lock_) {
            return;
        }
        IOLockLock(lock_);
        open_ = true;
        lockFreeOpen_.store(true, std::memory_order_seq_cst);
        IOLockUnlock(lock_);
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        if (!lock_) {
            return false;
        }
        IOLockLock(lock_);
        const bool open = open_;
        IOLockUnlock(lock_);
        return open;
    }

  private:
    IOLock* lock_{nullptr};
    bool open_{false};
    // ReadWithoutLock's admission: open_ mirrored for readers that do not
    // take the lock, and how many are reading now.
    std::atomic<bool> lockFreeOpen_{false};
    std::atomic<uint32_t> lockFreeReaders_{0};
};

} // namespace ASFW::Driver

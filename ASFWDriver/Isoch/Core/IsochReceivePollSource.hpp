//
// IsochReceivePollSource.hpp
// ASFWDriver
//
// A receive context as its consumer sees it: something it may ask to drain
// completed packets now, from any thread. The audio side's BeginRead pull
// uses it (Audio/DriverKit/Runtime/RxPullHook.hpp).
//

#pragma once

#include <cstdint>

namespace ASFW::Isoch {

// Where a reader's drain may stop: once `enough` says the reader has what it
// needs, or at `deadlineTicks` (mach_absolute_time units; 0: none). Checked
// after each packet, so a drain on a real-time thread does only the work its
// reader waits for and leaves the rest to the next poll.
struct IsochReceiveDrainLimit {
    bool (*enough)(const void* context) noexcept {nullptr};
    const void* context{nullptr};
    uint64_t deadlineTicks{0};

    [[nodiscard]] bool Enough() const noexcept { return enough != nullptr && enough(context); }
    [[nodiscard]] bool Reached(uint64_t nowTicks) const noexcept {
        return Enough() || (deadlineTicks != 0 && nowTicks >= deadlineTicks);
    }
};

class IIsochReceivePollSource {
  public:
    // Drains the packets the controller has completed, on the caller's
    // thread, until `limit` is reached. False when another poll holds the
    // context's gate (nothing was drained); true otherwise, with `processed`
    // the packets drained (0 for a stopped context).
    [[nodiscard]] virtual bool TryPoll(uint32_t& processed,
                                       const IsochReceiveDrainLimit& limit) noexcept = 0;

  protected:
    ~IIsochReceivePollSource() = default;
};

} // namespace ASFW::Isoch

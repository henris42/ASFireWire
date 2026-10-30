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

class IIsochReceivePollSource {
  public:
    // Drains the packets the controller has completed, on the caller's
    // thread. False when another poll holds the context's gate (nothing was
    // drained); true otherwise, with `processed` the packets drained (0 for a
    // stopped context).
    [[nodiscard]] virtual bool TryPoll(uint32_t& processed) noexcept = 0;

  protected:
    ~IIsochReceivePollSource() = default;
};

} // namespace ASFW::Isoch

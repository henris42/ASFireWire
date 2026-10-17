#pragma once

#include <functional>
#include <new> // libc++ placement-new declaration must precede DriverKit.

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#endif
#include "../Logging/Logging.hpp"

namespace ASFW::Common {

// Consumes one owned reference. Call after the dispatch source has quiesced.
// OSAction.iig: the target is retained until the ACTION is cancelled or freed;
// releasing our reference alone relies on all remote references disappearing.
// label must have static lifetime (the callers pass string literals).
//
// callbackQueue is the queue the action's callbacks run on. A callback block
// that DriverKit queued there before the cancel is not waited for by the cancel
// completion when that queue is the one the caller is running on (teardown
// inside Start() or Stop() on the Default queue): the block drains only after
// the caller returns, and would then find the action freed (DidAction on
// 0xeeee.. poison, DriverKit 21). Releasing from a block queued behind it on
// the same queue orders the release after every callback already queued.
inline void CancelAndReleaseOwnedAction(OSAction* action, const char* label, std::function<void()> done = {},
                                        IODispatchQueue* callbackQueue = nullptr) {
    if (!action) {
        if (done) done();
        return;
    }
    if (callbackQueue) {
        callbackQueue->retain();
    }
    auto releaseBehindQueuedCallbacks = ^{
        if (!callbackQueue) {
            action->release();
            if (done) done();
            return;
        }
        callbackQueue->DispatchAsync(^{
            ASFW_LOG(Controller, "[Teardown] action released behind queued callbacks owner=%{public}s action=%p", label, action);
            action->release();
            callbackQueue->release();
            if (done) done();
        });
    };
    ASFW_LOG(Controller, "[Teardown] action cancel requested owner=%{public}s action=%p", label, action);
    const auto kr = action->Cancel(^{
        ASFW_LOG(Controller, "[Teardown] action cancel completed owner=%{public}s action=%p", label, action);
        releaseBehindQueuedCallbacks();
    });
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Controller, "[Teardown] action cancel failed owner=%{public}s action=%p kr=0x%x", label, action, kr);
        releaseBehindQueuedCallbacks();
    }
}

} // namespace ASFW::Common

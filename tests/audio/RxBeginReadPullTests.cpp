// Unit tests for the BeginRead pull (Audio/DriverKit/Runtime/RxPullHook.hpp):
// the hook's pull, clear and withdraw; the receive consumer publishing its
// context while bound and taking it back at quiesce; events from a pulled
// batch waiting for the next regular poll; and the lock-free binding check
// that lets a batch run without the endpoint's lock.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/DriverKit/Runtime/RxPullHook.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Common/TimingUtils.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::DirectAudioBindingSnapshot;
using ASFW::Audio::Runtime::IDirectAudioBindingSource;
using ASFW::Audio::Runtime::RxPullHook;
using ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer;
using ASFW::Isoch::IIsochReceivePollSource;

// A receive context stand-in: busy for the first `busyPolls` tries, then
// drains `packets`; or, with `block` set, waits inside the poll until released.
class FakePollSource final : public IIsochReceivePollSource {
  public:
    bool TryPoll(uint32_t& processed) noexcept override {
        processed = 0;
        ++tries;
        if (block.load()) {
            inside.store(true);
            while (block.load()) {
                std::this_thread::yield();
            }
        }
        if (busyPolls > 0) {
            --busyPolls;
            return false;
        }
        processed = packets;
        return true;
    }

    uint32_t busyPolls{0};
    uint32_t packets{0};
    uint32_t tries{0};
    std::atomic<bool> block{false};
    std::atomic<bool> inside{false};
};

// The driver sets the timebase up at start; a test does it here.
uint64_t Ticks(uint64_t nanos) {
    (void)ASFW::Timing::initializeHostTimebase();
    return ASFW::Timing::nanosToHostTicks(nanos);
}

TEST(RxPullHookTests, APullWithoutASourceDoesNothing) {
    RxPullHook hook;
    EXPECT_EQ(hook.Pull(Ticks(100'000)), 0U);
    EXPECT_EQ(hook.pulls.load(), 0U);
    EXPECT_EQ(hook.inFlight.load(), 0U);
}

TEST(RxPullHookTests, APullDrainsThroughThePublishedSource) {
    RxPullHook hook;
    FakePollSource source;
    source.packets = 5;
    hook.Publish(&source);
    EXPECT_EQ(hook.Pull(Ticks(100'000)), 5U);
    EXPECT_EQ(hook.pulls.load(), 1U);
    EXPECT_EQ(hook.pulledPackets.load(), 5U);
    EXPECT_EQ(hook.busy.load(), 0U);
}

TEST(RxPullHookTests, APullWaitsOutAPollInProgress) {
    RxPullHook hook;
    FakePollSource source;
    source.busyPolls = 3;
    source.packets = 8;
    hook.Publish(&source);
    EXPECT_EQ(hook.Pull(Ticks(1'000'000'000)), 8U);
    EXPECT_EQ(source.tries, 4U);
    EXPECT_EQ(hook.busy.load(), 0U);
}

TEST(RxPullHookTests, APullGivesUpAfterItsBudget) {
    RxPullHook hook;
    FakePollSource source;
    source.busyPolls = UINT32_MAX;
    hook.Publish(&source);
    EXPECT_EQ(hook.Pull(0), 0U);
    EXPECT_EQ(hook.pulls.load(), 1U);
    EXPECT_EQ(hook.busy.load(), 1U);
}

TEST(RxPullHookTests, ClearTakesOutOnlyItsOwnSource) {
    RxPullHook hook;
    FakePollSource mine;
    FakePollSource other;
    hook.Publish(&mine);
    hook.Clear(&other);
    EXPECT_EQ(hook.source.load(), &mine);
    hook.Clear(&mine);
    EXPECT_EQ(hook.source.load(), nullptr);
}

TEST(RxPullHookTests, WithdrawReturnsOnlyAfterThePullInProgress) {
    RxPullHook hook;
    FakePollSource source;
    source.block.store(true);
    hook.Publish(&source);
    std::thread puller([&] { (void)hook.Pull(Ticks(100'000)); });
    while (!source.inside.load()) {
        std::this_thread::yield();
    }
    std::atomic<bool> withdrawn{false};
    std::thread withdrawer([&] {
        hook.Withdraw(&source);
        withdrawn.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(withdrawn.load()) << "Withdraw returned with a pull inside the source";
    EXPECT_EQ(hook.source.load(), nullptr) << "no new pull may start meanwhile";
    source.block.store(false);
    puller.join();
    withdrawer.join();
    EXPECT_TRUE(withdrawn.load());
    EXPECT_EQ(hook.inFlight.load(), 0U);
}

// --- The consumer publishes its context --------------------------------------

class CountingBindingSource final : public IDirectAudioBindingSource {
  public:
    explicit CountingBindingSource(DirectAudioBindingSnapshot snapshot) noexcept
        : snapshot_(snapshot) {}

    bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept override {
        ++copies;
        out = snapshot_;
        return snapshot_.valid;
    }
    uint64_t PeekDirectAudioBindingGeneration() const noexcept override {
        return peekable ? (snapshot_.valid ? snapshot_.generation : 0) : kBindingGenerationUnknown;
    }

    void Rebind(uint64_t generation, AudioTransportControlBlock* control) noexcept {
        snapshot_.generation = generation;
        snapshot_.control = control;
    }

    uint32_t copies{0};
    bool peekable{true};

  private:
    DirectAudioBindingSnapshot snapshot_{};
};

struct ConsumerRig {
    std::array<float, 64> input{};
    AudioTransportControlBlock control{};
    CountingBindingSource source;
    FakePollSource context;
    DirectAudioReceiveConsumer consumer;

    explicit ConsumerRig(bool secondary = false)
        : source({.generation = 1, .inputBase = input.data(), .inputBytes = sizeof(input),
                  .inputFrames = 32, .inputChannels = 2, .control = &control,
                  .sampleRateHz = 48000, .valid = true}),
          consumer(&source, {.am824Slots = 2, .streamChannels = 2, .isSecondary = secondary}) {}

    void Batch(bool readerPull = false) {
        consumer.BeginReceiveBatch({.drainCycleTimer = 0, .drainHostTicks = 1, .readerPull = readerPull});
    }
};

TEST(RxBeginReadPullTests, TheMasterStreamPublishesItsContextOnceBound) {
    ConsumerRig rig;
    rig.consumer.SetReceivePollSource(&rig.context);
    rig.consumer.OnReceiveActivated();
    EXPECT_EQ(rig.control.rxPull.source.load(), nullptr) << "not bound before the first batch";
    rig.Batch();
    EXPECT_EQ(rig.control.rxPull.source.load(), &rig.context);

    rig.consumer.OnReceiveQuiesced();
    EXPECT_EQ(rig.control.rxPull.source.load(), nullptr);
    rig.consumer.SetReceivePollSource(nullptr);
    EXPECT_EQ(rig.control.rxPull.source.load(), nullptr);
}

TEST(RxBeginReadPullTests, TakingTheSourceBackWithdrawsIt) {
    ConsumerRig rig;
    rig.consumer.SetReceivePollSource(&rig.context);
    rig.consumer.OnReceiveActivated();
    rig.Batch();
    ASSERT_EQ(rig.control.rxPull.source.load(), &rig.context);
    rig.consumer.SetReceivePollSource(nullptr);
    EXPECT_EQ(rig.control.rxPull.source.load(), nullptr);
}

TEST(RxBeginReadPullTests, SecondaryAndRefusedStreamsDoNotPublish) {
    ConsumerRig secondary(/*secondary=*/true);
    secondary.consumer.SetReceivePollSource(&secondary.context);
    secondary.consumer.OnReceiveActivated();
    secondary.Batch();
    EXPECT_EQ(secondary.control.rxPull.source.load(), nullptr);

    ConsumerRig refused;
    refused.consumer.AllowBeginReadPull(false);
    refused.consumer.SetReceivePollSource(&refused.context);
    refused.consumer.OnReceiveActivated();
    refused.Batch();
    EXPECT_EQ(refused.control.rxPull.source.load(), nullptr);
}

TEST(RxBeginReadPullTests, ANewControlBlockGetsTheContext) {
    ConsumerRig rig;
    rig.consumer.SetReceivePollSource(&rig.context);
    rig.consumer.OnReceiveActivated();
    rig.Batch();
    AudioTransportControlBlock moved{};
    rig.source.Rebind(2, &moved);
    rig.Batch();
    EXPECT_EQ(moved.rxPull.source.load(), &rig.context);
    rig.consumer.OnReceiveQuiesced();
    EXPECT_EQ(moved.rxPull.source.load(), nullptr);
}

// --- A batch copies the binding only when it changed ------------------------

TEST(RxBeginReadPullTests, ABatchCopiesTheBindingOnlyWhenItsGenerationChanges) {
    ConsumerRig rig;
    rig.consumer.OnReceiveActivated();
    rig.Batch();
    rig.Batch();
    rig.Batch(/*readerPull=*/true);
    EXPECT_EQ(rig.source.copies, 1U);
    rig.source.Rebind(2, &rig.control);
    rig.Batch();
    rig.Batch();
    EXPECT_EQ(rig.source.copies, 2U);

    // A source that cannot tell is copied every batch, as before.
    rig.source.peekable = false;
    rig.Batch();
    rig.Batch();
    EXPECT_EQ(rig.source.copies, 4U);
}

// --- Events from a pulled batch wait for a regular poll ---------------------

class FixedBindingSource final : public IDirectAudioBindingSource {
  public:
    explicit FixedBindingSource(DirectAudioBindingSnapshot snapshot) noexcept
        : snapshot_(snapshot) {}
    bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept override {
        out = snapshot_;
        return true;
    }

  private:
    DirectAudioBindingSnapshot snapshot_{};
};

uint32_t EncodeCycleTimer(uint32_t seconds, uint32_t cycle) {
    return (seconds << ASFW::Timing::kCycleTimerSecondsShift) |
           (cycle << ASFW::Timing::kCycleTimerCyclesShift);
}

// The headerless replay rig of IsochRxTimingTests: kReadDelay consecutive
// cycles establish replay, a gap of three cycles loses it.
struct HeaderlessRig {
    std::array<float, 4096> input{};
    AudioTransportControlBlock control{};
    FixedBindingSource source;
    DirectAudioReceiveConsumer consumer;
    std::array<uint8_t, 24> bytes{};
    uint32_t replayReady{0};
    uint32_t timingLosses{0};
    uint32_t seconds{5};
    uint32_t cycle{100};

    HeaderlessRig()
        : source({.generation = 1, .inputBase = input.data(), .inputBytes = sizeof(input),
                  .inputFrames = 2048, .inputChannels = 2, .control = &control,
                  .sampleRateHz = 48000, .valid = true}),
          consumer(&source, {.wireFormat = ASFW::Encoding::AudioWireFormat::kRawPcm24Upper24In32LE,
                             .framing = ASFW::Encoding::AudioPacketFraming::kHeaderless,
                             .am824Slots = 2,
                             .streamChannels = 2}) {
        consumer.SetReplayReadyCallback([this] { ++replayReady; });
        consumer.SetTimingLossCallback([this] { ++timingLosses; });
        consumer.OnReceiveActivated();
    }

    void Deliver(bool readerPull, uint32_t skipCycles = 0) {
        cycle += skipCycles;
        seconds += cycle / 8000;
        cycle %= 8000;
        const uint16_t raw = static_cast<uint16_t>(((seconds & 7u) << 13) | cycle);
        bytes[0] = static_cast<uint8_t>(raw);
        bytes[1] = static_cast<uint8_t>(raw >> 8);
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = EncodeCycleTimer(seconds, cycle),
            .drainHostTicks = uint64_t{seconds} * 1'000'000'000ULL + uint64_t{cycle} * 125'000ULL,
            .readerPull = readerPull,
        };
        consumer.BeginReceiveBatch(batch);
        consumer.ConsumePacket(batch, {.descriptorIndex = 0, .payload = bytes});
        if (++cycle == 8000) {
            cycle = 0;
            ++seconds;
        }
    }
};

TEST(RxBeginReadPullTests, ReplayReadyFromAPulledBatchWaitsForARegularPoll) {
    HeaderlessRig rig;
    for (uint32_t i = 0; i < ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay; ++i) {
        rig.Deliver(/*readerPull=*/true);
    }
    ASSERT_TRUE(rig.control.rxSequenceReplay.IsEstablished()) << "the pull still decodes";
    EXPECT_EQ(rig.replayReady, 0U) << "its handler starts transmit: not on the IO thread";
    rig.Deliver(/*readerPull=*/true);
    EXPECT_EQ(rig.replayReady, 0U);
    rig.Deliver(/*readerPull=*/false);
    EXPECT_EQ(rig.replayReady, 1U);
    rig.Deliver(/*readerPull=*/false);
    EXPECT_EQ(rig.replayReady, 1U) << "delivered once";
}

TEST(RxBeginReadPullTests, ATimingLossFromAPulledBatchWaitsForARegularPoll) {
    HeaderlessRig rig;
    for (uint32_t i = 0; i < ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay; ++i) {
        rig.Deliver(/*readerPull=*/false);
    }
    ASSERT_TRUE(rig.control.rxSequenceReplay.IsEstablished());
    rig.Deliver(/*readerPull=*/true, /*skipCycles=*/3);
    EXPECT_FALSE(rig.control.rxSequenceReplay.IsEstablished());
    EXPECT_EQ(rig.timingLosses, 0U);
    rig.Deliver(/*readerPull=*/false);
    EXPECT_EQ(rig.timingLosses, 1U);
}

TEST(RxBeginReadPullTests, EventsAPullDeferredDoNotOutliveTheRun) {
    HeaderlessRig rig;
    for (uint32_t i = 0; i < ASFW::Audio::Runtime::RxSequenceReplayState::kReadDelay; ++i) {
        rig.Deliver(/*readerPull=*/true);
    }
    rig.consumer.OnReceiveQuiesced();
    rig.consumer.OnReceiveActivated();
    rig.Deliver(/*readerPull=*/false);
    EXPECT_EQ(rig.replayReady, 0U);
}

} // namespace

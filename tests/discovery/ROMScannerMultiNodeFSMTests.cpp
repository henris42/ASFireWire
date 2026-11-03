#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#include <memory>

#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "../ASFWDriver/ConfigROM/Common/ConfigROMPolicies.hpp"
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"
#include "../ASFWDriver/Common/FWCommon.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

class MockAsyncSubsystem : public ASFW::Async::IFireWireBus {
public:
    struct PendingRead {
        ASFW::Async::InterfaceCompletionCallback callback;
        ASFW::FW::NodeId nodeId{0};
        ASFW::FW::FwSpeed speed{ASFW::FW::FwSpeed::S100};
        ASFW::Async::FWAddress address{};
        uint32_t length{0};
    };

    std::vector<PendingRead> pendingReads_;
    mutable std::mutex readsMtx_;
    mutable std::condition_variable readsCv_;

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId nodeId,
                                       ASFW::Async::FWAddress address,
                                       uint32_t length,
                                       ASFW::FW::FwSpeed speed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(readsMtx_);
        pendingReads_.push_back(PendingRead{.callback = std::move(callback),
                                            .nodeId = nodeId, .speed = speed,
                                            .address = address, .length = length});
        readsCv_.notify_all();
        return ASFW::Async::AsyncHandle{static_cast<uint32_t>(pendingReads_.size())};
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation,
                                        ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress,
                                        std::span<const uint8_t>,
                                        ASFW::FW::FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation,
                                  ASFW::FW::NodeId,
                                  ASFW::Async::FWAddress,
                                  ASFW::FW::LockOp,
                                  std::span<const uint8_t>,
                                  uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return ASFW::FW::FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{0}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    void WaitForPendingReads(size_t count) const {
        std::unique_lock lock(readsMtx_);
        readsCv_.wait_for(lock, std::chrono::seconds(1), [this, count] {
            return pendingReads_.size() >= count;
        });
    }

    void SimulateReadSuccess(size_t readIndex, const std::vector<uint32_t>& quadlets) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            if (readIndex >= pendingReads_.size()) {
                return;
            }
            callback = std::move(pendingReads_[readIndex].callback);
        }

        if (!callback) {
            return;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(quadlets.size() * 4);
        for (uint32_t q : quadlets) {
            bytes.push_back((q >> 24) & 0xFF);
            bytes.push_back((q >> 16) & 0xFF);
            bytes.push_back((q >> 8) & 0xFF);
            bytes.push_back(q & 0xFF);
        }

        callback(ASFW::Async::AsyncStatus::kSuccess, std::span(bytes.data(), bytes.size()));
    }

    void SimulateFullBIBSuccess(size_t startIdx, const std::vector<uint32_t>& bib) {
        WaitForPendingReads(startIdx + 1);
        SimulateReadSuccess(startIdx + 0, {bib[0]});
        WaitForPendingReads(startIdx + 2);
        SimulateReadSuccess(startIdx + 1, {bib[2]});
        WaitForPendingReads(startIdx + 3);
        SimulateReadSuccess(startIdx + 2, {bib[3]});
        WaitForPendingReads(startIdx + 4);
        SimulateReadSuccess(startIdx + 3, {bib[4]});
    }
    void FailRead(size_t index, ASFW::Async::AsyncStatus status = ASFW::Async::AsyncStatus::kTimeout) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            callback = std::move(pendingReads_.at(index).callback);
        }
        if (callback) callback(status, {});
    }
};

std::vector<uint32_t> CreateStandardBIBWithCrcLength4() {
    return {0x04040000, 0, 0, 0, 0};
}

std::vector<uint32_t> CreateBusyBIB() {
    return {0x00000000, 0, 0, 0, 0};
}

} // namespace

// b152: a unit slow to answer at boot was stepped S400 -> S200 -> S100 by
// response timeouts and then streamed at S100. Retries keep the node's speed,
// and when they are spent the node fails instead of slowing down.
TEST(ROMScanRetryPolicy, RetriesKeepTheNodeSpeedAndExhaustionFailsTheNode) {
    SpeedPolicy speedPolicy;
    ROMScanNodeStateMachine node(4, Generation{1}, FwSpeed::S400, 2);
    const RetryBackoffPolicy policy{};
    std::vector<ROMScanNodeStateMachine::State> transitions;
    const auto transition = [&](ROMScanNodeStateMachine&, ROMScanNodeStateMachine::State next,
                                const char*) {
        transitions.push_back(next);
        return true;
    };

    EXPECT_EQ(policy.Apply(node, speedPolicy, transition),
              RetryBackoffPolicy::Decision::RetrySameSpeed);
    EXPECT_EQ(policy.Apply(node, speedPolicy, transition),
              RetryBackoffPolicy::Decision::RetrySameSpeed);
    EXPECT_EQ(node.CurrentSpeed(), FwSpeed::S400);

    EXPECT_EQ(policy.Apply(node, speedPolicy, transition),
              RetryBackoffPolicy::Decision::FailedExhausted);
    EXPECT_EQ(node.CurrentSpeed(), FwSpeed::S400);
    ASSERT_FALSE(transitions.empty());
    EXPECT_EQ(transitions.back(), ROMScanNodeStateMachine::State::Failed);
    EXPECT_EQ(speedPolicy.ForNode(4).localToNode, FwSpeed::S400);
}

// S400 or nothing: a node behind an S200 path is not read at all, so no
// transaction ever runs below S400 and the node is never connected.
TEST(ROMScannerMultiNodeFSM, ANodeBehindASlowPathIsNotScanned) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;
    ROMScannerParams params{};
    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology{};
    topology.generation = 1;
    topology.localNodeId = 0;
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.physical.nodes.push_back({.physicalId = 0, .linkActive = true,
                                       .speedCode = 3, .portCount = 1});
    topology.physical.nodes.push_back({.physicalId = 1, .linkActive = true,
                                       .speedCode = 1, .portCount = 1});
    topology.physical.nodes[0].links[0].connected = true;
    topology.physical.nodes[0].links[0].remoteNodeId = 1;
    topology.physical.nodes[1].links[0].connected = true;
    topology.physical.nodes[1].links[0].remoteNodeId = 0;

    ROMScanRequest request{};
    request.gen = Generation{1};
    request.topology = topology;
    request.localNodeId = 0;
    std::mutex mtx;
    std::condition_variable cv;
    bool completed = false;
    std::vector<ConfigROM> roms;
    ASSERT_TRUE(scanner.Start(request, [&](Generation, std::vector<ConfigROM> found, bool) {
        std::lock_guard lock(mtx);
        completed = true;
        roms = std::move(found);
        cv.notify_one();
    }));

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return completed; });
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(roms.empty());
    std::lock_guard lock(mockAsync.readsMtx_);
    EXPECT_TRUE(mockAsync.pendingReads_.empty()) << "no read below S400";
}

TEST(ROMScannerMultiNodeFSM, AutomaticTwoNodesCompletesOnce) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;
    bool hadBusyNodes = false;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 11;
    topology.busBase16 = 0xFFC0;
    topology.physical.nodes.push_back({.physicalId = 1, .linkActive = true});
    topology.physical.nodes.push_back({.physicalId = 2, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            callbackCount++;
            hadBusyNodes = busy;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    // Interleaved BIB completion for two nodes.
    mockAsync.WaitForPendingReads(2);
    mockAsync.SimulateReadSuccess(0, {CreateStandardBIBWithCrcLength4()[0]});
    mockAsync.SimulateReadSuccess(1, {CreateStandardBIBWithCrcLength4()[0]});

    mockAsync.WaitForPendingReads(4);
    mockAsync.SimulateReadSuccess(2, {0});
    mockAsync.SimulateReadSuccess(3, {0});

    mockAsync.WaitForPendingReads(6);
    mockAsync.SimulateReadSuccess(4, {0});
    mockAsync.SimulateReadSuccess(5, {0});

    mockAsync.WaitForPendingReads(8);
    mockAsync.SimulateReadSuccess(6, {0});
    mockAsync.SimulateReadSuccess(7, {0});

    // Full root-directory parse (general ROM, crc_length == bus_info_length): each
    // node now reads its root-directory header. An empty header (0 entries) completes
    // the node. See ROMScanSession::ContinueAfterBIBSuccess (always reads the root dir;
    // cross-validated with Linux: firewire/core-device.c:650-652).
    mockAsync.WaitForPendingReads(10);
    mockAsync.SimulateReadSuccess(8, {0});
    mockAsync.SimulateReadSuccess(9, {0});

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return callbackCount > 0; });
    }

    EXPECT_EQ(callbackCount, 1);
    EXPECT_FALSE(hadBusyNodes);
    EXPECT_EQ(completedROMs.size(), 2u);
}

TEST(ROMScannerMultiNodeFSM, BusyBIBSetsBusyFlagAndRecovers) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadBusyNodes = false;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 9;
    topology.busBase16 = 0xFFC0;
    topology.physical.nodes.push_back({.physicalId = 3, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            done = true;
            hadBusyNodes = busy;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    // First BIB returns not-ready payload (q0 == 0), then retry succeeds.
    mockAsync.SimulateFullBIBSuccess(0, CreateBusyBIB());
    mockAsync.SimulateFullBIBSuccess(4, CreateStandardBIBWithCrcLength4());

    // Full root-directory parse: the recovered node reads its (empty) root-directory
    // header to complete (general ROM with crc_length == bus_info_length).
    mockAsync.WaitForPendingReads(9);
    mockAsync.SimulateReadSuccess(8, {0});

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return done; });
    }

    EXPECT_TRUE(done);
    EXPECT_TRUE(hadBusyNodes);
    EXPECT_EQ(completedROMs.size(), 1u);
}

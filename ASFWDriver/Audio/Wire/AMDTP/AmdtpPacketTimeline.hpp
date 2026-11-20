#pragma once

#include "AmdtpTypes.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

enum class PacketSlotState : uint8_t {
    Empty = 0,
    ExposedForAudio = 1,
    Completed = 3,
};

struct PacketTimelineSlot final {
    uint64_t packetIndex{0};

    uint8_t* packetBytes{nullptr};
    uint32_t packetCapacityBytes{0};
    uint32_t packetSizeBytes{0};

    bool isData{false};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};

    // Seqlock sequence: odd while the pump rewrites the slot, even when
    // stable. Every mutation (expose, no-data, reset) is bracketed by two
    // increments, so a cross-thread reader can detect both "mid-rewrite"
    // (odd) and "rewritten under me" (changed between reads).
    std::atomic<uint32_t> generation{0};
    std::atomic<PacketSlotState> state{PacketSlotState::Empty};
};

// Consistent copy of one slot, taken under the generation seqlock. The only
// live pointer is `slot`, kept for the post-write reuse recheck; all field
// reads must go through the snapshot, never back through the slot.
struct PacketSlotSnapshot final {
    const PacketTimelineSlot* slot{nullptr};
    uint32_t generation{0};
    uint64_t packetIndex{0};
    uint8_t* packetBytes{nullptr};
    uint32_t packetSizeBytes{0};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};
};

class AmdtpPacketTimeline final {
public:
    AmdtpPacketTimeline() noexcept = default;

    void Reset() noexcept;

    bool AttachSlots(PacketTimelineSlot* slots,
                     uint32_t slotCount) noexcept;

    bool ExposeDataPacket(const PreparedTxPacket& packet,
                          uint8_t* packetBytes,
                          uint32_t packetCapacityBytes) noexcept;

    void MarkNoDataPacket(uint64_t packetIndex) noexcept;
    // The newest DATA packet was rewritten as NO-DATA (RevertToNoData): its
    // frames move to the next DATA packet, so the exposed end falls back to
    // its first frame. Without this the fill takes those frames for already
    // lost and skips them.
    void RetractNewestDataPacket(uint64_t packetIndex, uint64_t firstAudioFrame) noexcept;

    // Pump-side lookup (same thread as the mutators). Returns a live slot;
    // must not be used from the IO/RT side — use SnapshotSlotForAudioFrame.
    PacketTimelineSlot* FindSlotForAudioFrame(uint64_t absoluteFrame) noexcept;
    const PacketTimelineSlot* FindSlotForAudioFrame(uint64_t absoluteFrame) const noexcept;

    // A miss's resumeFrame when no packet carries any later frame either.
    static constexpr uint64_t kNoLaterFrame = UINT64_MAX;

    // RT-safe lookup: seqlock-validated copy of the covering slot. Returns
    // false on miss or if the slot was being rewritten concurrently. Reads
    // every slot; the TX fill uses SnapshotSlotForAudioFrameNear instead.
    // The three-argument form also says, on a miss, where the frames resume:
    // the lowest first frame above absoluteFrame that a DATA packet carries,
    // or kNoLaterFrame.
    [[nodiscard]] bool SnapshotSlotForAudioFrame(uint64_t absoluteFrame,
                                                 PacketSlotSnapshot& out) const noexcept;
    [[nodiscard]] bool SnapshotSlotForAudioFrame(uint64_t absoluteFrame,
                                                 PacketSlotSnapshot& out,
                                                 uint64_t& resumeFrame) const noexcept;
    // How many times every slot was read for one frame (about 6 us each on a
    // 1512-slot ring): the lookups the walk below is there to avoid.
    [[nodiscard]] uint64_t FullScans() const noexcept {
        return fullScans_.load(std::memory_order_relaxed);
    }

    // RT-safe lookup that starts at packet `hintPacket` and steps through the
    // ring by packet index to the DATA packet that carries absoluteFrame.
    // DATA packets carry consecutive frames in packet order, so a hint at or
    // next to the frame's packet resolves in one or two slots where the scan
    // above reads all of them (1512, once per frame: about a fifth of a core
    // per stream at 96 kHz, b178). A miss is decided at the ring's edges: the
    // first packet not prepared yet, or the first one already overwritten by
    // a newer lap. A slot mid-rewrite, or frames out of packet order, fall
    // back to the scan.
    //
    // On a miss `resumeFrame` is the first frame above absoluteFrame that a
    // packet carries, or kNoLaterFrame when none does: no packet carries the
    // frames from absoluteFrame up to it either, so a caller with a run of
    // frames goes on from there. It is 0 (unknown: go on with the next frame)
    // only when a slot was mid-rewrite. Without it every frame ahead of the
    // stream's first packet repeats the walk to the ring's edge (b220: 230 us
    // for a 64-frame write, through the first 0.2 s of a stream), or the scan
    // when the hint lies before the ring's first packet (b223: 365 us).
    [[nodiscard]] bool SnapshotSlotForAudioFrameNear(uint64_t absoluteFrame,
                                                     uint64_t hintPacket,
                                                     PacketSlotSnapshot& out,
                                                     uint64_t& resumeFrame) const noexcept;
    [[nodiscard]] bool SnapshotSlotForAudioFrameNear(uint64_t absoluteFrame,
                                                     uint64_t hintPacket,
                                                     PacketSlotSnapshot& out) const noexcept {
        uint64_t resumeFrame = 0;
        return SnapshotSlotForAudioFrameNear(absoluteFrame, hintPacket, out, resumeFrame);
    }

    PacketTimelineSlot* SlotByIndex(uint64_t packetIndex) noexcept;
    const PacketTimelineSlot* SlotByIndex(uint64_t packetIndex) const noexcept;

    [[nodiscard]] uint32_t SlotCount() const noexcept;

    [[nodiscard]] uint64_t ExposedFrameEnd() const noexcept;

private:
    // What the ring position of one packet index holds, read under the slot's
    // seqlock.
    enum class SlotProbe : uint8_t {
        kData,      // that packet, exposed DATA; the snapshot holds it
        kNoFrames,  // that packet, carrying no frames (NO-DATA or retracted)
        kNotYet,    // an older packet or nothing: not prepared yet
        kGone,      // a newer packet: overwritten a lap ago
        kBusy,      // being rewritten
    };
    [[nodiscard]] SlotProbe ProbeSlot(uint64_t packetIndex,
                                      PacketSlotSnapshot& out) const noexcept;

    static void BeginSlotWrite(PacketTimelineSlot& slot) noexcept;
    static void EndSlotWrite(PacketTimelineSlot& slot) noexcept;

    [[nodiscard]] bool ScanForAudioFrame(uint64_t absoluteFrame, PacketSlotSnapshot& out,
                                         uint64_t* resumeFrame) const noexcept;

    PacketTimelineSlot* slots_{nullptr};
    uint32_t slotCount_{0};
    mutable std::atomic<uint64_t> fullScans_{0};
    // Written by the pump, read by the RT writer for miss classification.
    std::atomic<uint64_t> exposedFrameEnd_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP

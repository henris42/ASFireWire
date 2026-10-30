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

    // RT-safe lookup: seqlock-validated copy of the covering slot. Returns
    // false on miss or if the slot was being rewritten concurrently. Reads
    // every slot; the TX fill uses SnapshotSlotForAudioFrameNear instead.
    [[nodiscard]] bool SnapshotSlotForAudioFrame(uint64_t absoluteFrame,
                                                 PacketSlotSnapshot& out) const noexcept;

    // RT-safe lookup that starts at packet `hintPacket` and steps through the
    // ring by packet index to the DATA packet that carries absoluteFrame.
    // DATA packets carry consecutive frames in packet order, so a hint at or
    // next to the frame's packet resolves in one or two slots where the scan
    // above reads all of them (1512, once per frame: about a fifth of a core
    // per stream at 96 kHz, b178). A miss is decided at the ring's edges: the
    // first packet not prepared yet, or the first one already overwritten by
    // a newer lap. A slot mid-rewrite, or frames out of packet order, fall
    // back to the scan.
    [[nodiscard]] bool SnapshotSlotForAudioFrameNear(uint64_t absoluteFrame,
                                                     uint64_t hintPacket,
                                                     PacketSlotSnapshot& out) const noexcept;

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

    PacketTimelineSlot* slots_{nullptr};
    uint32_t slotCount_{0};
    // Written by the pump, read by the RT writer for miss classification.
    std::atomic<uint64_t> exposedFrameEnd_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP

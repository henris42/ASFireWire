#ifndef ASFW_DV_CAPTURE_ATOMICS_H
#define ASFW_DV_CAPTURE_ATOMICS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ASFWAtomicLoadU32Acquire(const volatile uint32_t* value);
void ASFWAtomicStoreU32Release(volatile uint32_t* value, uint32_t desired);

// Copies `length` bytes of the driver's shared status block into `out` unless
// the driver was writing it: the block's sequence (a uint64 at `sequenceOffset`)
// is odd while it is written and changes with every write (a seqlock).
// Returns the copied sequence, or 0 when the copy may be torn. Inline, so every
// target that imports the bridging header has it.
static inline uint64_t ASFWCopySeqlockedBlock(const void* shared, void* out, size_t length,
                                              size_t sequenceOffset) {
    const uint64_t* sequence = (const uint64_t*)((const uint8_t*)shared + sequenceOffset);
    const uint64_t before = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    if ((before & 1u) != 0) {
        return 0;
    }
    memcpy(out, shared, length);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint64_t after = __atomic_load_n(sequence, __ATOMIC_RELAXED);
    return before == after ? before : 0;
}

#ifdef __cplusplus
}
#endif

#endif

#include "replay_ring.h"

#include <stddef.h>

void replay_ring_reset(ReplayRing *ring)
{
    atomic_init(&ring->published, 0);
    for (unsigned worker = 0; worker < WORKER_COUNT; ++worker) {
        atomic_init(&ring->consumed[worker], 0);
    }
}

bool replay_ring_publish(ReplayRing *ring, const uint8_t bytes[FRAME_BYTES])
{
    uint32_t index = atomic_load_explicit(&ring->published, memory_order_relaxed);
    for (unsigned worker = 0; worker < WORKER_COUNT; ++worker) {
        uint32_t consumed = atomic_load_explicit(&ring->consumed[worker], memory_order_acquire);
        if (index - consumed == RING_CAPACITY) {
            return false;
        }
    }

    ReplayFrame *frame = &ring->slots[index % RING_CAPACITY];
    frame->index = index;
    for (unsigned lead = 0; lead < FRAME_LEADS; ++lead) {
        uint16_t code = bytes[2 * lead] | ((uint16_t)bytes[2 * lead + 1] << 8);
        frame->samples[lead] = code <= INT16_MAX ? code : (int32_t)code - 65536;
    }

    // Publish the fully initialized slot to both readers.
    atomic_store_explicit(&ring->published, index + 1, memory_order_release);
    return true;
}

const ReplayFrame *replay_ring_peek(const ReplayRing *ring, unsigned worker)
{
    uint32_t index = atomic_load_explicit(&ring->consumed[worker], memory_order_relaxed);
    if (index == atomic_load_explicit(&ring->published, memory_order_acquire)) {
        return NULL;
    }
    return &ring->slots[index % RING_CAPACITY];
}

void replay_ring_finish(ReplayRing *ring, unsigned worker)
{
    uint32_t index = atomic_load_explicit(&ring->consumed[worker], memory_order_relaxed);
    // Permit reuse only once this worker has completed every access to the slot.
    atomic_store_explicit(&ring->consumed[worker], index + 1, memory_order_release);
}

uint32_t replay_ring_completed(const ReplayRing *ring)
{
    uint32_t first = atomic_load_explicit(&ring->consumed[0], memory_order_acquire);
    uint32_t second = atomic_load_explicit(&ring->consumed[1], memory_order_acquire);
    return first < second ? first : second;
}

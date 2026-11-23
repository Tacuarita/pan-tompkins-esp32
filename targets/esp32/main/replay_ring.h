#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
    REPLAY_RATE_HZ = 257,
    WORKER_COUNT = 2,
    LEADS_PER_WORKER = 3,
    FRAME_LEADS = WORKER_COUNT * LEADS_PER_WORKER,
    FRAME_BYTES = FRAME_LEADS * 2,
    RING_CAPACITY = 64,
};

typedef struct {
    uint32_t index;
    int16_t samples[FRAME_LEADS];
} ReplayFrame;

typedef struct {
    ReplayFrame slots[RING_CAPACITY];
    _Atomic uint32_t published;
    _Atomic uint32_t consumed[WORKER_COUNT];
} ReplayRing;

void replay_ring_reset(ReplayRing *ring);
bool replay_ring_publish(ReplayRing *ring, const uint8_t bytes[FRAME_BYTES]);
const ReplayFrame *replay_ring_peek(const ReplayRing *ring, unsigned worker);
void replay_ring_finish(ReplayRing *ring, unsigned worker);
uint32_t replay_ring_completed(const ReplayRing *ring);

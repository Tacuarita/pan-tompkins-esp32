#include "replay_ring.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

enum { TEST_FRAMES = 7710 };

static ReplayRing ring;
static uint8_t payload[TEST_FRAMES][FRAME_BYTES];

static uint16_t code_for_frame(uint32_t index, unsigned lead)
{
    return (uint16_t)(0x7ffe + index + lead * 131);
}

static void check_frame(const ReplayFrame *frame, uint32_t index, unsigned first_lead,
                        unsigned lead_count)
{
    assert(frame != NULL);
    assert(frame->index == index);
    for (unsigned lead = first_lead; lead < first_lead + lead_count; ++lead) {
        assert((uint16_t)frame->samples[lead] == code_for_frame(index, lead));
    }
}

static void check_full_ring(void)
{
    replay_ring_reset(&ring);
    for (uint32_t index = 0; index < RING_CAPACITY; ++index) {
        assert(replay_ring_publish(&ring, payload[index]));
    }

    const ReplayFrame *held = replay_ring_peek(&ring, 1);
    assert(held == replay_ring_peek(&ring, 0));
    for (uint32_t index = 0; index < RING_CAPACITY; ++index) {
        check_frame(replay_ring_peek(&ring, 0), index, 0, FRAME_LEADS);
        replay_ring_finish(&ring, 0);
    }
    assert(!replay_ring_publish(&ring, payload[RING_CAPACITY]));
    check_frame(held, 0, 0, FRAME_LEADS);
    assert(replay_ring_peek(&ring, 0) == NULL);

    replay_ring_finish(&ring, 1);
    assert(replay_ring_publish(&ring, payload[RING_CAPACITY]));
    check_frame(replay_ring_peek(&ring, 0), RING_CAPACITY, 0, FRAME_LEADS);
    check_frame(replay_ring_peek(&ring, 1), 1, 0, FRAME_LEADS);
    assert(!replay_ring_publish(&ring, payload[RING_CAPACITY + 1]));
}

static void *verify_worker(void *argument)
{
    unsigned worker = *(const unsigned *)argument;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index) {
        const ReplayFrame *frame;
        while ((frame = replay_ring_peek(&ring, worker)) == NULL) {
            sched_yield();
        }
        if ((index + worker) % 3 == 0) {
            sched_yield();
        }
        check_frame(frame, index, worker * LEADS_PER_WORKER, LEADS_PER_WORKER);
        replay_ring_finish(&ring, worker);
    }
    assert(replay_ring_peek(&ring, worker) == NULL);
    return NULL;
}

int main(void)
{
    for (uint32_t index = 0; index < TEST_FRAMES; ++index) {
        for (unsigned lead = 0; lead < FRAME_LEADS; ++lead) {
            uint16_t code = code_for_frame(index, lead);
            payload[index][2 * lead] = code & 0xff;
            payload[index][2 * lead + 1] = code >> 8;
        }
    }
    check_full_ring();

    replay_ring_reset(&ring);
    pthread_t threads[WORKER_COUNT];
    unsigned ids[WORKER_COUNT] = {0, 1};
    for (unsigned worker = 0; worker < WORKER_COUNT; ++worker) {
        assert(pthread_create(&threads[worker], NULL, verify_worker, &ids[worker]) == 0);
    }
    for (uint32_t index = 0; index < TEST_FRAMES; ++index) {
        while (!replay_ring_publish(&ring, payload[index])) {
            sched_yield();
        }
    }
    for (unsigned worker = 0; worker < WORKER_COUNT; ++worker) {
        assert(pthread_join(threads[worker], NULL) == 0);
    }
    puts("PASS ring slot lifetime, full-ring rejection, and both 7710-frame worker sequences");
}

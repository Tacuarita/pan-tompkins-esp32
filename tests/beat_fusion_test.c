#include "beat_fusion.h"
#include "replay_ring.h"

#include <assert.h>
#include <stdio.h>

#define STREAM(array) {array, sizeof(array) / sizeof(array[0])}

static const PtCandidate lead_i[] = {{10, 18}, {32, 32}, {34, 35}, {63, 64},
                                     {90, 91}, {110, 112}};
static const PtCandidate lead_ii[] = {{16, 17}, {40, 49}, {92, 94}, {111, 113}};
static const PtCandidate lead_avr[] = {{15, 23}, {21, 29}, {62, 70}};
static const PtCandidate lead_avl[] = {{37, 46}, {114, 123}};
static const FusionLead fixture[FUSION_LEADS] = {
    [0] = STREAM(lead_i), [1] = STREAM(lead_ii),
    [3] = STREAM(lead_avr), [4] = STREAM(lead_avl),
};
static const uint32_t expected[] = {10, 16, 62, 90, 110};

typedef struct {
    const uint32_t *expected;
    size_t capacity;
    size_t emitted;
} ExpectedBeats;

static void verify_beat(void *context, uint32_t location)
{
    ExpectedBeats *beats = context;
    assert(beats->emitted < beats->capacity);
    assert(location == beats->expected[beats->emitted++]);
}

static void check_cutoffs(void)
{
    BeatFusion fusion = {.window = 5, .max_delay = 8};
    ExpectedBeats beats = {expected, 5, 0};
    // Even fully published list entries cannot be used ahead of both handoffs.
    beat_fusion_advance(&fusion, fixture, 23, verify_beat, &beats);
    assert(beats.emitted == 0);
    beat_fusion_advance(&fusion, fixture, 24, verify_beat, &beats);
    assert(beats.emitted == 1);
    beat_fusion_advance(&fusion, fixture, 30, verify_beat, &beats);
    assert(beats.emitted == 2);
    beat_fusion_advance(&fusion, fixture, 123, verify_beat, &beats);
    assert(beats.emitted == 4);
    assert(fusion.late_count == 2);
    beat_fusion_advance(&fusion, fixture, 124, verify_beat, &beats);
    beat_fusion_advance(&fusion, fixture, 124, verify_beat, &beats);
    assert(beats.emitted == 5);
    assert(fusion.late_count == 3);
}

static void check_handoff_order(unsigned delayed_worker)
{
    ReplayRing ring;
    replay_ring_reset(&ring);
    BeatFusion fusion = {.window = 5, .max_delay = 8};
    ExpectedBeats beats = {expected, 5, 0};
    FusionLead visible[FUSION_LEADS] = {0};
    for (unsigned lead = 0; lead < FUSION_LEADS; ++lead) {
        visible[lead].candidates = fixture[lead].candidates;
    }

    const uint8_t bytes[FRAME_BYTES] = {0};
    for (uint32_t tick = 0; tick < 131; ++tick) {
        if (tick < 124) {
            assert(replay_ring_publish(&ring, bytes));
        }
        for (unsigned worker = 0; worker < WORKER_COUNT; ++worker) {
            if (worker == delayed_worker && tick % 7 != 4) {
                continue;
            }
            const ReplayFrame *frame;
            while ((frame = replay_ring_peek(&ring, worker)) != NULL) {
                for (unsigned lead = worker * LEADS_PER_WORKER;
                     lead < (worker + 1) * LEADS_PER_WORKER; ++lead) {
                    size_t count = visible[lead].count;
                    if (count < fixture[lead].count &&
                        fixture[lead].candidates[count].production_frame == frame->index) {
                        ++visible[lead].count;
                    }
                }
                beat_fusion_advance(&fusion, visible, replay_ring_completed(&ring),
                                     verify_beat, &beats);
                replay_ring_finish(&ring, worker);
                uint32_t completed = replay_ring_completed(&ring);
                beat_fusion_advance(&fusion, visible, completed, verify_beat, &beats);
                size_t due = 0;
                while (due < beats.capacity && completed > expected[due] + 5 + 8) {
                    ++due;
                }
                assert(beats.emitted == due);
            }
        }
    }
    assert(replay_ring_completed(&ring) == 124);
    assert(beats.emitted == beats.capacity);
    assert(fusion.late_count == 3);
}

static void check_index_limits(void)
{
    const uint32_t edge = UINT32_MAX - 1;
    const PtCandidate candidate = {edge, edge};
    const FusionLead leads[FUSION_LEADS] = {{&candidate, 1}, {&candidate, 1}};
    ExpectedBeats beats = {&edge, 1, 0};
    BeatFusion fusion = {.window = 3, .max_delay = 5};
    beat_fusion_advance(&fusion, leads, UINT32_MAX, verify_beat, &beats);
    assert(beats.emitted == 0);
    fusion = (BeatFusion){0};
    beat_fusion_advance(&fusion, leads, edge, verify_beat, &beats);
    assert(beats.emitted == 0);
    beat_fusion_advance(&fusion, leads, UINT32_MAX, verify_beat, &beats);
    assert(beats.emitted == 1);
}

int main(void)
{
    check_cutoffs();
    check_handoff_order(WORKER_COUNT);
    check_handoff_order(0);
    check_handoff_order(1);
    check_index_limits();
    puts("PASS fusion inclusive boundaries, candidate ownership, delayed handoffs, and index limits");
}

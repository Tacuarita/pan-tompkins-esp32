#include "beat_fusion.h"

static bool is_late(const BeatFusion *fusion, PtCandidate candidate)
{
    return candidate.production_frame - candidate.beat_location > fusion->max_delay;
}

static void skip_late(BeatFusion *fusion, const FusionLead *lead, unsigned index)
{
    while (fusion->next[index] < fusion->handed_off[index]
           && is_late(fusion, lead->candidates[fusion->next[index]])) {
        ++fusion->next[index];
    }
}

void beat_fusion_advance(BeatFusion *fusion, const FusionLead leads[FUSION_LEADS],
                         uint32_t completed, FusionEmit emit, void *context)
{
    for (unsigned i = 0; i < FUSION_LEADS; ++i) {
        size_t *handed_off = &fusion->handed_off[i];
        while (*handed_off < leads[i].count) {
            PtCandidate candidate = leads[i].candidates[*handed_off];
            if (candidate.production_frame >= completed) {
                break;
            }
            fusion->late_count += is_late(fusion, candidate);
            ++*handed_off;
        }
        skip_late(fusion, &leads[i], i);
    }

    for (;;) {
        uint32_t anchor = UINT32_MAX;
        for (unsigned i = 0; i < FUSION_LEADS; ++i) {
            size_t next = fusion->next[i];
            if (next < fusion->handed_off[i] && leads[i].candidates[next].beat_location < anchor) {
                anchor = leads[i].candidates[next].beat_location;
            }
        }
        uint64_t end = (uint64_t)anchor + fusion->window;
        // Any eligible candidate at or before end was produced by end + H.
        if (completed <= end + fusion->max_delay) {
            return;
        }

        unsigned confirming_leads = 0;
        for (unsigned i = 0; i < FUSION_LEADS; ++i) {
            size_t *next = &fusion->next[i];
            bool confirms = false;
            while (*next < fusion->handed_off[i] && leads[i].candidates[*next].beat_location <= end) {
                confirms = true;
                ++*next;
                skip_late(fusion, &leads[i], i);
            }
            confirming_leads += confirms;
        }
        if (confirming_leads >= 2) {
            emit(context, anchor);
        }
    }
}

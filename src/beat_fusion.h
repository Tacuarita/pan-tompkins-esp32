#pragma once

#include "pan_tompkins.h"

#include <stddef.h>

enum { FUSION_LEADS = 6 };

typedef struct {
    const PtCandidate *candidates;
    size_t count;
} FusionLead;

typedef struct {
    uint32_t window;
    uint32_t max_delay;
    size_t handed_off[FUSION_LEADS];
    size_t next[FUSION_LEADS];
    uint64_t late_count;
} BeatFusion;

typedef void (*FusionEmit)(void *context, uint32_t location);

/*
 * Zero-initialize cursors and counts at the start of a replay. Each lead's
 * append-only list is strictly increasing in both location and production
 * frame. completed is the smaller worker completion count (frame 0 is
 * complete when completed is 1). Lists may already include newer candidates.
 */
void beat_fusion_advance(BeatFusion *fusion, const FusionLead leads[FUSION_LEADS],
                         uint32_t completed, FusionEmit emit, void *context);

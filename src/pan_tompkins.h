#ifndef PAN_TOMPKINS_H
#define PAN_TOMPKINS_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PT_SAMPLE_RATE = 257,
    PT_INTEGRATION_SAMPLES = (150 * PT_SAMPLE_RATE + 500) / 1000,
    PT_REFRACTORY_SAMPLES = (200 * PT_SAMPLE_RATE + 500) / 1000,
    PT_T_WAVE_SAMPLES = (360 * PT_SAMPLE_RATE + 500) / 1000,
    PT_FILTER_DELAY = 64,
    PT_FILTER_SAMPLES = 2 * PT_FILTER_DELAY + 1,
    PT_DERIVATIVE_DELAY = 2,
    PT_DERIVATIVE_LIMIT = 2896,
    PT_SQUARE_SHIFT = 8,
    PT_TRIPLET_LEADS = 3,
    PT_RR_INTERVALS = 8
};

typedef struct {
    uint32_t beat_location;
    uint32_t production_frame;
} PtCandidate;

typedef struct {
    uint16_t magnitude;
    uint16_t slope;
    uint16_t energy;
} PtFeature;

typedef struct {
    uint32_t location;
    uint16_t band;
    uint16_t slope;
    uint16_t integrated;
} PtPeak;

/* Caller-owned state. Zero-initialize once per lead to start a replay. */
typedef struct {
    uint32_t next_frame;
    int16_t input[PT_FILTER_SAMPLES];
    int16_t band[2 * PT_DERIVATIVE_DELAY + 1];
    PtFeature features[PT_INTEGRATION_SAMPLES + 1];
    uint32_t energy_sum;
    uint32_t previous_energy_sum;
    uint16_t signal_band;
    uint16_t noise_band;
    uint16_t signal_energy;
    uint16_t noise_energy;
    PtPeak last_beat;
    PtPeak search_peak;
    uint32_t rr[PT_RR_INTERVALS];
    uint64_t rr_sum;
    unsigned rr_count;
    unsigned rr_next;
} PtDetector;

/* For |derivative| <= PT_DERIVATIVE_LIMIT, returns a signed-16-safe value. */
uint16_t pt_square(int16_t derivative);

/*
 * Consume one sample and return whether a candidate was written.
 * Locations are relative to the first sample, at frame zero. Each state
 * supports at most UINT32_MAX samples. candidate is required.
 */
bool pt_process(PtDetector *detector, int16_t sample, PtCandidate *candidate);

/* Inputs are clipped derivatives. Results are in the same lead order. */
typedef void PtSquareTriplet(const int16_t derivatives[PT_TRIPLET_LEADS],
                             uint16_t energies[PT_TRIPLET_LEADS]);
PtSquareTriplet pt_square_triplet_scalar;

/* Consume one sample per independent detector. Bit n marks candidate n. */
unsigned pt_process_triplet(PtDetector *const detectors[PT_TRIPLET_LEADS],
                            const int16_t samples[PT_TRIPLET_LEADS],
                            PtCandidate candidates[PT_TRIPLET_LEADS], PtSquareTriplet *square);

#endif

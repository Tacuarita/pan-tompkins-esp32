#include "pan_tompkins.h"

/* Q15 taps, from the center outward. See docs/scalar-detector.md. */
static const int16_t bandpass[PT_FILTER_DELAY + 1] = {
    2610, 2462, 2219, 1839, 1354, 804, 236, -306, -782, -1160,
    -1419, -1550, -1557, -1455, -1267, -1021, -749, -480, -238, -44,
    94, 171, 193, 171, 117, 49, -20, -76, -110, -118,
    -101, -62, -7, 54, 114, 165, 201, 220, 220, 204,
    175, 138, 97, 58, 25, -1, -17, -25, -25, -20,
    -12, -3, 5, 10, 13, 13, 10, 5, -1, -8,
    -14, -19, -23, -25, -26
};

enum {
    FEATURE_SLOTS = PT_INTEGRATION_SAMPLES + 1,
    BAND_SLOTS = 2 * PT_DERIVATIVE_DELAY + 1,
    FIRST_DECISION_FRAME = PT_FILTER_SAMPLES - 1 + BAND_SLOTS - 1
                           + PT_INTEGRATION_SAMPLES,
    MAX_PEAK_DELAY = PT_FILTER_DELAY + PT_DERIVATIVE_DELAY + PT_INTEGRATION_SAMPLES
};

static int32_t round_divide(int32_t value, int32_t divisor)
{
    if (value < 0) {
        return -((-value + divisor / 2) / divisor);
    }
    return (value + divisor / 2) / divisor;
}

static int16_t clamp(int32_t value, int16_t minimum, int16_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return (int16_t)value;
}

static uint16_t magnitude(int16_t value)
{
    int32_t wide = value;
    return (uint16_t)(wide < 0 ? -wide : wide);
}

static int16_t filter(PtDetector *detector, uint32_t frame, int16_t sample)
{
    unsigned recent = frame % PT_FILTER_SAMPLES;
    detector->input[recent] = sample;
    unsigned older = (recent + 1) % PT_FILTER_SAMPLES;
    int32_t sum = 0;
    for (unsigned distance = PT_FILTER_DELAY; distance > 0; --distance) {
        int32_t pair = (int32_t)detector->input[recent] + detector->input[older];
        sum += (int32_t)bandpass[distance] * pair;
        recent = (recent + PT_FILTER_SAMPLES - 1) % PT_FILTER_SAMPLES;
        older = (older + 1) % PT_FILTER_SAMPLES;
    }
    sum += (int32_t)bandpass[0] * detector->input[recent];
    return clamp(round_divide(sum, 32768), INT16_MIN, INT16_MAX);
}

uint16_t pt_square(int16_t derivative)
{
    int32_t value = derivative;
    return (uint16_t)((value * value) >> PT_SQUARE_SHIFT);
}

void pt_square_triplet_scalar(const int16_t derivatives[PT_TRIPLET_LEADS],
                              uint16_t energies[PT_TRIPLET_LEADS])
{
    for (unsigned lead = 0; lead < PT_TRIPLET_LEADS; ++lead) {
        energies[lead] = pt_square(derivatives[lead]);
    }
}

static int16_t prepare_feature(PtDetector *detector, int16_t sample)
{
    uint32_t frame = detector->next_frame++;
    int16_t filtered = filter(detector, frame, sample);
    unsigned slot = frame % BAND_SLOTS;
    detector->band[slot] = filtered;
    int32_t derivative = 2 * (int32_t)filtered
                       + detector->band[(slot + BAND_SLOTS - 1) % BAND_SLOTS]
                       - detector->band[(slot + BAND_SLOTS - 3) % BAND_SLOTS]
                       - 2 * (int32_t)detector->band[(slot + BAND_SLOTS - 4) % BAND_SLOTS];
    int16_t clipped = clamp(round_divide(derivative, 2),
                            -PT_DERIVATIVE_LIMIT, PT_DERIVATIVE_LIMIT);

    PtFeature *feature = &detector->features[frame % FEATURE_SLOTS];
    feature->magnitude = magnitude(detector->band[(slot + BAND_SLOTS - PT_DERIVATIVE_DELAY)
                                                  % BAND_SLOTS]);
    feature->slope = magnitude(clipped);
    return clipped;
}

static PtPeak locate_peak(const PtDetector *detector, uint32_t frame, uint16_t integrated)
{
    PtPeak peak = {.integrated = integrated};
    for (uint32_t i = frame - PT_INTEGRATION_SAMPLES; i < frame; ++i) {
        const PtFeature *feature = &detector->features[i % FEATURE_SLOTS];
        if (feature->magnitude > peak.band) {
            peak.band = feature->magnitude;
            peak.location = i - PT_FILTER_DELAY - PT_DERIVATIVE_DELAY;
        }
        if (feature->slope > peak.slope) {
            peak.slope = feature->slope;
        }
    }
    return peak;
}

static uint16_t update_level(uint16_t level, uint16_t peak)
{
    return (uint16_t)((7u * level + peak + 4u) / 8u);
}

static uint16_t threshold(uint16_t signal, uint16_t noise)
{
    if (signal > noise) {
        return (uint16_t)(noise + (signal - noise + 3u) / 4u);
    }
    return noise > 0 ? noise : 1;
}

static bool plausible(const PtDetector *detector, PtPeak peak)
{
    if (detector->last_beat.integrated == 0) {
        return true;
    }
    if (peak.location <= detector->last_beat.location) {
        return false;
    }
    uint32_t interval = peak.location - detector->last_beat.location;
    return interval >= PT_REFRACTORY_SAMPLES
        && (interval >= PT_T_WAVE_SAMPLES || 2u * peak.slope >= detector->last_beat.slope);
}

static void accept(PtDetector *detector, PtPeak peak, uint32_t frame,
                   PtCandidate *candidate)
{
    if (detector->last_beat.integrated != 0) {
        uint32_t interval = peak.location - detector->last_beat.location;
        detector->rr_sum -= detector->rr[detector->rr_next];
        detector->rr[detector->rr_next] = interval;
        detector->rr_sum += interval;
        detector->rr_next = (detector->rr_next + 1) % PT_RR_INTERVALS;
        if (detector->rr_count < PT_RR_INTERVALS) {
            ++detector->rr_count;
        }
    }
    detector->last_beat = peak;
    detector->search_peak = (PtPeak){0};
    detector->signal_band = update_level(detector->signal_band, peak.band);
    detector->signal_energy = update_level(detector->signal_energy, peak.integrated);
    *candidate = (PtCandidate){peak.location, frame};
}

static bool finish_feature(PtDetector *detector, uint16_t energy, PtCandidate *candidate)
{
    uint32_t frame = detector->next_frame - 1;
    uint32_t previous_sum = detector->energy_sum;
    unsigned slot = frame % FEATURE_SLOTS;
    detector->energy_sum -= detector->features[(slot + 1) % FEATURE_SLOTS].energy;
    detector->features[slot].energy = energy;
    detector->energy_sum += energy;
    bool local_maximum = previous_sum >= detector->previous_energy_sum
                      && previous_sum > detector->energy_sum;
    detector->previous_energy_sum = previous_sum;

    if (frame < FIRST_DECISION_FRAME) {
        return false;
    }
    uint16_t band_limit = threshold(detector->signal_band, detector->noise_band);
    uint16_t energy_limit = threshold(detector->signal_energy, detector->noise_energy);
    uint16_t search_band = (uint16_t)((band_limit + 1u) / 2u);
    uint16_t search_energy = (uint16_t)((energy_limit + 1u) / 2u);

    if (local_maximum) {
        uint16_t integrated = (uint16_t)(previous_sum / PT_INTEGRATION_SAMPLES);
        PtPeak peak = locate_peak(detector, frame, integrated);
        bool eligible = plausible(detector, peak);
        if (eligible && peak.band >= band_limit && peak.integrated >= energy_limit) {
            accept(detector, peak, frame, candidate);
            return true;
        }
        if (eligible && peak.band >= search_band && peak.integrated >= search_energy
            && peak.integrated > detector->search_peak.integrated) {
            detector->search_peak = peak;
        }
        detector->noise_band = update_level(detector->noise_band, peak.band);
        detector->noise_energy = update_level(detector->noise_energy, peak.integrated);
    }

    if (detector->rr_count != 0 && detector->search_peak.integrated >= search_energy
        && detector->search_peak.band >= search_band) {
        uint64_t mean_interval = detector->rr_sum / detector->rr_count;
        uint64_t missed_limit = (166 * mean_interval + 50) / 100;
        if (frame - detector->last_beat.location >= missed_limit + MAX_PEAK_DELAY) {
            accept(detector, detector->search_peak, frame, candidate);
            return true;
        }
    }
    return false;
}

bool pt_process(PtDetector *detector, int16_t sample, PtCandidate *candidate)
{
    int16_t derivative = prepare_feature(detector, sample);
    return finish_feature(detector, pt_square(derivative), candidate);
}

unsigned pt_process_triplet(PtDetector *const detectors[PT_TRIPLET_LEADS],
                            const int16_t samples[PT_TRIPLET_LEADS],
                            PtCandidate candidates[PT_TRIPLET_LEADS], PtSquareTriplet *square)
{
    int16_t derivatives[PT_TRIPLET_LEADS];
    uint16_t energies[PT_TRIPLET_LEADS];
    for (unsigned lead = 0; lead < PT_TRIPLET_LEADS; ++lead) {
        derivatives[lead] = prepare_feature(detectors[lead], samples[lead]);
    }
    square(derivatives, energies);
    unsigned produced = 0;
    for (unsigned lead = 0; lead < PT_TRIPLET_LEADS; ++lead) {
        if (finish_feature(detectors[lead], energies[lead], &candidates[lead])) {
            produced |= 1u << lead;
        }
    }
    return produced;
}

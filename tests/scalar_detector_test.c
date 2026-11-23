#include "pan_tompkins.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

enum { STAGE_TEST_FRAMES = 4096 };

static double lowpass(double hz, int offset)
{
    double pi = acos(-1.0);
    if (offset == 0) {
        return 2.0 * hz / 257.0;
    }
    return sin(2.0 * pi * hz * offset / 257.0) / (pi * offset);
}

static void design_filter(int16_t taps[129])
{
    double pi = acos(-1.0);
    double ideal[129];
    double gain = 0;
    for (int i = 0; i < 129; ++i) {
        int k = i - 64;
        ideal[i] = (lowpass(15, k) - lowpass(5, k))
                 * (0.54 + 0.46 * cos(pi * k / 64.0));
        gain += ideal[i] * cos(2.0 * pi * 10.0 * k / 257.0);
    }
    int dc = 0;
    for (int i = 0; i < 129; ++i) {
        taps[i] = (int16_t)lround(32768.0 * ideal[i] / gain);
        dc += taps[i];
    }
    taps[64] = (int16_t)(taps[64] - dc);
}

static void build_input(int16_t input[STAGE_TEST_FRAMES], const int16_t taps[129])
{
    for (int i = 0; i < 512; ++i) {
        input[i] = INT16_MAX;
        input[512 + i] = INT16_MIN;
        input[1024 + i] = i % 2 == 0 ? INT16_MIN : INT16_MAX;
    }
    input[1536] = 8192;
    input[1792] = -8192;

    for (int i = 0; i < 129; ++i) {
        input[2048 + i] = taps[i] > 0 ? INT16_MAX : INT16_MIN;
        input[2304 + i] = taps[i] > 0 ? INT16_MIN : INT16_MAX;
    }
    uint32_t random = UINT32_C(257);
    for (int i = 2560; i < STAGE_TEST_FRAMES; ++i) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        input[i] = (int16_t)((int32_t)(random >> 16) - 32768);
    }
}

static int at(const int16_t samples[], int frame)
{
    return frame < 0 ? 0 : samples[frame];
}

static int clip(long sample, int minimum, int maximum)
{
    if (sample < minimum) {
        return minimum;
    }
    if (sample > maximum) {
        return maximum;
    }
    return (int)sample;
}

static int absolute(int sample)
{
    return sample < 0 ? -sample : sample;
}

static void check_stages(void)
{
    int16_t taps[129];
    design_filter(taps);
    int16_t input[STAGE_TEST_FRAMES] = {0};
    build_input(input, taps);
    int16_t band[STAGE_TEST_FRAMES];
    int energy[STAGE_TEST_FRAMES];
    PtDetector detector = {0};
    bool band_clips_positive = false;
    bool band_clips_negative = false;
    bool band_ties_positive = false;
    bool band_ties_negative = false;
    bool slope_clips_positive = false;
    bool slope_clips_negative = false;
    unsigned previous_sum = 0;

    for (int n = 0; n < STAGE_TEST_FRAMES; ++n) {
        PtCandidate candidate;
        bool emitted = pt_process(&detector, input[n], &candidate);
        if (n < 512 || (n >= 683 && n < 1024)) {
            assert(!emitted);
        }

        int64_t convolution = 0;
        for (int i = 0; i < 129; ++i) {
            convolution += (int64_t)taps[i] * at(input, n - i);
        }
        band_ties_positive |= convolution % 32768 == 16384;
        band_ties_negative |= convolution % 32768 == -16384;
        long rounded_band = lround((double)convolution / 32768.0);
        band_clips_positive |= rounded_band > INT16_MAX;
        band_clips_negative |= rounded_band < INT16_MIN;
        band[n] = (int16_t)clip(rounded_band, INT16_MIN, INT16_MAX);
        assert(detector.band[n % 5] == band[n]);

        long derivative = lround(at(band, n) + 0.5 * at(band, n - 1)
                               - 0.5 * at(band, n - 3) - at(band, n - 4));
        slope_clips_positive |= derivative > 2896;
        slope_clips_negative |= derivative < -2896;
        int clipped = clip(derivative, -2896, 2896);
        energy[n] = clipped * clipped / 256;
        const PtFeature *feature = &detector.features[n % 40];
        assert(feature->magnitude == absolute(at(band, n - 2)));
        assert(feature->slope == absolute(clipped));
        assert(feature->energy == energy[n]);

        unsigned integrated_sum = 0;
        for (int i = n - 38; i <= n; ++i) {
            if (i >= 0) {
                integrated_sum += (unsigned)energy[i];
            }
        }
        assert(detector.energy_sum == integrated_sum);
        assert(detector.previous_energy_sum == previous_sum);
        previous_sum = integrated_sum;
        if ((n >= 171 && n < 512) || (n >= 683 && n < 1024)) {
            assert(detector.energy_sum == 0);
            assert(band[n] == 0);
        }
    }
    assert(band_ties_positive && band_ties_negative);
    assert(band_clips_positive && band_clips_negative);
    assert(slope_clips_positive && slope_clips_negative);
    for (int value = -2896; value <= 2896; ++value) {
        assert(pt_square((int16_t)value) == value * value / 256);
    }
    assert(pt_square(-2896) == 32761);
}

static bool synthetic_peak(unsigned interval, uint16_t slope, uint16_t magnitude,
                           uint16_t energy, bool plateau_ends)
{
    unsigned sum = 39u * energy + 38u;
    PtDetector detector = {
        .next_frame = 400,
        .energy_sum = sum,
        .previous_energy_sum = sum,
        .signal_band = 2000,
        .signal_energy = 4000,
        .last_beat = {.location = 331 - interval, .slope = 1000, .integrated = 1}
    };
    /* At frame 400 slot 1 expires. A one-count drop ends the plateau. */
    detector.features[1].energy = plateau_ends ? 1 : 0;
    detector.features[397 % 40] = (PtFeature){.magnitude = magnitude, .slope = slope};
    detector.features[399 % 40] = (PtFeature){.magnitude = magnitude, .slope = slope};

    PtCandidate candidate = {0};
    bool emitted = pt_process(&detector, 0, &candidate);
    if (emitted) {
        assert(candidate.beat_location == 331);
        assert(candidate.production_frame == 400);
        assert(detector.signal_band == 1813);
        assert(detector.signal_energy == 3625);
    } else {
        assert(candidate.beat_location == 0 && candidate.production_frame == 0);
    }
    return emitted;
}

static int pulse(int frame, int center, int height)
{
    int distance = absolute(frame - center);
    return distance < 9 ? height * (9 - distance) / 9 : 0;
}

static void check_decisions(void)
{
    assert(!synthetic_peak(50, 1000, 500, 1000, true));
    assert(synthetic_peak(51, 1000, 500, 1000, true));
    assert(!synthetic_peak(92, 499, 500, 1000, true));
    assert(synthetic_peak(92, 500, 500, 1000, true));
    assert(synthetic_peak(93, 499, 500, 1000, true));
    assert(!synthetic_peak(100, 1000, 499, 1000, true));
    assert(!synthetic_peak(100, 1000, 500, 999, true));
    assert(!synthetic_peak(100, 1000, 500, 1000, false));

    static const PtCandidate expected[] = {
        {300, 386}, {460, 546}, {620, 706}, {780, 866}, {940, 1026},
        {1100, 1186}, {1260, 1346}, {1420, 1506}, {1580, 1666},
        {1740, 1951}, {2060, 2146}
    };
    PtDetector positive = {0};
    PtDetector negative = {0};
    unsigned next = 0;
    for (int n = 0; n < 2400; ++n) {
        int sample = pulse(n, 1740, 500) + pulse(n, 2060, 1200);
        for (int center = 300; center <= 1580; center += 160) {
            sample += pulse(n, center, 1200);
        }
        PtCandidate candidate_positive;
        PtCandidate candidate_negative;
        bool detected = pt_process(&positive, (int16_t)sample, &candidate_positive);
        assert(detected == pt_process(&negative, (int16_t)-sample, &candidate_negative));
        if (detected) {
            assert(next < sizeof(expected) / sizeof(expected[0]));
            assert(candidate_positive.beat_location == expected[next].beat_location);
            assert(candidate_positive.production_frame == expected[next].production_frame);
            assert(candidate_positive.beat_location == candidate_negative.beat_location);
            assert(candidate_positive.production_frame == candidate_negative.production_frame);
            ++next;
        }
    }
    assert(next == sizeof(expected) / sizeof(expected[0]));
}

int main(void)
{
    assert(PT_SAMPLE_RATE == 257);
    assert(PT_INTEGRATION_SAMPLES == 39);
    check_stages();
    check_decisions();
    puts("Scalar stage and decision checks passed");
    return 0;
}

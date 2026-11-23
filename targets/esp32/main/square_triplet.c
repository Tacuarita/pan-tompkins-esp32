#include "square_triplet.h"

#include <string.h>

#include "esp_cpu.h"

_Static_assert((PT_DERIVATIVE_LIMIT * PT_DERIVATIVE_LIMIT >> PT_SQUARE_SHIFT) <= INT16_MAX,
               "The shifted square must fit in a signed vector lane");

void pt_square_triplet_simd(const int16_t derivatives[PT_TRIPLET_LEADS],
                            uint16_t energies[PT_TRIPLET_LEADS])
{
    // Lanes 0–2 are the worker's leads in order. Lanes 3–7 are zero.
    _Alignas(16) uint32_t packed[4];
    packed[0] = (uint16_t)derivatives[0] | ((uint32_t)(uint16_t)derivatives[1] << 16);
    packed[1] = (uint16_t)derivatives[2];
    packed[2] = 0;
    packed[3] = 0;
    _Alignas(16) uint16_t squared[8];
    unsigned previous_shift;
    // ESP-IDF manages the AI coprocessor context for tasks. Keep q0 local to
    // this block and restore the shared scalar shift register before returning.
    __asm__ volatile (
        "rsr.sar %[old]\n\t"
        "wsr.sar %[shift]\n\t"
        "rsync\n\t"
        "ld.qr q0, %[input], 0\n\t"
        "ee.vmul.s16 q0, q0, q0\n\t"
        "st.qr q0, %[output], 0\n\t"
        "wsr.sar %[old]\n\t"
        "rsync\n\t"
        : [old] "=&a" (previous_shift)
        : [input] "a" (packed), [output] "a" (squared), [shift] "a" (PT_SQUARE_SHIFT)
        : "memory");
    for (unsigned lead = 0; lead < PT_TRIPLET_LEADS; ++lead) {
        energies[lead] = squared[lead];
    }
}

enum {
    BLOCK_TRIPLETS = 64,
    DERIVATIVE_VALUES = 2 * PT_DERIVATIVE_LIMIT + 1,
};

// Keep the loop and indirect call identical for the two kernels.
static __attribute__((noipa)) uint32_t time_block(PtSquareTriplet *square, unsigned count,
                                                 int16_t inputs[][PT_TRIPLET_LEADS],
                                                 uint16_t outputs[][PT_TRIPLET_LEADS])
{
    uint32_t start = esp_cpu_get_cycle_count();
    for (unsigned i = 0; i < count; ++i) {
        square(inputs[i], outputs[i]);
    }
    return esp_cpu_get_cycle_count() - start;
}

bool benchmark_squaring(bool simd_first, SquareBenchmark *result)
{
    int16_t inputs[BLOCK_TRIPLETS][PT_TRIPLET_LEADS];
    uint16_t scalar[BLOCK_TRIPLETS][PT_TRIPLET_LEADS];
    uint16_t simd[BLOCK_TRIPLETS][PT_TRIPLET_LEADS];
    // Warm both full operations, including the first use of the coprocessor.
    const int16_t warmup[] = {-PT_DERIVATIVE_LIMIT, 0, PT_DERIVATIVE_LIMIT};
    pt_square_triplet_scalar(warmup, scalar[0]);
    pt_square_triplet_simd(warmup, simd[0]);
    if (memcmp(scalar[0], simd[0], sizeof(scalar[0])) != 0) {
        return false;
    }

    *result = (SquareBenchmark){.triplets = DERIVATIVE_VALUES};
    for (unsigned base = 0; base < DERIVATIVE_VALUES; base += BLOCK_TRIPLETS) {
        unsigned count = DERIVATIVE_VALUES - base;
        if (count > BLOCK_TRIPLETS) {
            count = BLOCK_TRIPLETS;
        }
        for (unsigned i = 0; i < count; ++i) {
            // Each lead visits the full domain, with different values in the lanes.
            for (unsigned lane = 0; lane < PT_TRIPLET_LEADS; ++lane) {
                unsigned offset = lane * (DERIVATIVE_VALUES / PT_TRIPLET_LEADS);
                inputs[i][lane] = (int)((base + i + offset) % DERIVATIVE_VALUES)
                                - PT_DERIVATIVE_LIMIT;
            }
        }
        if (simd_first) {
            result->simd_cycles += time_block(pt_square_triplet_simd, count, inputs, simd);
            result->scalar_cycles += time_block(pt_square_triplet_scalar, count, inputs, scalar);
        } else {
            result->scalar_cycles += time_block(pt_square_triplet_scalar, count, inputs, scalar);
            result->simd_cycles += time_block(pt_square_triplet_simd, count, inputs, simd);
        }
        if (memcmp(scalar, simd, count * sizeof(scalar[0])) != 0) {
            return false;
        }
    }
    return true;
}

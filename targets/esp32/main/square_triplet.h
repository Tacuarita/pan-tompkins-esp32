#ifndef SQUARE_TRIPLET_H
#define SQUARE_TRIPLET_H

#include "pan_tompkins.h"

PtSquareTriplet pt_square_triplet_simd;

typedef struct {
    uint32_t scalar_cycles;
    uint32_t simd_cycles;
    unsigned triplets;
} SquareBenchmark;

bool benchmark_squaring(bool simd_first, SquareBenchmark *result);

#endif

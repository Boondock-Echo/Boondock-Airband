/*
 * demod_math.cpp
 * Mathematical utilities for demodulation
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "demod_math.h"
#include "config.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_4
#define M_PI_4 (M_PI / 4.0)
#endif

#ifndef M_1_PI
#define M_1_PI 0.31830988618379067154
#endif

void multiply(float ar, float aj, float br, float bj, float* cr, float* cj) {
    *cr = ar * br - aj * bj;
    *cj = aj * br + ar * bj;
}

#ifdef NFM
float fast_atan2(float y, float x) {
    float yabs, angle;
    float pi4 = (float)M_PI_4, pi34 = 3.0f * (float)M_PI_4;
    if (x == 0.0f && y == 0.0f) {
        return 0;
    }
    yabs = y;
    if (yabs < 0.0f) {
        yabs = -yabs;
    }
    if (x >= 0.0f) {
        angle = pi4 - pi4 * (x - yabs) / (x + yabs);
    } else {
        angle = pi34 - pi4 * (x + yabs) / (yabs - x);
    }
    if (y < 0.0f) {
        return -angle;
    }
    return angle;
}

float polar_disc_fast(float ar, float aj, float br, float bj) {
    float cr, cj;
    multiply(ar, aj, br, -bj, &cr, &cj);
    return (float)(fast_atan2(cj, cr) * M_1_PI);
}

float fm_quadri_demod(float ar, float aj, float br, float bj) {
    return (float)((br * aj - ar * bj) / (ar * ar + aj * aj + 1.0f) * M_1_PI);
}
#endif /* NFM */

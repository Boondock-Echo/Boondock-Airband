/*
 * demod_math.h
 * Mathematical utilities for demodulation
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _DEMOD_MATH_H
#define _DEMOD_MATH_H

void multiply(float ar, float aj, float br, float bj, float* cr, float* cj);

#ifdef NFM
float fast_atan2(float y, float x);
float polar_disc_fast(float ar, float aj, float br, float bj);
float fm_quadri_demod(float ar, float aj, float br, float bj);
#endif /* NFM */

#endif /* _DEMOD_MATH_H */

/*
 * demod_init.h
 * Demodulation initialization functions
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _DEMOD_INIT_H
#define _DEMOD_INIT_H

#include "boondock_airband.h"
#include "helper_functions.h"

void init_demod(demod_params_t* params, Signal* signal, int device_start, int device_end);
bool init_output(channel_t* channel, output_t* output);
void init_output_params(output_params_t* params, int device_start, int device_end, int mixer_start, int mixer_end);
int next_device(demod_params_t* params, int current);
int count_devices_running(void);

#endif /* _DEMOD_INIT_H */

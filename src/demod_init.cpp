/*
 * demod_init.cpp
 * Demodulation initialization functions
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "demod_init.h"
#include "boondock_airband.h"
#include <cassert>

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#else
#include <fftw3.h>
#endif /* WITH_BCM_VC */

#ifdef WITH_PULSEAUDIO
#include "pulse.h"
#endif /* WITH_PULSEAUDIO */

extern size_t fft_size;

void init_demod(demod_params_t* params, Signal* signal, int device_start, int device_end) {
    assert(params != NULL);
    assert(signal != NULL);

    params->mp3_signal = signal;
    params->device_start = device_start;
    params->device_end = device_end;

#ifndef WITH_BCM_VC
    params->fftin = fftwf_alloc_complex(fft_size);
    params->fftout = fftwf_alloc_complex(fft_size);
    params->fft = fftwf_plan_dft_1d(fft_size, params->fftin, params->fftout, FFTW_FORWARD, FFTW_MEASURE);
#endif /* WITH_BCM_VC */
}

bool init_output(channel_t* channel, output_t* output) {
    if (output->has_mp3_output) {
        output->lame = airlame_init(channel->mode, channel->highpass, channel->lowpass);
        output->lamebuf = (unsigned char*)malloc(sizeof(unsigned char) * LAMEBUF_SIZE);
    }
    if (output->type == O_ICECAST) {
        shout_setup((icecast_data*)(output->data), channel->mode);
    } else if (output->type == O_UDP_STREAM) {
        udp_stream_data* sdata = (udp_stream_data*)(output->data);
        if (!udp_stream_init(sdata, channel->mode, (size_t)WAVE_BATCH * sizeof(float), sdata->channel_id)) {
            return false;
        }
#ifdef WITH_PULSEAUDIO
    } else if (output->type == O_PULSE) {
        pulse_init();
        pulse_setup((pulse_data*)(output->data), channel->mode);
#endif /* WITH_PULSEAUDIO */
    }

    return true;
}

void init_output_params(output_params_t* params, int device_start, int device_end, int mixer_start, int mixer_end) {
    assert(params != NULL);

    params->mp3_signal = new Signal;
    params->device_start = device_start;
    params->device_end = device_end;
    params->mixer_start = mixer_start;
    params->mixer_end = mixer_end;
}

int next_device(demod_params_t* params, int current) {
    current++;
    if (current < params->device_end) {
        return current;
    }
    return params->device_start;
}

int count_devices_running() {
    extern device_t* devices;
    extern int device_count;
    int ret = 0;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].input->state == INPUT_RUNNING) {
            ret++;
        }
    }
    return ret;
}

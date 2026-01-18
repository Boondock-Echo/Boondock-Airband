/*
 * afc.h
 * Automatic Frequency Control
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _AFC_H
#define _AFC_H

#include "boondock_airband.h"

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#else
#include <fftw3.h>
#endif /* WITH_BCM_VC */

#ifdef AFC_LOGGING
#include "logging.h"
#endif /* AFC_LOGGING */

extern size_t fft_size;

class AFC {
   private:
    const status _prev_axcindicate;

#ifdef WITH_BCM_VC
    float square(const GPU_FFT_COMPLEX* fft_results, size_t index) {
        return fft_results[index].re * fft_results[index].re + fft_results[index].im * fft_results[index].im;
    }
#else
    float square(const fftwf_complex* fft_results, size_t index) {
        return fft_results[index][0] * fft_results[index][0] + fft_results[index][1] * fft_results[index][1];
    }
#endif /* WITH_BCM_VC */

    template <class FFT_RESULTS, int STEP>
    size_t check(const FFT_RESULTS* fft_results, const size_t base, const float base_value, unsigned char afc) {
        float threshold = 0;
        size_t bin;
        for (bin = base;; bin += STEP) {
            if (STEP < 0) {
                if (bin < -STEP)
                    break;

            } else if ((size_t)(bin + STEP) >= fft_size)
                break;

            const float value = square(fft_results, (size_t)(bin + STEP));
            if (value <= base_value)
                break;

            if (base == (size_t)bin) {
                threshold = (value - base_value) / (float)afc;
            } else {
                if ((value - base_value) < threshold)
                    break;

                threshold += threshold / 10.0;
            }
        }
        return bin;
    }

   public:
    AFC(device_t* dev, int index) : _prev_axcindicate(dev->channels[index].axcindicate) {}

    template <class FFT_RESULTS>
    void finalize(device_t* dev, int index, const FFT_RESULTS* fft_results) {
        channel_t* channel = &dev->channels[index];
        if (channel->afc == 0)
            return;

        const char axcindicate = channel->axcindicate;
        if (axcindicate != NO_SIGNAL && _prev_axcindicate == NO_SIGNAL) {
            const size_t base = dev->base_bins[index];
            const float base_value = square(fft_results, base);
            size_t bin = check<FFT_RESULTS, -1>(fft_results, base, base_value, channel->afc);
            if (bin == base)
                bin = check<FFT_RESULTS, 1>(fft_results, base, base_value, channel->afc);

            if (dev->bins[index] != bin) {
#ifdef AFC_LOGGING
                log(LOG_INFO, "AFC device=%d channel=%d: base=%zu prev=%zu now=%zu\n", dev->device, index, base, dev->bins[index], bin);
#endif /* AFC_LOGGING */
                dev->bins[index] = bin;
                if (bin > base)
                    channel->axcindicate = AFC_UP;
                else if (bin < base)
                    channel->axcindicate = AFC_DOWN;
            }
        } else if (axcindicate == NO_SIGNAL && _prev_axcindicate != NO_SIGNAL)
            dev->bins[index] = dev->base_bins[index];
    }
};

#endif /* _AFC_H */

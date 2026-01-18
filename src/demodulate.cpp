/*
 * demodulate.cpp
 * Main demodulation thread
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "config.h"
#include "boondock_airband.h"
#include "demodulate.h"
#include "demod_init.h"
#include "demod_math.h"
#include "afc.h"
#include "logging.h"
#include "helper_functions.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#include "hello_fft/mailbox.h"
#else
#include <fftw3.h>
#endif /* WITH_BCM_VC */

extern device_t* devices;
extern volatile int do_exit;
extern volatile int capture_enabled;
extern int devices_running;
extern int tui;
extern size_t fft_size, fft_size_log;

#ifdef NFM
extern fm_demod_algo fm_demod;
#endif /* NFM */

void* demodulate(void* params) {
    assert(params != NULL);
    demod_params_t* demod_params = (demod_params_t*)params;

    debug_print("Starting demod thread, devices %d:%d, signal %p\n", demod_params->device_start, demod_params->device_end, demod_params->mp3_signal);

    // initialize fft engine
#ifdef WITH_BCM_VC
    int mb = mbox_open();
    struct GPU_FFT* fft;
    int ret = gpu_fft_prepare(mb, fft_size_log, GPU_FFT_FWD, FFT_BATCH, &fft);
    switch (ret) {
        case -1:
            log(LOG_CRIT, "Unable to enable V3D. Please check your firmware is up to date.\n");
            error();
            break;
        case -2:
            log(LOG_CRIT, "log2_N=%d not supported. Try between 8 and 17.\n", fft_size_log);
            error();
            break;
        case -3:
            log(LOG_CRIT, "Out of memory. Try a smaller batch or increase GPU memory.\n");
            error();
            break;
    }
#else
    fftwf_complex* fftin = demod_params->fftin;
    fftwf_complex* fftout = demod_params->fftout;
#endif /* WITH_BCM_VC */

    float ALIGNED32 levels_u8[256], levels_s8[256];
    float* levels_ptr = NULL;

    for (int i = 0; i < 256; i++) {
        levels_u8[i] = (i - 127.5f) / 127.5f;
    }
    for (int16_t i = -127; i < 128; i++) {
        levels_s8[(uint8_t)i] = i / 128.0f;
    }

    // initialize fft window
    // blackman 7
    // the whole matrix is computed
#ifdef WITH_BCM_VC
    float ALIGNED32 window[fft_size * 2];
#else
    float ALIGNED32 window[fft_size];
#endif /* WITH_BCM_VC */

    const double a0 = 0.27105140069342f;
    const double a1 = 0.43329793923448f;
    const double a2 = 0.21812299954311f;
    const double a3 = 0.06592544638803f;
    const double a4 = 0.01081174209837f;
    const double a5 = 0.00077658482522f;
    const double a6 = 0.00001388721735f;

    for (size_t i = 0; i < fft_size; i++) {
        double x = a0 - (a1 * cos((2.0 * M_PI * i) / (fft_size - 1))) + (a2 * cos((4.0 * M_PI * i) / (fft_size - 1))) - (a3 * cos((6.0 * M_PI * i) / (fft_size - 1))) +
                   (a4 * cos((8.0 * M_PI * i) / (fft_size - 1))) - (a5 * cos((10.0 * M_PI * i) / (fft_size - 1))) + (a6 * cos((12.0 * M_PI * i) / (fft_size - 1)));
#ifdef WITH_BCM_VC
        window[i * 2] = window[i * 2 + 1] = (float)x;
#else
        window[i] = (float)x;
#endif /* WITH_BCM_VC */
    }

#ifdef DEBUG
    struct timeval ts, te;
    gettimeofday(&ts, NULL);
#endif /* DEBUG */
    // JSON status and health metrics removed - available via web interface instead
    size_t available;
    int device_num = demod_params->device_start;
    while (true) {
        if (do_exit) {
#ifdef WITH_BCM_VC
            log(LOG_INFO, "Freeing GPU memory\n");
            gpu_fft_release(fft);
#endif /* WITH_BCM_VC */
            return NULL;
        }

        device_t* dev = devices + device_num;

        pthread_mutex_lock(&dev->input->buffer_lock);
        if (dev->input->bufe >= dev->input->bufs)
            available = dev->input->bufe - dev->input->bufs;
        else
            available = dev->input->buf_size - dev->input->bufs + dev->input->bufe;
        pthread_mutex_unlock(&dev->input->buffer_lock);

        // Check if capture is enabled
        if (!capture_enabled) {
            SLEEP(100);  // Sleep briefly when capture is disabled
            continue;
        }

        if (devices_running == 0) {
            log(LOG_ERR, "All receivers failed, exiting\n");
            do_exit = 1;
            continue;
        }

        if (dev->input->state != INPUT_RUNNING) {
            if (dev->input->state == INPUT_FAILED) {
                dev->input->state = INPUT_DISABLED;
                disable_device_outputs(dev);
                devices_running--;
            }
            device_num = next_device(demod_params, device_num);
            continue;
        }

        // number of input bytes per output wave sample (x 2 for I and Q)
        size_t bps = 2 * dev->input->bytes_per_sample * (size_t)round((double)dev->input->sample_rate / (double)WAVE_RATE);
        if (available < bps * FFT_BATCH + fft_size * dev->input->bytes_per_sample * 2) {
            // move to next device
            device_num = next_device(demod_params, device_num);
            SLEEP(10);
            continue;
        }

        if (dev->input->sfmt == SFMT_S16) {
            float const scale = 1.0f / dev->input->fullscale;
#ifdef WITH_BCM_VC
            struct GPU_FFT_COMPLEX* ptr = fft->in;
            for (size_t b = 0; b < FFT_BATCH; b++, ptr += fft->step) {
                short* buf2 = (short*)(dev->input->buffer + dev->input->bufs + b * bps);
                for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                    ptr[i].re = scale * (float)buf2[0] * window[i * 2];
                    ptr[i].im = scale * (float)buf2[1] * window[i * 2];
                }
            }
#else
            short* buf2 = (short*)(dev->input->buffer + dev->input->bufs);
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = scale * (float)buf2[0] * window[i];
                fftin[i][1] = scale * (float)buf2[1] * window[i];
            }
#endif /* WITH_BCM_VC */
        } else if (dev->input->sfmt == SFMT_F32) {
            float const scale = 1.0f / dev->input->fullscale;
#ifdef WITH_BCM_VC
            struct GPU_FFT_COMPLEX* ptr = fft->in;
            for (size_t b = 0; b < FFT_BATCH; b++, ptr += fft->step) {
                float* buf2 = (float*)(dev->input->buffer + dev->input->bufs + b * bps);
                for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                    ptr[i].re = scale * buf2[0] * window[i * 2];
                    ptr[i].im = scale * buf2[1] * window[i * 2];
                }
            }
#else  // WITH_BCM_VC
            float* buf2 = (float*)(dev->input->buffer + dev->input->bufs);
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = scale * buf2[0] * window[i];
                fftin[i][1] = scale * buf2[1] * window[i];
            }
#endif /* WITH_BCM_VC */

        } else {  // S8 or U8
            levels_ptr = (dev->input->sfmt == SFMT_U8 ? levels_u8 : levels_s8);

#ifdef WITH_BCM_VC
            sample_fft_arg sfa = {fft_size / 4, fft->in};
            for (size_t i = 0; i < FFT_BATCH; i++) {
                samplefft(&sfa, dev->input->buffer + dev->input->bufs + i * bps, window, levels_ptr);
                sfa.dest += fft->step;
            }
#else
            unsigned char* buf2 = dev->input->buffer + dev->input->bufs;
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = levels_ptr[buf2[0]] * window[i];
                fftin[i][1] = levels_ptr[buf2[1]] * window[i];
            }
#endif /* WITH_BCM_VC */
        }

#ifdef WITH_BCM_VC
        gpu_fft_execute(fft);
#else
        fftwf_execute(demod_params->fft);
#endif /* WITH_BCM_VC */

        // Update spectrum analyzer data (only update periodically to reduce overhead)
        dev->spectrum.update_counter++;
        if (dev->spectrum.enabled && (dev->spectrum.update_counter % 4 == 0)) {  // Update every 4th FFT
            pthread_mutex_lock(&dev->spectrum.mutex);
            // Store full bandwidth: map FFT bins to frequency range [center - sample_rate/2, center + sample_rate/2]
            // FFT output is: [DC, +f1, +f2, ..., +Nyquist, -fN, ..., -f1] (for real signals, symmetric)
            // We want to display: [center - sample_rate/2, ..., center, ..., center + sample_rate/2]
            // So we map: spectrum[i] = FFT[(i + fft_size/2) % fft_size] for i = 0 to fft_size-1
#ifdef WITH_BCM_VC
            const GPU_FFT_COMPLEX* fftout = fft->out;
            for (size_t i = 0; i < dev->spectrum.size; i++) {
                // Map to FFT bin: shift by fft_size/2 to center DC at middle of array
                size_t bin_idx = (i + fft_size / 2) % fft_size;
                float mag = sqrtf(fftout[bin_idx].re * fftout[bin_idx].re + fftout[bin_idx].im * fftout[bin_idx].im);
                // Convert to dB, with minimum floor
                dev->spectrum.magnitude[i] = 20.0f * log10f(mag + 1e-10f);
            }
#else
            for (size_t i = 0; i < dev->spectrum.size; i++) {
                // Map to FFT bin: shift by fft_size/2 to center DC at middle of array
                size_t bin_idx = (i + fft_size / 2) % fft_size;
                float mag = sqrtf(fftout[bin_idx][0] * fftout[bin_idx][0] + fftout[bin_idx][1] * fftout[bin_idx][1]);
                // Convert to dB, with minimum floor
                dev->spectrum.magnitude[i] = 20.0f * log10f(mag + 1e-10f);
            }
#endif /* WITH_BCM_VC */
            dev->spectrum.last_update = time(NULL);
            pthread_mutex_unlock(&dev->spectrum.mutex);
        }

#ifdef WITH_BCM_VC
        for (int i = 0; i < dev->channel_count; i++) {
            float* wavein = dev->channels[i].wavein + dev->waveend;
            __builtin_prefetch(wavein, 1);
            const int bin = dev->bins[i];
            const GPU_FFT_COMPLEX* fftout = fft->out + bin;
            for (int j = 0; j < FFT_BATCH; j++, ++wavein, fftout += fft->step)
                *wavein = sqrtf(fftout->im * fftout->im + fftout->re * fftout->re);
        }
        for (int j = 0; j < dev->channel_count; j++) {
            if (dev->channels[j].needs_raw_iq) {
                struct GPU_FFT_COMPLEX* ptr = fft->out;
                for (int job = 0; job < FFT_BATCH; job++) {
                    dev->channels[j].iq_in[2 * (dev->waveend + job)] = ptr[dev->bins[j]].re;
                    dev->channels[j].iq_in[2 * (dev->waveend + job) + 1] = ptr[dev->bins[j]].im;
                    ptr += fft->step;
                }
            }
        }
#else
        for (int j = 0; j < dev->channel_count; j++) {
            dev->channels[j].wavein[dev->waveend] = sqrtf(fftout[dev->bins[j]][0] * fftout[dev->bins[j]][0] + fftout[dev->bins[j]][1] * fftout[dev->bins[j]][1]);
            if (dev->channels[j].needs_raw_iq) {
                dev->channels[j].iq_in[2 * dev->waveend] = fftout[dev->bins[j]][0];
                dev->channels[j].iq_in[2 * dev->waveend + 1] = fftout[dev->bins[j]][1];
            }
        }
#endif /* WITH_BCM_VC */

        dev->waveend += FFT_BATCH;

        if (dev->waveend >= WAVE_BATCH + AGC_EXTRA) {
            for (int i = 0; i < dev->channel_count; i++) {
                AFC afc(dev, i);
                channel_t* channel = dev->channels + i;
                freq_t* fparms = channel->freqlist + channel->freq_idx;

                // set to NO_SIGNAL, will be updated to SIGNAL based on squelch below
                channel->axcindicate = NO_SIGNAL;

                for (int j = AGC_EXTRA; j < WAVE_BATCH + AGC_EXTRA; j++) {
                    float& real = channel->iq_in[2 * (j - AGC_EXTRA)];
                    float& imag = channel->iq_in[2 * (j - AGC_EXTRA) + 1];

                    fparms->squelch.process_raw_sample(channel->wavein[j]);

                    // If squelch is open / opening and using I/Q, then cleanup the signal and possibly update squelch.
                    if (fparms->squelch.should_filter_sample() && channel->needs_raw_iq) {
                        // remove phase rotation introduced by FFT sliding window
                        float swf, cwf, re_tmp, im_tmp;
                        sincosf_lut(channel->dm_phi, &swf, &cwf);
                        multiply(real, imag, cwf, -swf, &re_tmp, &im_tmp);
                        channel->dm_phi += channel->dm_dphi;
                        channel->dm_phi &= 0xffffff;

                        // apply lowpass filter, will be a no-op if not configured
                        fparms->lowpass_filter.apply(re_tmp, im_tmp);

                        // update I/Q and wave
                        real = re_tmp;
                        imag = im_tmp;
                        channel->wavein[j] = sqrt(real * real + imag * imag);

                        // update squelch post-cleanup
                        if (fparms->lowpass_filter.enabled()) {
                            fparms->squelch.process_filtered_sample(channel->wavein[j]);
                        }
                    }

                    if (fparms->modulation == MOD_AM) {
                        // if squelch is just opening then bootstrip agcavgfast with prior values of wavein
                        if (fparms->squelch.first_open_sample()) {
                            for (int k = j - AGC_EXTRA; k < j; k++) {
                                if (channel->wavein[k] >= fparms->squelch.squelch_level()) {
                                    fparms->agcavgfast = fparms->agcavgfast * 0.9f + channel->wavein[k] * 0.1f;
                                }
                            }
                        }
                        // if squelch is just closing then fade out the prior samples of waveout
                        else if (fparms->squelch.last_open_sample()) {
                            for (int k = j - AGC_EXTRA + 1; k < j; k++) {
                                channel->waveout[k] = channel->waveout[k - 1] * 0.94f;
                            }
                        }
                    }

                    float& waveout = channel->waveout[j];

                    // If squelch sees power then do modulation-specific processing
                    if (fparms->squelch.should_process_audio()) {
                        if (fparms->modulation == MOD_AM) {
                            if (channel->wavein[j] > fparms->squelch.squelch_level()) {
                                fparms->agcavgfast = fparms->agcavgfast * 0.995f + channel->wavein[j] * 0.005f;
                            }

                            waveout = (channel->wavein[j - AGC_EXTRA] - fparms->agcavgfast) / (fparms->agcavgfast * 1.5f);
                            if (abs(waveout) > 0.8f) {
                                waveout *= 0.85f;
                                fparms->agcavgfast *= 1.15f;
                            }
                        }
#ifdef NFM
                        else if (fparms->modulation == MOD_NFM) {
                            // FM demod
                            if (fm_demod == FM_FAST_ATAN2) {
                                waveout = polar_disc_fast(real, imag, channel->pr, channel->pj);
                            } else if (fm_demod == FM_QUADRI_DEMOD) {
                                waveout = fm_quadri_demod(real, imag, channel->pr, channel->pj);
                            }
                            channel->pr = real;
                            channel->pj = imag;

                            // de-emphasis IIR + DC blocking
                            fparms->agcavgfast = fparms->agcavgfast * 0.995f + waveout * 0.005f;
                            waveout -= fparms->agcavgfast;
                            waveout = waveout * (1.0f - channel->alpha) + channel->prev_waveout * channel->alpha;

                            // save off waveout before notch and ampfactor
                            channel->prev_waveout = waveout;
                        }
#endif /* NFM */

                        // process audio sample for CTCSS, will be no-op if not configured
                        fparms->squelch.process_audio_sample(waveout);
                    }

                    // If squelch is still open then save samples to output
                    if (fparms->squelch.is_open()) {
                        // apply the notch filter, will be a no-op if not configured
                        fparms->notch_filter.apply(waveout);

                        // apply the ampfactor
                        waveout *= fparms->ampfactor;

                        // make sure the value is between +/- 1 (requirement for libmp3lame)
                        if (std::isnan(waveout)) {
                            waveout = 0.0;
                        } else if (waveout > 1.0) {
                            waveout = 1.0;
                        } else if (waveout < -1.0) {
                            waveout = -1.0;
                        }

                        channel->axcindicate = SIGNAL;
                        if (channel->has_iq_outputs) {
                            channel->iq_out[2 * (j - AGC_EXTRA)] = real;
                            channel->iq_out[2 * (j - AGC_EXTRA) + 1] = imag;
                        }

                        // Squelch is closed
                    } else {
                        waveout = 0;
                        if (channel->has_iq_outputs) {
                            channel->iq_out[2 * (j - AGC_EXTRA)] = 0;
                            channel->iq_out[2 * (j - AGC_EXTRA) + 1] = 0;
                        }
                    }
                }
                memmove(channel->wavein, channel->wavein + WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float));
                if (channel->needs_raw_iq) {
                    memmove(channel->iq_in, channel->iq_in + 2 * WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float) * 2);
                }

#ifdef WITH_BCM_VC
                afc.finalize(dev, i, fft->out);
#else
                afc.finalize(dev, i, demod_params->fftout);
#endif /* WITH_BCM_VC */

                if (tui) {
                    char symbol = fparms->squelch.signal_outside_filter() ? '~' : (char)channel->axcindicate;
                    if (dev->mode == R_SCAN) {
                        GOTOXY(0, device_num * 17 + dev->row + 3);
                        printf("%4.0f/%3.0f%c %7.3f ", level_to_dBFS(fparms->squelch.signal_level()), level_to_dBFS(fparms->squelch.noise_level()), symbol,
                               (dev->channels[0].freqlist[channel->freq_idx].frequency / 1000000.0));
                    } else {
                        GOTOXY(i * 10, device_num * 17 + dev->row + 3);
                        printf("%4.0f/%3.0f%c ", level_to_dBFS(fparms->squelch.signal_level()), level_to_dBFS(fparms->squelch.noise_level()), symbol);
                    }
                    fflush(stdout);
                }

                if (channel->axcindicate != NO_SIGNAL) {
                    channel->freqlist[channel->freq_idx].active_counter++;
                }
            }
            if (dev->waveavail == 1) {
                debug_print("devices[%d]: output channel overrun\n", device_num);
                dev->output_overrun_count++;
            } else {
                dev->waveavail = 1;
            }
            dev->waveend -= WAVE_BATCH;
#ifdef DEBUG
            gettimeofday(&te, NULL);
            debug_bulk_print("waveavail %lu.%lu %lu\n", te.tv_sec, (unsigned long)te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
            ts.tv_sec = te.tv_sec;
            ts.tv_usec = te.tv_usec;
#endif /* DEBUG */
            
            // JSON status and health metrics are now available via web interface
            // No need to print to terminal - reduces verbosity
            
            demod_params->mp3_signal->send();
            dev->row++;
            if (dev->row == 12) {
                dev->row = 0;
            }
        }

        dev->input->bufs = (dev->input->bufs + bps * FFT_BATCH) % dev->input->buf_size;
        device_num = next_device(demod_params, device_num);
    }
}

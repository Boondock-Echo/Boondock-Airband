/*
 * signal_handling.cpp
 * Signal handling and process control
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
#include "logging.h"
#include "signal_handling.h"
#include <csignal>
#include <sys/time.h>
#include <unistd.h>

extern volatile int do_exit;
extern volatile int do_reload;
extern bool log_scan_activity;
extern device_t* devices;
extern int device_count;
extern size_t fft_size;

void sighandler(int sig) {
    // Async-signal-safe: only set flag, don't call log() or other non-safe functions
    // Use write() directly to stderr if we need to log (write is async-signal-safe)
    if (sig == SIGHUP) {
        // SIGHUP triggers configuration reload
        do_reload = 1;
        const char msg[] = "Got SIGHUP, reloading configuration...\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    } else {
        // Other signals cause exit
        const char msg[] = "Got signal, exiting...\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        do_exit = 1;
    }
}

void* controller_thread(void* params) {
    device_t* dev = (device_t*)params;
    int i = 0;
    int consecutive_squelch_off = 0;
    int new_centerfreq = 0;
    struct timeval tv;

    if (dev->channels[0].freq_count < 2)
        return 0;
    while (!do_exit) {
        SLEEP(200);
        if (dev->channels[0].axcindicate == NO_SIGNAL) {
            if (consecutive_squelch_off < 10) {
                consecutive_squelch_off++;
            } else {
                i++;
                i %= dev->channels[0].freq_count;
                dev->channels[0].freq_idx = i;
                new_centerfreq = dev->channels[0].freqlist[i].frequency + 20 * (double)(dev->input->sample_rate / fft_size);
                if (input_set_centerfreq(dev->input, new_centerfreq) < 0) {
                    break;
                }
            }
        } else {
            if (consecutive_squelch_off == 10) {
                if (log_scan_activity)
                    log(LOG_INFO, "Activity on %7.3f MHz\n", dev->channels[0].freqlist[i].frequency / 1000000.0);
                if (i != dev->last_frequency) {
                    // squelch has just opened on a new frequency - we might need to update outputs' metadata
                    gettimeofday(&tv, NULL);
                    tag_queue_put(dev, i, tv);
                    dev->last_frequency = i;
                }
            }
            consecutive_squelch_off = 0;
        }
    }
    return 0;
}

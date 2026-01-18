/*
 * RTLSDR AM/NFM demodulator, mixer, streamer and recorder
 *
 * Copyright (c) 2014 Wong Man Hang <microtony@gmail.com>
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#if defined WITH_BCM_VC && !defined __arm__
#error Broadcom VideoCore support can only be enabled on ARM builds
#endif

// From this point we may safely assume that WITH_BCM_VC implies __arm__

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#include "hello_fft/mailbox.h"
#endif /* WITH_BCM_VC */

#include <fcntl.h>
#include <lame/lame.h>
#include <ogg/ogg.h>
#include <pthread.h>
#include <shout/shout.h>
#include <stdint.h>  // uint8_t
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <vorbis/vorbisenc.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <libconfig.h++>
#include "input-common.h"
#include "logging.h"
#include "boondock_airband.h"
#include "squelch.h"
#include "web_server.h"
#include "helper_functions.h"
#include "signal_handling.h"
#include "demod_math.h"
#include "afc.h"
#include "demod_init.h"
#include "demodulate.h"
#include "config_utils.h"

#ifdef WITH_PROFILING
#include "gperftools/profiler.h"
#endif /* WITH_PROFILING */

using namespace std;
using namespace libconfig;

device_t* devices;
mixer_t* mixers;
int device_count, mixer_count;
int devices_running = 0;
int tui = 0;  // do not display textual user interface
int shout_metadata_delay = 3;
volatile int do_exit = 0;
volatile int do_reload = 0;  // Signal to reload configuration
volatile int capture_enabled = 1;  // Capture process enabled by default
bool use_localtime = false;
bool multiple_demod_threads = false;
bool multiple_output_threads = false;
bool log_scan_activity = false;
char* stats_filepath = NULL;
size_t fft_size_log = DEFAULT_FFT_SIZE_LOG;
size_t fft_size = 1 << fft_size_log;
int file_chunk_duration_minutes = 60;  // Default: 60 minutes

#ifdef NFM
float alpha = exp(-1.0f / (WAVE_RATE * 2e-4));
fm_demod_algo fm_demod = FM_FAST_ATAN2;
#endif /* NFM */

#ifdef DEBUG
char* debug_path;
#endif /* DEBUG */

int main(int argc, char* argv[]) {
    // Check for --capture flag first
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--capture") == 0) {
            return capture_main(argc, argv);
        }
    }
    // Default: run capture process (which includes web server as a thread)
    // This ensures web server can access devices/spectrum data
    return capture_main(argc, argv);
}

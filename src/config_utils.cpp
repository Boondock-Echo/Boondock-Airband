/*
 * config_utils.cpp
 * Configuration file utilities
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "config_utils.h"
#include "config.h"
#include "boondock_airband.h"
#include "web_server.h"
#include "signal_handling.h"
#include "demod_init.h"
#include "demodulate.h"
#include "logging.h"
#include "helper_functions.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <getopt.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <csignal>
#include <pthread.h>
#include <libconfig.h++>
#include <lame/lame.h>
#include <shout/shout.h>

#ifdef WITH_PROFILING
#include "gperftools/profiler.h"
#endif /* WITH_PROFILING */

#ifdef WITH_PULSEAUDIO
#include "pulse.h"
#endif /* WITH_PULSEAUDIO */

using namespace std;
using namespace libconfig;

void usage() {
    cout << "Usage: boondock_airband [options] [-c <config_file_path>]\n\
\t-h\t\t\tDisplay this help text\n\
\t-f\t\t\tRun in foreground, display textual waterfalls\n\
\t-F\t\t\tRun in foreground, do not display waterfalls (for running as a systemd service)\n";
#ifdef NFM
    cout << "\t-Q\t\t\tUse quadri correlator for FM demodulation (default is atan2)\n";
#endif /* NFM */
#ifdef DEBUG
    cout << "\t-d <file>\t\tLog debugging information to <file> (default is " << DEBUG_PATH << ")\n";
#endif /* DEBUG */
    cout << "\t-e\t\t\tPrint messages to standard error (disables syslog logging)\n";
    cout << "\t-c <config_file_path>\tUse non-default configuration file\n\t\t\t\t(default: " << CFGFILE << ")\n\
\t-p <port>\t\tWeb interface port (default: 5000)\n\
\t-v\t\t\tDisplay version and exit\n";
    exit(EXIT_SUCCESS);
}

// Create default config file with SoapySDR Airspy and NOAA channels
bool create_default_config(const char* config_path) {
    FILE* f = fopen(config_path, "w");
    if (!f) {
        cerr << "Failed to create default config file " << config_path << ": " << strerror(errno) << "\n";
        return false;
    }
    
    fprintf(f, "# Default Boondock Airband configuration\n");
    fprintf(f, "# Generated automatically - modify as needed\n\n");
    fprintf(f, "fft_size = 2048;\n");
    fprintf(f, "localtime = false;\n");
    fprintf(f, "file_chunk_duration_minutes = 5;\n\n");
    fprintf(f, "devices:\n");
    fprintf(f, "(\n");
    fprintf(f, "  {\n");
    fprintf(f, "    type = \"soapysdr\";\n");
    fprintf(f, "    device_string = \"driver=airspy\";\n");
    fprintf(f, "    gain = \"LNA=12,MIX=10,VGA=10\";\n");
    fprintf(f, "    centerfreq = 162.47500;\n");
    fprintf(f, "    correction = 0;\n");
    fprintf(f, "    sample_rate = 10.0;\n");
    fprintf(f, "    channels:\n");
    fprintf(f, "    (\n");
    
    // NOAA channels: 162.400, 162.425, 162.450, 162.475, 162.500, 162.525, 162.550
    const double noaa_freqs[] = {162.40000, 162.42500, 162.45000, 162.47500, 162.50000, 162.52500, 162.55000};
    const char* noaa_labels[] = {"NOAA 162.400", "NOAA 162.425", "NOAA 162.450", "NOAA 162.475", 
                                  "NOAA 162.500", "NOAA 162.525", "NOAA 162.550"};
    int num_channels = sizeof(noaa_freqs) / sizeof(noaa_freqs[0]);
    
    for (int i = 0; i < num_channels; i++) {
        fprintf(f, "      {\n");
        fprintf(f, "        freq = %.5f;\n", noaa_freqs[i]);
        fprintf(f, "        label = \"%s\";\n", noaa_labels[i]);
        fprintf(f, "        modulation = \"nfm\";\n");
        fprintf(f, "        bandwidth = 12000;\n");
        fprintf(f, "        outputs:\n");
        fprintf(f, "        (\n");
        fprintf(f, "          {\n");
        fprintf(f, "            type = \"file\";\n");
        fprintf(f, "            directory = \"recordings/%s\";\n", noaa_labels[i]);
        fprintf(f, "            filename_template = \"%s\";\n", noaa_labels[i]);
        fprintf(f, "            continuous = true;\n");
        fprintf(f, "            include_freq = true;\n");
        fprintf(f, "            dated_subdirectories = true;\n");
        fprintf(f, "          }\n");
        fprintf(f, "        );\n");
        fprintf(f, "      }%s\n", (i < num_channels - 1) ? "," : "");
    }
    
    fprintf(f, "    );\n");
    fprintf(f, "  }\n");
    fprintf(f, ");\n");
    
    fclose(f);
    return true;
}

// Get the directory where the executable is located
static char* get_executable_dir() {
    static char exe_path[1024] = {0};
    static char* dir_path = NULL;
    
    if (dir_path != NULL) {
        return dir_path;  // Already computed
    }
    
    // Try to get executable path
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        // Find last '/' and null-terminate there to get directory
        char* last_slash = strrchr(exe_path, '/');
        if (last_slash != NULL) {
            *last_slash = '\0';
            dir_path = strdup(exe_path);
            return dir_path;
        }
    }
    
    // Fallback: use current directory
    dir_path = strdup(".");
    return dir_path;
}

int capture_main(int argc, char* argv[]) {
#ifdef WITH_PROFILING
    ProfilerStart("boondock_airband.prof");
#endif /* WITH_PROFILING */

    char* cfgfile = NULL;
    char* cfgfile_allocated = NULL;  // Track if we allocated cfgfile dynamically
    char local_config_path[1024];
    
    // Default to local config file in the same directory as the executable
    char* exe_dir = get_executable_dir();
    snprintf(local_config_path, sizeof(local_config_path), "%s/boondock_airband.conf", exe_dir);
    cfgfile = local_config_path;  // Default to local config

    int opt;
    char optstring[16] = "efFhvc:";

#ifdef NFM
    strcat(optstring, "Q");
#endif /* NFM */

#ifdef DEBUG
    strcat(optstring, "d:");
#endif /* DEBUG */

    int foreground = 0;  // daemonize
    int do_syslog = 1;

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
#ifdef NFM
            case 'Q':
                fm_demod = FM_QUADRI_DEMOD;
                break;
#endif /* NFM */

#ifdef DEBUG
            case 'd':
                debug_path = strdup(optarg);
                break;
#endif /* DEBUG */

            case 'e':
                do_syslog = 0;
                break;
            case 'f':
                foreground = 1;
                tui = 1;
                break;
            case 'F':
                foreground = 1;
                tui = 0;
                break;
            case 'c':
                cfgfile = optarg;
                // If user specified a config file, we don't need to track allocation
                cfgfile_allocated = NULL;
                break;
            case 'v':
                cout << "Boondock-Airband version " << BOONDOCK_AIRBAND_VERSION << "\n";
                exit(EXIT_SUCCESS);
            case 'h':
            default:
                usage();
                break;
        }
    }
    
    // If no config file was specified, use local config in executable directory
    // cfgfile is already set to local_config_path by default
    
#ifdef DEBUG
    if (!debug_path)
        debug_path = strdup(DEBUG_PATH);
    init_debug(debug_path);
#endif /* DEBUG */

    // If executing other than as root, GPU memory gets alloc'd and the
    // 'permission denied' message on /dev/mem kills boondock_airband without
    // releasing GPU memory.
#ifdef WITH_BCM_VC
    if (0 != getuid()) {
        cerr << "FFT library requires that boondock_airband be executed as root\n";
        exit(1);
    }
#endif /* WITH_BCM_VC */

    // Check if config file exists, create default if it doesn't (only for local config)
    if (cfgfile == local_config_path && access(cfgfile, F_OK) != 0) {
        log(LOG_INFO, "Configuration file %s not found, attempting to create default configuration\n", cfgfile);
        if (!create_default_config(cfgfile)) {
            log(LOG_WARNING, "Cannot create config file in %s. Continuing anyway - you may need to create a config file manually.\n", cfgfile);
        } else {
            log(LOG_INFO, "Created default configuration file at %s\n", cfgfile);
        }
    }

    // read config
    try {
        Config config;
        config.readFile(cfgfile);
        Setting& root = config.getRoot();
        if (root.exists("fft_size")) {
            int fsize = (int)(root["fft_size"]);
            fft_size_log = 0;
            for (size_t i = MIN_FFT_SIZE_LOG; i <= MAX_FFT_SIZE_LOG; i++) {
                if (fsize == 1 << i) {
                    fft_size = (size_t)fsize;
                    fft_size_log = i;
                    break;
                }
            }
            if (fft_size_log == 0) {
                cerr << "Configuration error: invalid fft_size value (must be a power of two in range " << (1 << MIN_FFT_SIZE_LOG) << "-" << (1 << MAX_FFT_SIZE_LOG) << ")\n";
                error();
            }
        }
        if (root.exists("shout_metadata_delay"))
            shout_metadata_delay = (int)(root["shout_metadata_delay"]);
        if (shout_metadata_delay < 0 || shout_metadata_delay > 2 * TAG_QUEUE_LEN) {
            cerr << "Configuration error: shout_metadata_delay is out of allowed range (0-" << 2 * TAG_QUEUE_LEN << ")\n";
            error();
        }
        if (root.exists("localtime") && (bool)root["localtime"] == true)
            use_localtime = true;
        if (root.exists("multiple_demod_threads") && (bool)root["multiple_demod_threads"] == true) {
#ifdef WITH_BCM_VC
            cerr << "Using multiple_demod_threads not supported with BCM VideoCore for FFT\n";
            exit(1);
#endif /* WITH_BCM_VC */
            multiple_demod_threads = true;
        }
        if (root.exists("multiple_output_threads") && (bool)root["multiple_output_threads"] == true) {
            multiple_output_threads = true;
        }
        if (root.exists("log_scan_activity") && (bool)root["log_scan_activity"] == true)
            log_scan_activity = true;
        if (root.exists("stats_filepath"))
            stats_filepath = strdup(root["stats_filepath"]);
        if (root.exists("file_chunk_duration_minutes"))
            file_chunk_duration_minutes = (int)(root["file_chunk_duration_minutes"]);
#ifdef NFM
        if (root.exists("tau"))
            alpha = ((int)root["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)root["tau"])));
#endif /* NFM */

        Setting& devs = config.lookup("devices");
        device_count = devs.getLength();
        if (device_count < 1) {
            cerr << "Configuration error: no devices defined\n";
            error();
        }

        struct sigaction sigact, pipeact;
        memset(&sigact, 0, sizeof(sigact));
        memset(&pipeact, 0, sizeof(pipeact));
        pipeact.sa_handler = SIG_IGN;
        sigact.sa_handler = &sighandler;
        sigaction(SIGPIPE, &pipeact, NULL);
        sigaction(SIGHUP, &sigact, NULL);
        sigaction(SIGINT, &sigact, NULL);
        sigaction(SIGQUIT, &sigact, NULL);
        sigaction(SIGTERM, &sigact, NULL);

        devices = (device_t*)XCALLOC(device_count, sizeof(device_t));
        shout_init();

        if (do_syslog) {
            openlog("boondock_airband", LOG_PID, LOG_DAEMON);
            log_destination = SYSLOG;
        } else if (foreground) {
            log_destination = STDERR;
        } else {
            log_destination = NONE;
        }

        if (root.exists("mixers")) {
            Setting& mx = config.lookup("mixers");
            mixers = (mixer_t*)XCALLOC(mx.getLength(), sizeof(struct mixer_t));
            if ((mixer_count = parse_mixers(mx)) > 0) {
                mixers = (mixer_t*)XREALLOC(mixers, mixer_count * sizeof(struct mixer_t));
            } else {
                free(mixers);
            }
        } else {
            mixer_count = 0;
        }

        uint32_t devs_enabled = parse_devices(devs);
        if (devs_enabled < 1) {
            cerr << "Configuration error: no devices defined\n";
            error();
        }
        device_count = devs_enabled;
    } catch (const FileIOException& e) {
        cerr << "Cannot read configuration file " << cfgfile << "\n";
        error();
    } catch (const ParseException& e) {
        cerr << "Error while parsing configuration file " << cfgfile << " line " << e.getLine() << ": " << e.getError() << "\n";
        error();
    } catch (const SettingNotFoundException& e) {
        cerr << "Configuration error: mandatory parameter missing: " << e.getPath() << "\n";
        error();
    } catch (const SettingTypeException& e) {
        cerr << "Configuration error: invalid parameter type: " << e.getPath() << "\n";
        error();
    } catch (const ConfigException& e) {
        cerr << "Unhandled config exception\n";
        error();
    }

    log(LOG_INFO, "Boondock-Airband version %s starting (capture mode)\n", BOONDOCK_AIRBAND_VERSION);

    if (!foreground) {
        int pid1, pid2;
        if ((pid1 = fork()) == -1) {
            cerr << "Cannot fork child process: " << strerror(errno) << "\n";
            error();
        }
        if (pid1) {
            waitpid(-1, NULL, 0);
            return (0);
        } else {
            if ((pid2 = fork()) == -1) {
                cerr << "Cannot fork child process: " << strerror(errno) << "\n";
                error();
            }
            if (pid2) {
                return (0);
            } else {
                int nullfd, dupfd;
                if ((nullfd = open("/dev/null", O_RDWR)) == -1) {
                    log(LOG_CRIT, "Cannot open /dev/null: %s\n", strerror(errno));
                    error();
                }
                for (dupfd = 0; dupfd <= 2; dupfd++) {
                    if (dup2(nullfd, dupfd) == -1) {
                        log(LOG_CRIT, "dup2(): %s\n", strerror(errno));
                        error();
                    }
                }
                if (nullfd > 2)
                    close(nullfd);
            }
        }
    }

    for (int i = 0; i < mixer_count; i++) {
        if (mixers[i].enabled == false) {
            continue;
        }
        channel_t* channel = &mixers[i].channel;
        for (int k = 0; k < channel->output_count; k++) {
            output_t* output = channel->outputs + k;
            if (!init_output(channel, output)) {
                cerr << "Failed to initialize mixer " << i << " output " << k << " - aborting\n";
                error();
            }
        }
    }
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (!init_output(channel, output)) {
                    cerr << "Failed to initialize device " << i << " channel " << j << " output " << k << " - aborting\n";
                    error();
                }
            }
        }
        if (input_init(dev->input) != 0 || dev->input->state != INPUT_INITIALIZED) {
            if (errno != 0) {
                cerr << "Failed to initialize input device " << i << ": " << strerror(errno) << " - aborting\n";
            } else {
                cerr << "Failed to initialize input device " << i << " - aborting\n";
            }
            error();
        }
        if (input_start(dev->input) != 0) {
            cerr << "Failed to start input on device " << i << ": " << strerror(errno) << " - aborting\n";
            error();
        }
        if (dev->mode == R_SCAN) {
            if (pthread_mutex_init(&dev->tag_queue_lock, NULL) != 0) {
                cerr << "Failed to initialize mutex - aborting\n";
                error();
            }
            pthread_create(&dev->controller_thread, NULL, &controller_thread, dev);
        }
    }

    int timeout = 50;  // 5 seconds
    while ((devices_running = count_devices_running()) != device_count && timeout > 0) {
        SLEEP(100);
        timeout--;
    }
    if ((devices_running = count_devices_running()) != device_count) {
        log(LOG_ERR, "%d device(s) failed to initialize - aborting\n", device_count - devices_running);
        error();
    }
    if (tui) {
        printf("\e[1;1H\e[2J");
        GOTOXY(0, 0);
        printf("                                                                               ");
        for (int i = 0; i < device_count; i++) {
            GOTOXY(0, i * 17 + 1);
            for (int j = 0; j < devices[i].channel_count; j++) {
                printf(" %7.3f  ", devices[i].channels[j].freqlist[devices[i].channels[j].freq_idx].frequency / 1000000.0);
            }
            if (i != device_count - 1) {
                GOTOXY(0, i * 17 + 16);
                printf("-------------------------------------------------------------------------------");
            }
        }
    }
    THREAD output_check;
    pthread_create(&output_check, NULL, &output_check_thread, NULL);

    int demod_thread_count = multiple_demod_threads ? device_count : 1;
    demod_params_t* demod_params = (demod_params_t*)XCALLOC(demod_thread_count, sizeof(demod_params_t));
    THREAD* demod_threads = (THREAD*)XCALLOC(demod_thread_count, sizeof(THREAD));

    int output_thread_count = 1;
    if (multiple_output_threads) {
        output_thread_count = demod_thread_count;
        if (mixer_count > 0) {
            output_thread_count++;
        }
    }
    output_params_t* output_params = (output_params_t*)XCALLOC(output_thread_count, sizeof(output_params_t));
    THREAD* output_threads = (THREAD*)XCALLOC(output_thread_count, sizeof(THREAD));

    // Setup the output and demod threads
    if (multiple_output_threads == false) {
        init_output_params(&output_params[0], 0, device_count, 0, mixer_count);
        if (multiple_demod_threads == false) {
            init_demod(&demod_params[0], output_params[0].mp3_signal, 0, device_count);
        } else {
            for (int i = 0; i < demod_thread_count; i++) {
                init_demod(&demod_params[i], output_params[0].mp3_signal, i, i + 1);
            }
        }
    } else {
        if (multiple_demod_threads == false) {
            init_output_params(&output_params[0], 0, device_count, 0, 0);
            init_demod(&demod_params[0], output_params[0].mp3_signal, 0, device_count);
        } else {
            for (int i = 0; i < device_count; i++) {
                init_output_params(&output_params[i], i, i + 1, 0, 0);
                init_demod(&demod_params[i], output_params[i].mp3_signal, i, i + 1);
            }
        }
        if (mixer_count > 0) {
            init_output_params(&output_params[output_thread_count - 1], 0, 0, 0, mixer_count);
        }
    }

    // Startup the output threads
    for (int i = 0; i < output_thread_count; i++) {
        pthread_create(&output_threads[i], NULL, &output_thread, &output_params[i]);
    }

    // Startup the mixer thread (if there is one)
    THREAD mixer;
    if (mixer_count > 0) {
        pthread_create(&mixer, NULL, &mixer_thread, output_params[output_thread_count - 1].mp3_signal);
    }

#ifdef WITH_PULSEAUDIO
    pulse_start();
#endif /* WITH_PULSEAUDIO */

    sincosf_lut_init();

    // Start web server as a thread (so it can access devices/spectrum data)
    // Default web port is 5000, but check config for web_port setting
    int web_port = 5000;
    try {
        Config config;
        config.readFile(cfgfile);
        Setting& root = config.getRoot();
        if (root.exists("web_port")) {
            web_port = (int)(root["web_port"]);
            if (web_port <= 0 || web_port > 65535) {
                web_port = 5000;  // Reset to default
            }
        }
        web_server_set_config_path(cfgfile);
    } catch (...) {
        // Use default port if config read fails
        web_server_set_config_path(cfgfile);
    }
    
    if (web_server_start(web_port) != 0) {
        log(LOG_WARNING, "Failed to start web server on port %d, continuing without web interface\n", web_port);
    } else {
        log(LOG_INFO, "Web server started on port %d\n", web_port);
    }

    // Startup the demod threads
    for (int i = 0; i < demod_thread_count; i++) {
        pthread_create(&demod_threads[i], NULL, &demodulate, &demod_params[i]);
    }

    // Wait for demod threads to exit
    for (int i = 0; i < demod_thread_count; i++) {
        pthread_join(demod_threads[i], NULL);
    }
    
    // Stop web server before cleanup
    web_server_stop();

    log(LOG_INFO, "Cleaning up\n");
    for (int i = 0; i < device_count; i++) {
        if (devices[i].mode == R_SCAN)
            pthread_join(devices[i].controller_thread, NULL);
        if (input_stop(devices[i].input) != 0 || devices[i].input->state != INPUT_STOPPED) {
            if (errno != 0) {
                log(LOG_ERR, "Failed do stop device #%d: %s\n", i, strerror(errno));
            } else {
                log(LOG_ERR, "Failed do stop device #%d\n", i);
            }
        }
    }
    log(LOG_INFO, "Input threads closed\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        disable_device_outputs(dev);
    }

    if (mixer_count > 0) {
        log(LOG_INFO, "Closing mixer thread\n");
        pthread_join(mixer, NULL);
    }

    log(LOG_INFO, "Closing output thread(s)\n");
    for (int i = 0; i < output_thread_count; i++) {
        output_params[i].mp3_signal->send();
        pthread_join(output_threads[i], NULL);
    }

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->lame) {
                    lame_close(output->lame);
                }
            }
        }
    }

    close_debug();
#ifdef WITH_PROFILING
    ProfilerStop();
#endif /* WITH_PROFILING */
    
    // Free allocated config file path if we created one
    if (cfgfile_allocated) {
        free(cfgfile_allocated);
    }
    
    return 0;
}

int web_server_main(int argc, char* argv[]) {
    char* cfgfile = NULL;
    char* cfgfile_allocated = NULL;  // Track if we allocated cfgfile dynamically
    char local_config_path[1024];
    
    // Default to local config file in the same directory as the executable
    char* exe_dir = get_executable_dir();
    snprintf(local_config_path, sizeof(local_config_path), "%s/boondock_airband.conf", exe_dir);
    cfgfile = local_config_path;  // Default to local config

    int opt;
    char optstring[16] = "efFhvc:p:";
    int do_syslog = 1;
    int web_port = 5000;  // Default web server port

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
            case 'e':
                do_syslog = 0;
                break;
            case 'f':
                // Run in foreground with TUI (not applicable to web server mode, but accept it)
                tui = 1;
                break;
            case 'F':
                // Run in foreground without TUI (not applicable to web server mode, but accept it)
                tui = 0;
                break;
            case 'c':
                cfgfile = optarg;
                // If user specified a config file, we don't need to track allocation
                cfgfile_allocated = NULL;
                break;
            case 'p':
                web_port = atoi(optarg);
                if (web_port <= 0 || web_port > 65535) {
                    cerr << "Invalid port number: " << optarg << "\n";
                    exit(EXIT_FAILURE);
                }
                break;
            case 'v':
                cout << "Boondock-Airband version " << BOONDOCK_AIRBAND_VERSION << "\n";
                exit(EXIT_SUCCESS);
            case 'h':
            default:
                usage();
                break;
        }
    }
    
    // cfgfile is already set to local_config_path by default
    // If -c was specified, it's already been set to optarg above

    // Setup signal handlers
    struct sigaction sigact;
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = &sighandler;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    // Setup logging
    if (do_syslog) {
        openlog("boondock_airband", LOG_PID, LOG_DAEMON);
        log_destination = SYSLOG;
    } else {
        log_destination = STDERR;
    }

    // Check if config file exists, create default if it doesn't
    if (access(cfgfile, F_OK) != 0) {
        log(LOG_INFO, "Configuration file %s not found, attempting to create default configuration\n", cfgfile);
        if (!create_default_config(cfgfile)) {
            // If we can't create in the default location, try a user-writable location
            char fallback_config[512];
            const char* home = getenv("HOME");
            if (home && strlen(home) > 0) {
                snprintf(fallback_config, sizeof(fallback_config), "%s/.boondock_airband.conf", home);
            } else {
                snprintf(fallback_config, sizeof(fallback_config), "/tmp/boondock_airband.conf");
            }
            
            log(LOG_WARNING, "Cannot create config file in %s (permission denied), trying %s\n", cfgfile, fallback_config);
            if (create_default_config(fallback_config)) {
                cfgfile_allocated = strdup(fallback_config);
                cfgfile = cfgfile_allocated;
                log(LOG_INFO, "Created default configuration file at %s\n", fallback_config);
            } else {
                log(LOG_WARNING, "Cannot create config file in default or fallback location. Web interface will work but you may need to create a config file manually.\n");
                // Continue anyway - web server can work without config, user can create it via web interface
            }
        } else {
            log(LOG_INFO, "Created default configuration file at %s\n", cfgfile);
        }
    }

    // Read config file to get web server port if specified
    // Store config path for web server (even if file doesn't exist yet)
    web_server_set_config_path(cfgfile);
    
    // Try to read config file if it exists
    if (access(cfgfile, F_OK) == 0) {
        try {
            Config config;
            config.readFile(cfgfile);
            Setting& root = config.getRoot();
            if (root.exists("web_port")) {
                web_port = (int)(root["web_port"]);
                if (web_port <= 0 || web_port > 65535) {
                    log(LOG_WARNING, "Configuration error: web_port must be between 1 and 65535, using default %d\n", web_port);
                    web_port = 5000;  // Reset to default
                }
            }
        } catch (const FileIOException& e) {
            log(LOG_WARNING, "Cannot read configuration file %s: %s\n", cfgfile, e.what());
            // Continue anyway - web server can work without full config
        } catch (const ParseException& e) {
            log(LOG_WARNING, "Error while parsing configuration file %s line %d: %s\n", cfgfile, e.getLine(), e.getError());
            // Continue anyway - web server can work without full config
        } catch (...) {
            // Continue anyway
        }
    } else {
        log(LOG_INFO, "Configuration file %s does not exist. Web interface will allow you to create one.\n", cfgfile);
    }

    log(LOG_INFO, "Boondock-Airband version %s starting (web server mode)\n", BOONDOCK_AIRBAND_VERSION);

    // Start web server
    if (web_server_start(web_port) != 0) {
        log(LOG_ERR, "Failed to start web server on port %d\n", web_port);
        return 1;
    }

    // Main loop - wait for exit signal
    while (!do_exit) {
        SLEEP(1000);  // Sleep 1 second
    }

    log(LOG_INFO, "Shutting down web server\n");
    web_server_stop();

    // Free allocated config file path if we created one
    if (cfgfile_allocated) {
        free(cfgfile_allocated);
    }

    return 0;
}

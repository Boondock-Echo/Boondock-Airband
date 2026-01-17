/*
 * config.cpp
 * Configuration parsing routines
 *
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

#include <assert.h>
#include <stdint.h>  // uint32_t
#include <syslog.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <libconfig.h++>
#include "input-common.h"  // input_t
#include "boondock_airband.h"
#include "helper_functions.h"

using namespace std;

static int parse_outputs(libconfig::Setting& outs, channel_t* channel, int i, int j, bool parsing_mixers) {
    int oo = 0;
    for (int o = 0; o < channel->output_count; o++) {
        channel->outputs[oo].has_mp3_output = false;
        channel->outputs[oo].lame = NULL;
        channel->outputs[oo].lamebuf = NULL;

        if (outs[o].exists("disable") && (bool)outs[o]["disable"] == true) {
            continue;
        }
        if (!strncmp(outs[o]["type"], "icecast", 7)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct icecast_data));
            channel->outputs[oo].type = O_ICECAST;
            icecast_data* idata = (icecast_data*)(channel->outputs[oo].data);
            idata->hostname = strdup(outs[o]["server"]);
            idata->port = outs[o]["port"];
            idata->mountpoint = strdup(outs[o]["mountpoint"]);
            idata->username = strdup(outs[o]["username"]);
            idata->password = strdup(outs[o]["password"]);
            if (outs[o].exists("name"))
                idata->name = strdup(outs[o]["name"]);
            if (outs[o].exists("genre"))
                idata->genre = strdup(outs[o]["genre"]);
            if (outs[o].exists("description"))
                idata->description = strdup(outs[o]["description"]);
            if (outs[o].exists("send_scan_freq_tags"))
                idata->send_scan_freq_tags = (bool)outs[o]["send_scan_freq_tags"];
            else
                idata->send_scan_freq_tags = 0;
#ifdef LIBSHOUT_HAS_TLS
            if (outs[o].exists("tls")) {
                if (outs[o]["tls"].getType() == libconfig::Setting::TypeString) {
                    if (!strcmp(outs[o]["tls"], "auto")) {
                        idata->tls_mode = SHOUT_TLS_AUTO;
                    } else if (!strcmp(outs[o]["tls"], "auto_no_plain")) {
                        idata->tls_mode = SHOUT_TLS_AUTO_NO_PLAIN;
                    } else if (!strcmp(outs[o]["tls"], "transport")) {
                        idata->tls_mode = SHOUT_TLS_RFC2818;
                    } else if (!strcmp(outs[o]["tls"], "upgrade")) {
                        idata->tls_mode = SHOUT_TLS_RFC2817;
                    } else if (!strcmp(outs[o]["tls"], "disabled")) {
                        idata->tls_mode = SHOUT_TLS_DISABLED;
                    } else {
                        if (parsing_mixers) {
                            cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
                        } else {
                            cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
                        }
                        cerr << "invalid value for tls; must be one of: auto, auto_no_plain, transport, upgrade, disabled\n";
                        error();
                    }
                } else {
                    if (parsing_mixers) {
                        cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
                    } else {
                        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
                    }
                    cerr << "tls value must be a string\n";
                    error();
                }
            } else {
                idata->tls_mode = SHOUT_TLS_DISABLED;
            }
#endif /* LIBSHOUT_HAS_TLS */

            channel->outputs[oo].has_mp3_output = true;
        } else if (!strncmp(outs[o]["type"], "file", 4)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct file_data));
            channel->outputs[oo].type = O_FILE;
            file_data* fdata = (file_data*)(channel->outputs[oo].data);

            fdata->type = O_FILE;
            if (!outs[o].exists("directory") || !outs[o].exists("filename_template")) {
                if (parsing_mixers) {
                    cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
                } else {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
                }
                cerr << "both directory and filename_template required for file\n";
                error();
            }
            fdata->basedir = outs[o]["directory"].c_str();
            fdata->basename = outs[o]["filename_template"].c_str();
            fdata->dated_subdirectories = outs[o].exists("dated_subdirectories") ? (bool)(outs[o]["dated_subdirectories"]) : false;
            fdata->suffix = ".mp3";

            fdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            fdata->append = (!outs[o].exists("append")) || (bool)(outs[o]["append"]);
            fdata->split_on_transmission = outs[o].exists("split_on_transmission") ? (bool)(outs[o]["split_on_transmission"]) : false;
            fdata->include_freq = outs[o].exists("include_freq") ? (bool)(outs[o]["include_freq"]) : false;
            fdata->device_index = i;
            fdata->channel_index = parsing_mixers ? -1 : j;  // -1 for mixers
            fdata->metadata_f = NULL;
            fdata->last_metadata_log_sec = 0;
            fdata->last_metadata_flush.tv_sec = 0;
            fdata->last_metadata_flush.tv_usec = 0;

            channel->outputs[oo].has_mp3_output = true;

            if (fdata->split_on_transmission) {
                if (parsing_mixers) {
                    cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: split_on_transmission is not allowed for mixers\n";
                    error();
                }
                if (fdata->continuous) {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: can't have both continuous and split_on_transmission\n";
                    error();
                }
            }

        } else if (!strncmp(outs[o]["type"], "rawfile", 7)) {
            if (parsing_mixers) {  // rawfile outputs not allowed for mixers
                cerr << "Configuration error: mixers.[" << i << "] outputs[" << o << "]: rawfile output is not allowed for mixers\n";
                error();
            }
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct file_data));
            channel->outputs[oo].type = O_RAWFILE;
            file_data* fdata = (file_data*)(channel->outputs[oo].data);

            fdata->type = O_RAWFILE;
            if (!outs[o].exists("directory") || !outs[o].exists("filename_template")) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: both directory and filename_template required for file\n";
                error();
            }

            fdata->basedir = outs[o]["directory"].c_str();
            fdata->basename = outs[o]["filename_template"].c_str();
            fdata->dated_subdirectories = outs[o].exists("dated_subdirectories") ? (bool)(outs[o]["dated_subdirectories"]) : false;
            fdata->suffix = ".cf32";

            fdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            fdata->append = (!outs[o].exists("append")) || (bool)(outs[o]["append"]);
            fdata->split_on_transmission = outs[o].exists("split_on_transmission") ? (bool)(outs[o]["split_on_transmission"]) : false;
            fdata->include_freq = outs[o].exists("include_freq") ? (bool)(outs[o]["include_freq"]) : false;
            fdata->device_index = i;
            fdata->channel_index = j;
            fdata->metadata_f = NULL;
            fdata->last_metadata_log_sec = 0;
            fdata->last_metadata_flush.tv_sec = 0;
            fdata->last_metadata_flush.tv_usec = 0;
            channel->needs_raw_iq = channel->has_iq_outputs = 1;

            if (fdata->continuous && fdata->split_on_transmission) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: can't have both continuous and split_on_transmission\n";
                error();
            }
        } else if (!strncmp(outs[o]["type"], "mixer", 5)) {
            if (parsing_mixers) {  // mixer outputs not allowed for mixers
                cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: mixer output is not allowed for mixers\n";
                error();
            }
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct mixer_data));
            channel->outputs[oo].type = O_MIXER;
            mixer_data* mdata = (mixer_data*)(channel->outputs[oo].data);
            const char* name = (const char*)outs[o]["name"];
            if ((mdata->mixer = getmixerbyname(name)) == NULL) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: unknown mixer \"" << name << "\"\n";
                error();
            }
            float ampfactor = outs[o].exists("ampfactor") ? (float)outs[o]["ampfactor"] : 1.0f;
            float balance = outs[o].exists("balance") ? (float)outs[o]["balance"] : 0.0f;
            if (balance < -1.0f || balance > 1.0f) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: balance out of allowed range <-1.0;1.0>\n";
                error();
            }
            if ((mdata->input = mixer_connect_input(mdata->mixer, ampfactor, balance)) < 0) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o
                     << "]: "
                        "could not connect to mixer "
                     << name << ": " << mixer_get_error() << "\n";
                error();
            }
            debug_print("dev[%d].chan[%d].out[%d] connected to mixer %s as input %d (ampfactor=%.1f balance=%.1f)\n", i, j, o, name, mdata->input, ampfactor, balance);
        } else if (!strncmp(outs[o]["type"], "udp_stream", 6)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct udp_stream_data));
            channel->outputs[oo].type = O_UDP_STREAM;

            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[oo].data;

            sdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            sdata->enable_headers = outs[o].exists("udp_headers") ? (bool)(outs[o]["udp_headers"]) : false;
            sdata->enable_chunking = outs[o].exists("udp_chunking") ? (bool)(outs[o]["udp_chunking"]) : true;
            // For devices: j is channel index, for mixers: i is mixer index
            sdata->channel_id = parsing_mixers ? i : j;

            if (outs[o].exists("dest_address")) {
                sdata->dest_address = strdup(outs[o]["dest_address"]);
            } else {
                if (parsing_mixers) {
                    cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
                } else {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
                }
                cerr << "missing dest_address\n";
                error();
            }

            if (outs[o].exists("dest_port")) {
                if (outs[o]["dest_port"].getType() == libconfig::Setting::TypeInt) {
                    char buffer[12];
                    sprintf(buffer, "%d", (int)outs[o]["dest_port"]);
                    sdata->dest_port = strdup(buffer);
                } else {
                    sdata->dest_port = strdup(outs[o]["dest_port"]);
                }
            } else {
                if (parsing_mixers) {
                    cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
                } else {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
                }
                cerr << "missing dest_port\n";
                error();
            }
#ifdef WITH_PULSEAUDIO
        } else if (!strncmp(outs[o]["type"], "pulse", 5)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct pulse_data));
            channel->outputs[oo].type = O_PULSE;

            pulse_data* pdata = (pulse_data*)(channel->outputs[oo].data);
            pdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            pdata->server = outs[o].exists("server") ? strdup(outs[o]["server"]) : NULL;
            pdata->name = outs[o].exists("name") ? strdup(outs[o]["name"]) : "boondock_airband";
            pdata->sink = outs[o].exists("sink") ? strdup(outs[o]["sink"]) : NULL;

            if (outs[o].exists("stream_name")) {
                pdata->stream_name = strdup(outs[o]["stream_name"]);
            } else {
                if (parsing_mixers) {
                    cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: PulseAudio outputs of mixers must have stream_name defined\n";
                    error();
                }
                char buf[1024];
                snprintf(buf, sizeof(buf), "%.3f MHz", (float)channel->freqlist[0].frequency / 1000000.0f);
                pdata->stream_name = strdup(buf);
            }
#endif /* WITH_PULSEAUDIO */
        } else if (!strncmp(outs[o]["type"], "boondock_api", 12)) {
            // Skeleton for Boondock API - implementation to be defined later
            channel->outputs[oo].data = XCALLOC(1, sizeof(char) * 256);  // Placeholder
            channel->outputs[oo].type = O_BOONDOCK_API;
            // Store minimal data for now - actual implementation will parse api_url and api_key
            cerr << "Warning: Boondock API output type is not yet implemented (skeleton only)\n";
        } else if (!strncmp(outs[o]["type"], "redis", 5)) {
            // Skeleton for Redis - implementation to be defined later
            channel->outputs[oo].data = XCALLOC(1, sizeof(char) * 256);  // Placeholder
            channel->outputs[oo].type = O_REDIS;
            // Store minimal data for now - actual implementation will parse address, port, password, database
            cerr << "Warning: Redis output type is not yet implemented (skeleton only)\n";
        } else {
            if (parsing_mixers) {
                cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
            } else {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
            }
            cerr << "unknown output type\n";
            error();
        }
        channel->outputs[oo].enabled = true;
        channel->outputs[oo].active = false;
        oo++;
    }
    return oo;
}

static struct freq_t* mk_freqlist(int n) {
    if (n < 1) {
        cerr << "mk_freqlist: invalid list length " << n << "\n";
        error();
    }
    struct freq_t* fl = (struct freq_t*)XCALLOC(n, sizeof(struct freq_t));
    for (int i = 0; i < n; i++) {
        fl[i].frequency = 0;
        fl[i].label = NULL;
        fl[i].agcavgfast = 0.5f;
        fl[i].ampfactor = 1.0f;
        fl[i].squelch = Squelch();
        fl[i].active_counter = 0;
        fl[i].modulation = MOD_AM;
    }
    return fl;
}

static void warn_if_freq_not_in_range(int devidx, int chanidx, int freq, int centerfreq, int sample_rate) {
    static const float soft_bw_threshold = 0.9f;
    float bw_limit = (float)sample_rate / 2.f * soft_bw_threshold;
    if ((float)abs(freq - centerfreq) >= bw_limit) {
        log(LOG_WARNING, "Warning: dev[%d].channel[%d]: frequency %.3f MHz is outside of SDR operating bandwidth (%.3f-%.3f MHz)\n", devidx, chanidx, (double)freq / 1e6,
            (double)(centerfreq - bw_limit) / 1e6, (double)(centerfreq + bw_limit) / 1e6);
    }
}

static int parse_anynum2int(libconfig::Setting& f) {
    int ret = 0;
    if (f.getType() == libconfig::Setting::TypeInt) {
        ret = (int)f;
    } else if (f.getType() == libconfig::Setting::TypeFloat) {
        ret = (int)((double)f * 1e6);
    } else if (f.getType() == libconfig::Setting::TypeString) {
        char* s = strdup((char const*)f);
        ret = (int)atofs(s);
        free(s);
    }
    return ret;
}

static int parse_channels(libconfig::Setting& chans, device_t* dev, int i) {
    int jj = 0;
    for (int j = 0; j < chans.getLength(); j++) {
        // Skip channels marked as disabled (they remain in config file but are not processed)
        if (chans[j].exists("disable") && (bool)chans[j]["disable"] == true) {
            continue;
        }
        channel_t* channel = dev->channels + jj;
        for (int k = 0; k < AGC_EXTRA; k++) {
            channel->wavein[k] = 20;
            channel->waveout[k] = 0.5;
        }
        channel->axcindicate = NO_SIGNAL;
        channel->mode = MM_MONO;
        channel->freq_count = 1;
        channel->freq_idx = 0;
        channel->highpass = chans[j].exists("highpass") ? (int)chans[j]["highpass"] : 100;
        channel->lowpass = chans[j].exists("lowpass") ? (int)chans[j]["lowpass"] : 2500;
#ifdef NFM
        channel->pr = 0;
        channel->pj = 0;
        channel->prev_waveout = 0.5;
        channel->alpha = dev->alpha;
#endif /* NFM */

        // Make sure lowpass / highpass aren't flipped.
        // If lowpass is enabled (greater than zero) it must be larger than highpass
        if (channel->lowpass > 0 && channel->lowpass < channel->highpass) {
            cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: lowpass (" << channel->lowpass << ") must be greater than or equal to highpass (" << channel->highpass << ")\n";
            error();
        }

        modulations channel_modulation = MOD_AM;
        if (chans[j].exists("modulation")) {
#ifdef NFM
            if (strncmp(chans[j]["modulation"], "nfm", 3) == 0) {
                channel_modulation = MOD_NFM;
            } else
#endif /* NFM */
                if (strncmp(chans[j]["modulation"], "am", 2) != 0) {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: unknown modulation\n";
                    error();
                }
        }
        channel->afc = chans[j].exists("afc") ? (unsigned char)(unsigned int)chans[j]["afc"] : 0;
        if (dev->mode == R_MULTICHANNEL) {
            channel->freqlist = mk_freqlist(1);
            channel->freqlist[0].frequency = parse_anynum2int(chans[j]["freq"]);
            warn_if_freq_not_in_range(i, j, channel->freqlist[0].frequency, dev->input->centerfreq, dev->input->sample_rate);
            if (chans[j].exists("label")) {
                channel->freqlist[0].label = strdup(chans[j]["label"]);
            }
            channel->freqlist[0].modulation = channel_modulation;
        } else { /* R_SCAN */
            channel->freq_count = chans[j]["freqs"].getLength();
            if (channel->freq_count < 1) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: freqs should be a list with at least one element\n";
                error();
            }
            channel->freqlist = mk_freqlist(channel->freq_count);
            if (chans[j].exists("labels") && chans[j]["labels"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: labels should be a list with at least " << channel->freq_count << " elements\n";
                error();
            }
            if (chans[j].exists("squelch_threshold") && libconfig::Setting::TypeList == chans[j]["squelch_threshold"].getType() && chans[j]["squelch_threshold"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_threshold should be an int or a list of ints with at least " << channel->freq_count
                     << " elements\n";
                error();
            }
            if (chans[j].exists("squelch_snr_threshold") && libconfig::Setting::TypeList == chans[j]["squelch_snr_threshold"].getType() &&
                chans[j]["squelch_snr_threshold"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j
                     << "]: squelch_snr_threshold should be an int, a float or a list of "
                        "ints or floats with at least "
                     << channel->freq_count << " elements\n";
                error();
            }
            if (chans[j].exists("notch") && libconfig::Setting::TypeList == chans[j]["notch"].getType() && chans[j]["notch"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: notch should be an float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
            if (chans[j].exists("notch_q") && libconfig::Setting::TypeList == chans[j]["notch_q"].getType() && chans[j]["notch_q"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: notch_q should be a float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
            if (chans[j].exists("ctcss") && libconfig::Setting::TypeList == chans[j]["ctcss"].getType() && chans[j]["ctcss"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: ctcss should be an float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
            if (chans[j].exists("modulation") && chans[j].exists("modulations")) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: can't set both modulation and modulations\n";
                error();
            }
            if (chans[j].exists("modulations") && chans[j]["modulations"].getLength() < channel->freq_count) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: modulations should be a list with at least " << channel->freq_count << " elements\n";
                error();
            }

            for (int f = 0; f < channel->freq_count; f++) {
                channel->freqlist[f].frequency = parse_anynum2int((chans[j]["freqs"][f]));
                if (chans[j].exists("labels")) {
                    channel->freqlist[f].label = strdup(chans[j]["labels"][f]);
                }
                if (chans[j].exists("modulations")) {
#ifdef NFM
                    if (strncmp(chans[j]["modulations"][f], "nfm", 3) == 0) {
                        channel->freqlist[f].modulation = MOD_NFM;
                    } else
#endif /* NFM */
                        if (strncmp(chans[j]["modulations"][f], "am", 2) == 0) {
                            channel->freqlist[f].modulation = MOD_AM;
                        } else {
                            cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] modulations.[" << f << "]: unknown modulation\n";
                            error();
                        }
                } else {
                    channel->freqlist[f].modulation = channel_modulation;
                }
            }
            // Set initial frequency for scanning
            // We tune 20 FFT bins higher to avoid DC spike
            dev->input->centerfreq = channel->freqlist[0].frequency + 20 * (double)(dev->input->sample_rate / fft_size);
        }
        if (chans[j].exists("squelch")) {
            cerr << "Warning: 'squelch' no longer supported and will be ignored, use 'squelch_threshold' or 'squelch_snr_threshold' instead\n";
        }
        if (chans[j].exists("squelch_threshold") && chans[j].exists("squelch_snr_threshold")) {
            cerr << "Warning: Both 'squelch_threshold' and 'squelch_snr_threshold' are set and may conflict\n";
        }
        if (chans[j].exists("squelch_threshold")) {
            // Value is dBFS, zero disables manual threshold (ie use auto squelch), negative is valid, positive is invalid
            if (libconfig::Setting::TypeList == chans[j]["squelch_threshold"].getType()) {
                // New-style array of per-frequency squelch settings
                for (int f = 0; f < channel->freq_count; f++) {
                    int threshold_dBFS = (int)chans[j]["squelch_threshold"][f];
                    if (threshold_dBFS > 0) {
                        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_threshold must be less than or equal to 0\n";
                        error();
                    } else if (threshold_dBFS == 0) {
                        channel->freqlist[f].squelch.set_squelch_level_threshold(0);
                    } else {
                        channel->freqlist[f].squelch.set_squelch_level_threshold(dBFS_to_level(threshold_dBFS));
                    }
                }
            } else if (libconfig::Setting::TypeInt == chans[j]["squelch_threshold"].getType()) {
                // Legacy (single squelch for all frequencies)
                int threshold_dBFS = (int)chans[j]["squelch_threshold"];
                float level;
                if (threshold_dBFS > 0) {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_threshold must be less than or equal to 0\n";
                    error();
                } else if (threshold_dBFS == 0) {
                    level = 0;
                } else {
                    level = dBFS_to_level(threshold_dBFS);
                }

                for (int f = 0; f < channel->freq_count; f++) {
                    channel->freqlist[f].squelch.set_squelch_level_threshold(level);
                }
            } else {
                cerr << "Invalid value for squelch_threshold (should be int or list - use parentheses)\n";
                error();
            }
        }
        if (chans[j].exists("squelch_snr_threshold")) {
            // Value is SNR in dB, zero disables squelch (ie always open), -1 uses default value, positive is valid, other negative values are invalid
            if (libconfig::Setting::TypeList == chans[j]["squelch_snr_threshold"].getType()) {
                // New-style array of per-frequency squelch settings
                for (int f = 0; f < channel->freq_count; f++) {
                    float snr = 0.f;
                    if (libconfig::Setting::TypeFloat == chans[j]["squelch_snr_threshold"][f].getType()) {
                        snr = (float)chans[j]["squelch_snr_threshold"][f];
                    } else if (libconfig::Setting::TypeInt == chans[j]["squelch_snr_threshold"][f].getType()) {
                        snr = (int)chans[j]["squelch_snr_threshold"][f];
                    } else {
                        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_snr_threshold list must be of int or float\n";
                        error();
                    }

                    if (snr == -1.0) {
                        continue;  // "disable" for this channel in list
                    } else if (snr < 0) {
                        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_snr_threshold must be greater than or equal to 0\n";
                        error();
                    } else {
                        channel->freqlist[f].squelch.set_squelch_snr_threshold(snr);
                    }
                }
            } else if (libconfig::Setting::TypeFloat == chans[j]["squelch_snr_threshold"].getType() || libconfig::Setting::TypeInt == chans[j]["squelch_snr_threshold"].getType()) {
                // Legacy (single squelch for all frequencies)
                float snr = (libconfig::Setting::TypeFloat == chans[j]["squelch_snr_threshold"].getType()) ? (float)chans[j]["squelch_snr_threshold"] : (int)chans[j]["squelch_snr_threshold"];

                if (snr == -1.0) {
                    continue;  // "disable" so use the default without error message
                } else if (snr < 0) {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: squelch_snr_threshold must be greater than or equal to 0\n";
                    error();
                }

                for (int f = 0; f < channel->freq_count; f++) {
                    channel->freqlist[f].squelch.set_squelch_snr_threshold(snr);
                }
            } else {
                cerr << "Invalid value for squelch_snr_threshold (should be float, int, or list of int/float - use parentheses)\n";
                error();
            }
        }
        if (chans[j].exists("notch")) {
            static const float default_q = 10.0;

            if (chans[j].exists("notch_q") && chans[j]["notch"].getType() != chans[j]["notch_q"].getType()) {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: notch_q (if set) must be the same type as notch - "
                     << "float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
            if (libconfig::Setting::TypeList == chans[j]["notch"].getType()) {
                for (int f = 0; f < channel->freq_count; f++) {
                    float freq = (float)chans[j]["notch"][f];
                    float q = chans[j].exists("notch_q") ? (float)chans[j]["notch_q"][f] : default_q;

                    if (q == 0.0) {
                        q = default_q;
                    } else if (q <= 0.0) {
                        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] freq.[" << f << "]: invalid value for notch_q: " << q << " (must be greater than 0.0)\n";
                        error();
                    }

                    if (freq == 0) {
                        continue;  // "disable" for this channel in list
                    } else if (freq < 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "] freq.[" << f << "]: invalid value for notch: " << freq << ", ignoring\n";
                    } else {
                        channel->freqlist[f].notch_filter = NotchFilter(freq, WAVE_RATE, q);
                    }
                }
            } else if (libconfig::Setting::TypeFloat == chans[j]["notch"].getType()) {
                float freq = (float)chans[j]["notch"];
                float q = chans[j].exists("notch_q") ? (float)chans[j]["notch_q"] : default_q;
                if (q <= 0.0) {
                    cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: invalid value for notch_q: " << q << " (must be greater than 0.0)\n";
                    error();
                }
                for (int f = 0; f < channel->freq_count; f++) {
                    if (freq == 0) {
                        continue;  // "disable" is default so ignore without error message
                    } else if (freq < 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "]: notch value '" << freq << "' invalid, ignoring\n";
                    } else {
                        channel->freqlist[f].notch_filter = NotchFilter(freq, WAVE_RATE, q);
                    }
                }
            } else {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: notch should be an float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
        }
        if (chans[j].exists("ctcss")) {
            if (libconfig::Setting::TypeList == chans[j]["ctcss"].getType()) {
                for (int f = 0; f < channel->freq_count; f++) {
                    float freq = (float)chans[j]["ctcss"][f];

                    if (freq == 0) {
                        continue;  // "disable" for this channel in list
                    } else if (freq < 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "] freq.[" << f << "]: invalid value for ctcss: " << freq << ", ignoring\n";
                    } else {
                        channel->freqlist[f].squelch.set_ctcss_freq(freq, WAVE_RATE);
                    }
                }
            } else if (libconfig::Setting::TypeFloat == chans[j]["ctcss"].getType()) {
                float freq = (float)chans[j]["ctcss"];
                for (int f = 0; f < channel->freq_count; f++) {
                    if (freq <= 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "]: ctcss value '" << freq << "' invalid, ignoring\n";
                    } else {
                        channel->freqlist[f].squelch.set_ctcss_freq(freq, WAVE_RATE);
                    }
                }
            } else {
                cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: ctcss should be an float or a list of floats with at least " << channel->freq_count << " elements\n";
                error();
            }
        }
        if (chans[j].exists("bandwidth")) {
            channel->needs_raw_iq = 1;

            if (libconfig::Setting::TypeList == chans[j]["bandwidth"].getType()) {
                for (int f = 0; f < channel->freq_count; f++) {
                    int bandwidth = parse_anynum2int(chans[j]["bandwidth"][f]);

                    if (bandwidth == 0) {
                        continue;  // "disable" for this channel in list
                    } else if (bandwidth < 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "] freq.[" << f << "]: bandwidth value '" << bandwidth << "' invalid, ignoring\n";
                    } else {
                        channel->freqlist[f].lowpass_filter = LowpassFilter((float)bandwidth / 2, WAVE_RATE);
                    }
                }
            } else {
                int bandwidth = parse_anynum2int(chans[j]["bandwidth"]);
                if (bandwidth == 0) {
                    continue;  // "disable" is default so ignore without error message
                } else if (bandwidth < 0) {
                    cerr << "devices.[" << i << "] channels.[" << j << "]: bandwidth value '" << bandwidth << "' invalid, ignoring\n";
                } else {
                    for (int f = 0; f < channel->freq_count; f++) {
                        channel->freqlist[f].lowpass_filter = LowpassFilter((float)bandwidth / 2, WAVE_RATE);
                    }
                }
            }
        }
        if (chans[j].exists("ampfactor")) {
            if (libconfig::Setting::TypeList == chans[j]["ampfactor"].getType()) {
                for (int f = 0; f < channel->freq_count; f++) {
                    float ampfactor = (float)chans[j]["ampfactor"][f];

                    if (ampfactor < 0) {
                        cerr << "devices.[" << i << "] channels.[" << j << "] freq.[" << f << "]: ampfactor '" << ampfactor << "' must not be negative\n";
                        error();
                    }

                    channel->freqlist[f].ampfactor = ampfactor;
                }
            } else {
                float ampfactor = (float)chans[j]["ampfactor"];

                if (ampfactor < 0) {
                    cerr << "devices.[" << i << "] channels.[" << j << "]: ampfactor '" << ampfactor << "' must not be negative\n";
                    error();
                }

                for (int f = 0; f < channel->freq_count; f++) {
                    channel->freqlist[f].ampfactor = ampfactor;
                }
            }
        }

#ifdef NFM
        if (chans[j].exists("tau")) {
            channel->alpha = ((int)chans[j]["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)chans[j]["tau"])));
        }
#endif /* NFM */
        libconfig::Setting& outputs = chans[j]["outputs"];
        channel->output_count = outputs.getLength();
        if (channel->output_count < 1) {
            cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XCALLOC(channel->output_count, sizeof(struct output_t));
        int outputs_enabled = parse_outputs(outputs, channel, i, j, false);
        if (outputs_enabled < 1) {
            cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XREALLOC(channel->outputs, outputs_enabled * sizeof(struct output_t));
        channel->output_count = outputs_enabled;

        dev->base_bins[jj] = dev->bins[jj] =
            (size_t)ceil((channel->freqlist[0].frequency + dev->input->sample_rate - dev->input->centerfreq) / (double)(dev->input->sample_rate / fft_size) - 1.0) % fft_size;
        debug_print("bins[%d]: %zu\n", jj, dev->bins[jj]);

#ifdef NFM
        for (int f = 0; f < channel->freq_count; f++) {
            if (channel->freqlist[f].modulation == MOD_NFM) {
                channel->needs_raw_iq = 1;
                break;
            }
        }
#endif /* NFM */

        if (channel->needs_raw_iq) {
            // Downmixing is done only for NFM and raw IQ outputs. It's not critical to have some residual
            // freq offset in AM, as it doesn't affect sound quality significantly.
            double dm_dphi = (double)(channel->freqlist[0].frequency - dev->input->centerfreq);  // downmix freq in Hz

            // In general, sample_rate is not required to be an integer multiple of WAVE_RATE.
            // However the FFT window may only slide by an integer number of input samples. A non-zero rounding error
            // introduces additional phase rotation which we have to compensate in order to shift the channel of interest
            // to the center of the spectrum of the output I/Q stream. This is important for correct NFM demodulation.
            // The error value (in Hz):
            // - has an absolute value 0..WAVE_RATE/2
            // - is linear with the error introduced by rounding the value of sample_rate/WAVE_RATE to the nearest integer
            //   (range of -0.5..0.5)
            // - is linear with the distance between center frequency and the channel frequency, normalized to 0..1
            double decimation_factor = ((double)dev->input->sample_rate / (double)WAVE_RATE);
            double dm_dphi_correction = (double)WAVE_RATE / 2.0;
            dm_dphi_correction *= (decimation_factor - round(decimation_factor));
            dm_dphi_correction *= (double)(channel->freqlist[0].frequency - dev->input->centerfreq) / ((double)dev->input->sample_rate / 2.0);

            debug_print("dev[%d].chan[%d]: dm_dphi: %f Hz dm_dphi_correction: %f Hz\n", i, jj, dm_dphi, dm_dphi_correction);
            dm_dphi -= dm_dphi_correction;
            debug_print("dev[%d].chan[%d]: dm_dphi_corrected: %f Hz\n", i, jj, dm_dphi);
            // Normalize
            dm_dphi /= (double)WAVE_RATE;
            // Unalias it, to prevent overflow of int during cast
            dm_dphi -= trunc(dm_dphi);
            debug_print("dev[%d].chan[%d]: dm_dphi_normalized=%f\n", i, jj, dm_dphi);
            // Translate this to uint32_t range 0x00000000-0x00ffffff
            dm_dphi *= 256.0 * 65536.0;
            // Cast it to signed int first, because casting negative float to uint is not portable
            channel->dm_dphi = (uint32_t)((int)dm_dphi);
            debug_print("dev[%d].chan[%d]: dm_dphi_scaled=%f cast=0x%x\n", i, jj, dm_dphi, channel->dm_dphi);
            channel->dm_phi = 0.f;
        }

#ifdef DEBUG_SQUELCH
        // Setup squelch debug file, if enabled
        char tmp_filepath[1024];
        for (int f = 0; f < channel->freq_count; f++) {
            snprintf(tmp_filepath, sizeof(tmp_filepath), "./squelch_debug-%d-%d.dat", j, f);
            channel->freqlist[f].squelch.set_debug_file(tmp_filepath);
        }
#endif /* DEBUG_SQUELCH */

        jj++;
    }
    return jj;
}

int parse_devices(libconfig::Setting& devs) {
    int devcnt = 0;
    for (int i = 0; i < devs.getLength(); i++) {
        if (devs[i].exists("disable") && (bool)devs[i]["disable"] == true)
            continue;
        device_t* dev = devices + devcnt;
        if (devs[i].exists("type")) {
            dev->input = input_new(devs[i]["type"]);
            if (dev->input == NULL) {
                cerr << "Configuration error: devices.[" << i << "]: unsupported device type\n";
                error();
            }
        } else {
#ifdef WITH_RTLSDR
            cerr << "Warning: devices.[" << i << "]: assuming device type \"rtlsdr\", please set \"type\" in the device section.\n";
            dev->input = input_new("rtlsdr");
#else
            cerr << "Configuration error: devices.[" << i << "]: mandatory parameter missing: type\n";
            error();
#endif /* WITH_RTLSDR */
        }
        assert(dev->input != NULL);
        if (devs[i].exists("sample_rate")) {
            int sample_rate = parse_anynum2int(devs[i]["sample_rate"]);
            if (sample_rate < WAVE_RATE) {
                cerr << "Configuration error: devices.[" << i << "]: sample_rate must be greater than " << WAVE_RATE << "\n";
                error();
            }
            dev->input->sample_rate = sample_rate;
        }
        if (devs[i].exists("mode")) {
            if (!strncmp(devs[i]["mode"], "multichannel", 12)) {
                dev->mode = R_MULTICHANNEL;
            } else if (!strncmp(devs[i]["mode"], "scan", 4)) {
                dev->mode = R_SCAN;
            } else {
                cerr << "Configuration error: devices.[" << i << "]: invalid mode (must be one of: \"scan\", \"multichannel\")\n";
                error();
            }
        } else {
            dev->mode = R_MULTICHANNEL;
        }
        if (dev->mode == R_MULTICHANNEL) {
            dev->input->centerfreq = parse_anynum2int(devs[i]["centerfreq"]);
        }  // centerfreq for R_SCAN will be set by parse_channels() after frequency list has been read
#ifdef NFM
        if (devs[i].exists("tau")) {
            dev->alpha = ((int)devs[i]["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)devs[i]["tau"])));
        } else {
            dev->alpha = alpha;
        }
#endif /* NFM */

        // Parse hardware-dependent configuration parameters
        if (input_parse_config(dev->input, devs[i]) < 0) {
            // FIXME: get and display error string from input_parse_config
            // Right now it exits the program on failure.
        }
        // Some basic sanity checks for crucial parameters which have to be set
        // (or can be modified) by the input driver
        assert(dev->input->sfmt != SFMT_UNDEF);
        assert(dev->input->fullscale > 0);
        assert(dev->input->bytes_per_sample > 0);
        assert(dev->input->sample_rate > WAVE_RATE);

        // For the input buffer size use a base value and round it up to the nearest multiple
        // of FFT_BATCH blocks of input samples.
        // ceil is required here because sample rate is not guaranteed to be an integer multiple of WAVE_RATE.
        size_t fft_batch_len = FFT_BATCH * (2 * dev->input->bytes_per_sample * (size_t)ceil((double)dev->input->sample_rate / (double)WAVE_RATE));
        dev->input->buf_size = MIN_BUF_SIZE;
        if (dev->input->buf_size % fft_batch_len != 0)
            dev->input->buf_size += fft_batch_len - dev->input->buf_size % fft_batch_len;
        debug_print("dev->input->buf_size: %zu\n", dev->input->buf_size);
        dev->input->buffer = (unsigned char*)XCALLOC(sizeof(unsigned char), dev->input->buf_size + 2 * dev->input->bytes_per_sample * fft_size);
        dev->input->bufs = dev->input->bufe = 0;
        dev->input->overflow_count = 0;
        dev->output_overrun_count = 0;
        dev->waveend = dev->waveavail = dev->row = dev->tq_head = dev->tq_tail = 0;
        dev->last_frequency = -1;
        
        // Initialize spectrum analyzer data
        // Store full FFT size to cover entire bandwidth (DC to Nyquist)
        dev->spectrum.size = fft_size;
        dev->spectrum.magnitude = (float*)XCALLOC(dev->spectrum.size, sizeof(float));
        pthread_mutex_init(&dev->spectrum.mutex, NULL);
        dev->spectrum.last_update = 0;
        dev->spectrum.enabled = true;  // Enable by default
        dev->spectrum.update_counter = 0;

        libconfig::Setting& chans = devs[i]["channels"];
        if (chans.getLength() < 1) {
            cerr << "Configuration error: devices.[" << i << "]: no channels configured\n";
            error();
        }
        dev->channels = (channel_t*)XCALLOC(chans.getLength(), sizeof(channel_t));
        dev->bins = (size_t*)XCALLOC(chans.getLength(), sizeof(size_t));
        dev->base_bins = (size_t*)XCALLOC(chans.getLength(), sizeof(size_t));
        dev->channel_count = 0;
        int channel_count = parse_channels(chans, dev, i);
        if (channel_count < 1) {
            cerr << "Configuration error: devices.[" << i << "]: no channels enabled\n";
            error();
        }
        if (dev->mode == R_SCAN && channel_count > 1) {
            cerr << "Configuration error: devices.[" << i << "]: only one channel is allowed in scan mode\n";
            error();
        }
        dev->channels = (channel_t*)XREALLOC(dev->channels, channel_count * sizeof(channel_t));
        dev->bins = (size_t*)XREALLOC(dev->bins, channel_count * sizeof(size_t));
        dev->base_bins = (size_t*)XREALLOC(dev->base_bins, channel_count * sizeof(size_t));
        dev->channel_count = channel_count;
        devcnt++;
    }
    return devcnt;
}

int parse_mixers(libconfig::Setting& mx) {
    const char* name;
    int mm = 0;
    for (int i = 0; i < mx.getLength(); i++) {
        if (mx[i].exists("disable") && (bool)mx[i]["disable"] == true)
            continue;
        if ((name = mx[i].getName()) == NULL) {
            cerr << "Configuration error: mixers.[" << i << "]: undefined mixer name\n";
            error();
        }
        debug_print("mm=%d name=%s\n", mm, name);
        mixer_t* mixer = &mixers[mm];
        mixer->name = strdup(name);
        mixer->enabled = false;
        mixer->interval = MIX_DIVISOR;
        mixer->output_overrun_count = 0;
        mixer->input_count = 0;
        mixer->inputs = NULL;
        mixer->inputs_todo = NULL;
        mixer->input_mask = NULL;
        channel_t* channel = &mixer->channel;
        channel->highpass = mx[i].exists("highpass") ? (int)mx[i]["highpass"] : 100;
        channel->lowpass = mx[i].exists("lowpass") ? (int)mx[i]["lowpass"] : 2500;
        channel->mode = MM_MONO;

        // Make sure lowpass / highpass aren't flipped.
        // If lowpass is enabled (greater than zero) it must be larger than highpass
        if (channel->lowpass > 0 && channel->lowpass < channel->highpass) {
            cerr << "Configuration error: mixers.[" << i << "]: lowpass (" << channel->lowpass << ") must be greater than or equal to highpass (" << channel->highpass << ")\n";
            error();
        }

        libconfig::Setting& outputs = mx[i]["outputs"];
        channel->output_count = outputs.getLength();
        if (channel->output_count < 1) {
            cerr << "Configuration error: mixers.[" << i << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XCALLOC(channel->output_count, sizeof(struct output_t));
        int outputs_enabled = parse_outputs(outputs, channel, i, 0, true);
        if (outputs_enabled < 1) {
            cerr << "Configuration error: mixers.[" << i << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XREALLOC(channel->outputs, outputs_enabled * sizeof(struct output_t));
        channel->output_count = outputs_enabled;
        mm++;
    }
    return mm;
}

// Convert channels.json to libconfig format string
static string convert_json_to_libconfig(const string& json_path) {
    ifstream file(json_path);
    if (!file.is_open()) {
        cerr << "Cannot open channels.json: " << json_path << "\n";
        return "";
    }
    
    stringstream libconfig;
    string line;
    
    // Read entire file
    string json_content;
    while (getline(file, line)) {
        json_content += line + "\n";
    }
    file.close();
    
    // Write global settings
    libconfig << "fft_size = 2048;\n";
    libconfig << "localtime = false;\n";
    libconfig << "multiple_demod_threads = true;\n";
    libconfig << "multiple_output_threads = true;\n";
    libconfig << "file_chunk_duration_minutes = 5;\n";
    libconfig << "\ndevices: (\n";
    
    // Simple JSON parsing - extract devices array
    size_t devices_pos = json_content.find("\"devices\"");
    if (devices_pos == string::npos) {
        cerr << "Invalid channels.json: missing devices array\n";
        return "";
    }
    
    // Find opening bracket of devices array
    size_t devices_start = json_content.find("[", devices_pos);
    if (devices_start == string::npos) {
        cerr << "Invalid channels.json: devices array not found\n";
        return "";
    }
    
    // Parse devices (simplified - assumes single device for now)
    size_t device_start = json_content.find("{", devices_start);
    if (device_start == string::npos) {
        cerr << "Invalid channels.json: no device found\n";
        return "";
    }
    
    // Extract device properties using simple string parsing
    string device_json = json_content.substr(device_start);
    
    // Extract type
    size_t type_pos = device_json.find("\"type\"");
    string device_type = "rtlsdr";
    if (type_pos != string::npos) {
        size_t colon = device_json.find(":", type_pos);
        size_t quote1 = device_json.find("\"", colon);
        size_t quote2 = device_json.find("\"", quote1 + 1);
        if (quote1 != string::npos && quote2 != string::npos) {
            device_type = device_json.substr(quote1 + 1, quote2 - quote1 - 1);
        }
    }
    
    // Extract sample_rate
    size_t sample_rate_pos = device_json.find("\"sample_rate\"");
    string sample_rate = "2.40";
    if (sample_rate_pos != string::npos) {
        size_t colon = device_json.find(":", sample_rate_pos);
        size_t end = device_json.find_first_of(",}", colon);
        if (end != string::npos) {
            sample_rate = device_json.substr(colon + 1, end - colon - 1);
            sample_rate.erase(0, sample_rate.find_first_not_of(" \t\n"));
            sample_rate.erase(sample_rate.find_last_not_of(" \t\n") + 1);
        }
    }
    
    // Extract centerfreq
    size_t centerfreq_pos = device_json.find("\"centerfreq\"");
    string centerfreq = "162.48200";
    if (centerfreq_pos != string::npos) {
        size_t colon = device_json.find(":", centerfreq_pos);
        size_t end = device_json.find_first_of(",}", colon);
        if (end != string::npos) {
            centerfreq = device_json.substr(colon + 1, end - colon - 1);
            centerfreq.erase(0, centerfreq.find_first_not_of(" \t\n"));
            centerfreq.erase(centerfreq.find_last_not_of(" \t\n") + 1);
        }
    }
    
    // Extract gain
    size_t gain_pos = device_json.find("\"gain\"");
    string gain = "19.7";
    if (gain_pos != string::npos) {
        size_t colon = device_json.find(":", gain_pos);
        size_t end = device_json.find_first_of(",}", colon);
        if (end != string::npos) {
            gain = device_json.substr(colon + 1, end - colon - 1);
            gain.erase(0, gain.find_first_not_of(" \t\n"));
            gain.erase(gain.find_last_not_of(" \t\n") + 1);
        }
    }
    
    // Extract correction
    size_t correction_pos = device_json.find("\"correction\"");
    string correction = "0";
    if (correction_pos != string::npos) {
        size_t colon = device_json.find(":", correction_pos);
        size_t end = device_json.find_first_of(",}", colon);
        if (end != string::npos) {
            correction = device_json.substr(colon + 1, end - colon - 1);
            correction.erase(0, correction.find_first_not_of(" \t\n"));
            correction.erase(correction.find_last_not_of(" \t\n") + 1);
        }
    }
    
    // Extract index
    size_t index_pos = device_json.find("\"index\"");
    string index = "0";
    if (index_pos != string::npos) {
        size_t colon = device_json.find(":", index_pos);
        size_t end = device_json.find_first_of(",}", colon);
        if (end != string::npos) {
            index = device_json.substr(colon + 1, end - colon - 1);
            index.erase(0, index.find_first_not_of(" \t\n"));
            index.erase(index.find_last_not_of(" \t\n") + 1);
        }
    }
    
    // Write device header
    libconfig << "  {\n";
    libconfig << "    type = \"" << device_type << "\";\n";
    libconfig << "    index = " << index << ";\n";
    libconfig << "    gain = " << gain << ";\n";
    libconfig << "    centerfreq = " << centerfreq << ";\n";
    libconfig << "    correction = " << correction << ";\n";
    libconfig << "    sample_rate = " << sample_rate << ";\n";
    libconfig << "    channels: (\n";
    
    // Extract channels array
    size_t channels_pos = device_json.find("\"channels\"");
    if (channels_pos == string::npos) {
        cerr << "Invalid channels.json: no channels array\n";
        return "";
    }
    
    size_t channels_start = device_json.find("[", channels_pos);
    if (channels_start == string::npos) {
        cerr << "Invalid channels.json: channels array not found\n";
        return "";
    }
    
    // Parse each channel
    size_t channel_start = channels_start;
    int channel_num = 0;
    while ((channel_start = device_json.find("{", channel_start)) != string::npos) {
        // Find the matching closing brace (handle nested braces)
        int brace_count = 0;
        size_t channel_end = channel_start;
        while (channel_end < device_json.length() && channel_end <= device_json.find("]", channels_start)) {
            if (device_json[channel_end] == '{') brace_count++;
            if (device_json[channel_end] == '}') {
                brace_count--;
                if (brace_count == 0) break;
            }
            channel_end++;
        }
        if (channel_end == device_json.length()) break;
        
        string channel_json = device_json.substr(channel_start, channel_end - channel_start + 1);
        
        // Check if channel is enabled
        size_t enabled_pos = channel_json.find("\"enabled\"");
        bool channel_enabled = true;
        if (enabled_pos != string::npos) {
            size_t colon = channel_json.find(":", enabled_pos);
            size_t end = channel_json.find_first_of(",}", colon);
            if (end != string::npos) {
                string enabled_str = channel_json.substr(colon + 1, end - colon - 1);
                enabled_str.erase(0, enabled_str.find_first_not_of(" \t\n"));
                enabled_str.erase(enabled_str.find_last_not_of(" \t\n") + 1);
                channel_enabled = (enabled_str == "true");
            }
        }
        
        // Only process enabled channels
        if (!channel_enabled) {
            channel_start = channel_end + 1;
            continue;
        }
        
        // Extract channel properties
        // freq
        size_t freq_pos = channel_json.find("\"freq\"");
        string freq = "0";
        if (freq_pos != string::npos) {
            size_t colon = channel_json.find(":", freq_pos);
            size_t end = channel_json.find_first_of(",}", colon);
            if (end != string::npos) {
                freq = channel_json.substr(colon + 1, end - colon - 1);
                freq.erase(0, freq.find_first_not_of(" \t\n"));
                freq.erase(freq.find_last_not_of(" \t\n") + 1);
            }
        }
        
        // label
        size_t label_pos = channel_json.find("\"label\"");
        string label = "";
        if (label_pos != string::npos) {
            size_t colon = channel_json.find(":", label_pos);
            size_t quote1 = channel_json.find("\"", colon);
            size_t quote2 = channel_json.find("\"", quote1 + 1);
            if (quote1 != string::npos && quote2 != string::npos) {
                label = channel_json.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
        
        // modulation
        size_t mod_pos = channel_json.find("\"modulation\"");
        string modulation = "nfm";
        if (mod_pos != string::npos) {
            size_t colon = channel_json.find(":", mod_pos);
            size_t quote1 = channel_json.find("\"", colon);
            size_t quote2 = channel_json.find("\"", quote1 + 1);
            if (quote1 != string::npos && quote2 != string::npos) {
                modulation = channel_json.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
        
        // bandwidth
        size_t bw_pos = channel_json.find("\"bandwidth\"");
        string bandwidth = "12000";
        if (bw_pos != string::npos) {
            size_t colon = channel_json.find(":", bw_pos);
            size_t end = channel_json.find_first_of(",}", colon);
            if (end != string::npos) {
                bandwidth = channel_json.substr(colon + 1, end - colon - 1);
                bandwidth.erase(0, bandwidth.find_first_not_of(" \t\n"));
                bandwidth.erase(bandwidth.find_last_not_of(" \t\n") + 1);
            }
        }
        
        // Write channel
        libconfig << "      {\n";
        libconfig << "        freq = " << freq << ";\n";
        if (!label.empty()) {
            libconfig << "        label = \"" << label << "\";\n";
        }
        libconfig << "        modulation = \"" << modulation << "\";\n";
        libconfig << "        bandwidth = " << bandwidth << ";\n";
        
        // Parse outputs
        size_t outputs_pos = channel_json.find("\"outputs\"");
        if (outputs_pos != string::npos) {
            size_t outputs_start = channel_json.find("[", outputs_pos);
            if (outputs_start != string::npos) {
                libconfig << "        outputs: (\n";
                
                size_t output_start = outputs_start;
                int output_num = 0;
                while ((output_start = channel_json.find("{", output_start)) != string::npos) {
                    // Find the matching closing brace (handle nested braces)
                    int output_brace_count = 0;
                    size_t output_end = output_start;
                    while (output_end < channel_json.length() && output_end <= channel_json.find("]", outputs_start)) {
                        if (channel_json[output_end] == '{') output_brace_count++;
                        if (channel_json[output_end] == '}') {
                            output_brace_count--;
                            if (output_brace_count == 0) break;
                        }
                        output_end++;
                    }
                    if (output_end == channel_json.length() || output_end > channel_json.find("]", outputs_start)) break;
                    
                    string output_json = channel_json.substr(output_start, output_end - output_start + 1);
                    
                    // Debug: check if directory is in the extracted JSON
                    if (output_json.find("\"directory\"") != string::npos) {
                        size_t dir_debug = output_json.find("\"directory\"");
                        size_t dir_debug_end = min(dir_debug + 100, output_json.length());
                        cerr << "DEBUG output_json (around directory): " << output_json.substr(dir_debug, dir_debug_end - dir_debug) << "\n";
                    }
                    
                    // Check if output is enabled
                    size_t out_enabled_pos = output_json.find("\"enabled\"");
                    bool output_enabled = true;
                    if (out_enabled_pos != string::npos) {
                        size_t colon = output_json.find(":", out_enabled_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string enabled_str = output_json.substr(colon + 1, end - colon - 1);
                            enabled_str.erase(0, enabled_str.find_first_not_of(" \t\n"));
                            enabled_str.erase(enabled_str.find_last_not_of(" \t\n") + 1);
                            output_enabled = (enabled_str == "true");
                        }
                    }
                    
                    if (!output_enabled) {
                        output_start = output_end + 1;
                        continue;
                    }
                    
                    // Extract output type
                    size_t output_type_pos = output_json.find("\"type\"");
                    string output_type = "file";
                    if (output_type_pos != string::npos) {
                        size_t colon = output_json.find(":", output_type_pos);
                        size_t quote1 = output_json.find("\"", colon);
                        size_t quote2 = output_json.find("\"", quote1 + 1);
                        if (quote1 != string::npos && quote2 != string::npos) {
                            output_type = output_json.substr(quote1 + 1, quote2 - quote1 - 1);
                        }
                    }
                    
                    libconfig << "          {\n";
                    libconfig << "            type = \"" << output_type << "\";\n";
                    
                    // Extract directory
                    size_t dir_pos = output_json.find("\"directory\"");
                    if (dir_pos != string::npos) {
                        size_t colon = output_json.find(":", dir_pos);
                        if (colon != string::npos) {
                            // Skip whitespace after colon
                            size_t value_start = colon + 1;
                            while (value_start < output_json.length() && (output_json[value_start] == ' ' || output_json[value_start] == '\t')) {
                                value_start++;
                            }
                            // Find the opening quote after the colon
                            if (value_start < output_json.length() && output_json[value_start] == '"') {
                                size_t quote1 = value_start;
                                // Find the closing quote, starting from after the opening quote
                                size_t quote2 = quote1 + 1;
                                while (quote2 < output_json.length() && output_json[quote2] != '"') {
                                    // Handle escaped quotes
                                    if (output_json[quote2] == '\\' && quote2 + 1 < output_json.length()) {
                                        quote2 += 2;
                                    } else {
                                        quote2++;
                                    }
                                }
                                if (quote2 < output_json.length()) {
                                    string directory = output_json.substr(quote1 + 1, quote2 - quote1 - 1);
                                    // Escape quotes in the directory string for libconfig
                                    string escaped_dir = directory;
                                    size_t esc_pos = 0;
                                    while ((esc_pos = escaped_dir.find("\"", esc_pos)) != string::npos) {
                                        escaped_dir.replace(esc_pos, 1, "\\\"");
                                        esc_pos += 2;
                                    }
                                    libconfig << "            directory = \"" << escaped_dir << "\";\n";
                                }
                            }
                        }
                    }
                    
                    // Extract filename_template
                    size_t filename_pos = output_json.find("\"filename_template\"");
                    if (filename_pos != string::npos) {
                        size_t colon = output_json.find(":", filename_pos);
                        if (colon != string::npos) {
                            // Skip whitespace after colon
                            size_t value_start = colon + 1;
                            while (value_start < output_json.length() && (output_json[value_start] == ' ' || output_json[value_start] == '\t')) {
                                value_start++;
                            }
                            // Find the opening quote after the colon
                            if (value_start < output_json.length() && output_json[value_start] == '"') {
                                size_t quote1 = value_start;
                                // Find the closing quote, starting from after the opening quote
                                size_t quote2 = quote1 + 1;
                                while (quote2 < output_json.length() && output_json[quote2] != '"') {
                                    // Handle escaped quotes
                                    if (output_json[quote2] == '\\' && quote2 + 1 < output_json.length()) {
                                        quote2 += 2;
                                    } else {
                                        quote2++;
                                    }
                                }
                                if (quote2 < output_json.length()) {
                                    string filename = output_json.substr(quote1 + 1, quote2 - quote1 - 1);
                                    // Escape quotes in the filename string for libconfig
                                    string escaped_filename = filename;
                                    size_t esc_pos = 0;
                                    while ((esc_pos = escaped_filename.find("\"", esc_pos)) != string::npos) {
                                        escaped_filename.replace(esc_pos, 1, "\\\"");
                                        esc_pos += 2;
                                    }
                                    libconfig << "            filename_template = \"" << escaped_filename << "\";\n";
                                }
                            }
                        }
                    }
                    
                    // Extract continuous
                    size_t cont_pos = output_json.find("\"continuous\"");
                    if (cont_pos != string::npos) {
                        size_t colon = output_json.find(":", cont_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string cont_str = output_json.substr(colon + 1, end - colon - 1);
                            cont_str.erase(0, cont_str.find_first_not_of(" \t\n"));
                            cont_str.erase(cont_str.find_last_not_of(" \t\n") + 1);
                            libconfig << "            continuous = " << (cont_str == "true" ? "true" : "false") << ";\n";
                        }
                    }
                    
                    // Extract split_on_transmission
                    size_t split_pos = output_json.find("\"split_on_transmission\"");
                    if (split_pos != string::npos) {
                        size_t colon = output_json.find(":", split_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string split_str = output_json.substr(colon + 1, end - colon - 1);
                            split_str.erase(0, split_str.find_first_not_of(" \t\n"));
                            split_str.erase(split_str.find_last_not_of(" \t\n") + 1);
                            libconfig << "            split_on_transmission = " << (split_str == "true" ? "true" : "false") << ";\n";
                        }
                    }
                    
                    // Extract include_freq
                    size_t inc_freq_pos = output_json.find("\"include_freq\"");
                    if (inc_freq_pos != string::npos) {
                        size_t colon = output_json.find(":", inc_freq_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string inc_freq_str = output_json.substr(colon + 1, end - colon - 1);
                            inc_freq_str.erase(0, inc_freq_str.find_first_not_of(" \t\n"));
                            inc_freq_str.erase(inc_freq_str.find_last_not_of(" \t\n") + 1);
                            libconfig << "            include_freq = " << (inc_freq_str == "true" ? "true" : "false") << ";\n";
                        }
                    }
                    
                    // Extract append
                    size_t append_pos = output_json.find("\"append\"");
                    if (append_pos != string::npos) {
                        size_t colon = output_json.find(":", append_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string append_str = output_json.substr(colon + 1, end - colon - 1);
                            append_str.erase(0, append_str.find_first_not_of(" \t\n"));
                            append_str.erase(append_str.find_last_not_of(" \t\n") + 1);
                            libconfig << "            append = " << (append_str == "true" ? "true" : "false") << ";\n";
                        }
                    }
                    
                    // Extract dated_subdirectories
                    size_t dated_pos = output_json.find("\"dated_subdirectories\"");
                    if (dated_pos != string::npos) {
                        size_t colon = output_json.find(":", dated_pos);
                        size_t end = output_json.find_first_of(",}", colon);
                        if (end != string::npos) {
                            string dated_str = output_json.substr(colon + 1, end - colon - 1);
                            dated_str.erase(0, dated_str.find_first_not_of(" \t\n"));
                            dated_str.erase(dated_str.find_last_not_of(" \t\n") + 1);
                            libconfig << "            dated_subdirectories = " << (dated_str == "true" ? "true" : "false") << ";\n";
                        }
                    }
                    
                    libconfig << "          }";
                    if (output_num > 0) {
                        libconfig << ",";
                    }
                    libconfig << "\n";
                    output_num++;
                    output_start = output_end + 1;
                }
                
                libconfig << "        );\n";
            }
        }
        
        libconfig << "      }\n";
        channel_num++;
        channel_start = channel_end + 1;
    }
    
    libconfig << "    );\n";
    libconfig << "  }\n";
    libconfig << ");\n";
    
    return libconfig.str();
}

// Read configuration from channels.json and convert to libconfig Config object
bool read_config_from_channels_json(const char* json_path, libconfig::Config& config) {
    if (!file_exists(json_path)) {
        cerr << "channels.json not found: " << json_path << "\n";
        return false;
    }
    
    string libconfig_str = convert_json_to_libconfig(json_path);
    if (libconfig_str.empty()) {
        return false;
    }
    
    // Debug: write generated config to stderr for troubleshooting
    // Find the directory line to see what's being generated
    size_t dir_debug_pos = libconfig_str.find("directory =");
    if (dir_debug_pos != string::npos) {
        size_t dir_debug_end = libconfig_str.find("\n", dir_debug_pos);
        if (dir_debug_end != string::npos) {
            cerr << "DEBUG directory line: " << libconfig_str.substr(dir_debug_pos, dir_debug_end - dir_debug_pos) << "\n";
        }
    }
    cerr << "Generated libconfig (first 500 chars):\n" << libconfig_str.substr(0, 500) << "\n";
    
    try {
        config.readString(libconfig_str.c_str());
        return true;
    } catch (const libconfig::ParseException& e) {
        cerr << "Error parsing converted config: " << e.getError() << " at line " << e.getLine() << "\n";
        // Output the problematic line for debugging
        stringstream ss(libconfig_str);
        string line;
        int line_num = 1;
        while (getline(ss, line) && line_num <= e.getLine() + 2) {
            if (line_num >= e.getLine() - 2) {
                cerr << (line_num == e.getLine() ? ">>> " : "    ") << line_num << ": " << line << "\n";
            }
            line_num++;
        }
        return false;
    }
}

// vim: ts=4

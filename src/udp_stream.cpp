/*
 * udp_stream.cpp
 *
 * Copyright (C) 2024 charlie-foxtrot
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
#include <string.h>  // strerror()
#include <syslog.h>  // LOG_INFO / LOG_ERR
#include <unistd.h>  // close()
#include <cassert>   // assert()
#include <errno.h>   // errno, EAGAIN, EWOULDBLOCK

#include <arpa/inet.h>  // inet_aton()
#include <netdb.h>      // getaddrinfo()
#include <sys/socket.h> // setsockopt, SO_SNDBUF, IP_TOS

#include "boondock_airband.h"

// Maximum UDP payload size to avoid fragmentation (Ethernet MTU 1500 - IP header 20 - UDP header 8)
#define MAX_UDP_PAYLOAD 1472
#define UDP_HEADER_SIZE sizeof(struct udp_packet_header)

bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len, int channel_id) {
    // pre-allocate the stereo buffer
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    sdata->channel_id = channel_id;
    sdata->send_socket = -1;
    sdata->dest_sockaddr_len = 0;

    // lookup address / port
    struct addrinfo hints, *result, *rptr;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    int error = getaddrinfo(sdata->dest_address, sdata->dest_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "udp_stream: could not resolve %s:%s - %s\n", sdata->dest_address, sdata->dest_port, gai_strerror(error));
        return false;
    }

    // check each result and try to create a connection
    for (rptr = result; rptr != NULL; rptr = rptr->ai_next) {
        sdata->send_socket = socket(rptr->ai_family, rptr->ai_socktype, rptr->ai_protocol);
        if (sdata->send_socket == -1) {
            log(LOG_ERR, "udp_stream: socket failed: %s\n", strerror(errno));
            continue;
        }

        if (connect(sdata->send_socket, rptr->ai_addr, rptr->ai_addrlen) == -1) {
            log(LOG_INFO, "udp_stream: connect to %s:%s failed: %s\n", sdata->dest_address, sdata->dest_port, strerror(errno));
            close(sdata->send_socket);
            sdata->send_socket = -1;
            continue;
        }

        // Set socket options for optimal UDP streaming
        int sendbuf = 256 * 1024;  // 256 KB send buffer
        if (setsockopt(sdata->send_socket, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) == -1) {
            log(LOG_WARNING, "udp_stream: failed to set SO_SNDBUF: %s\n", strerror(errno));
        }

        // Set IP_TOS for low latency (audio streaming priority)
        if (rptr->ai_family == AF_INET) {
            int tos = 0x10;  // IPTOS_LOWDELAY
            if (setsockopt(sdata->send_socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == -1) {
                // Ignore error - not critical
            }
        }

        sdata->dest_sockaddr = *rptr->ai_addr;
        sdata->dest_sockaddr_len = rptr->ai_addrlen;
        break;
    }
    freeaddrinfo(result);

    // error if no valid socket
    if (sdata->send_socket == -1) {
        log(LOG_ERR, "udp_stream: could not set up UDP socket to %s:%s - all addresses failed\n", sdata->dest_address, sdata->dest_port);
        return false;
    }

    log(LOG_INFO, "udp_stream: sending %s 32-bit float at %d Hz to %s:%s (headers: %s, chunking: %s)\n", 
        mode == MM_MONO ? "Mono" : "Stereo", WAVE_RATE, sdata->dest_address, sdata->dest_port,
        sdata->enable_headers ? "enabled" : "disabled",
        sdata->enable_chunking ? "enabled" : "disabled");
    return true;
}

// Helper function to create and fill UDP header
static void fill_udp_header(udp_stream_data* sdata, channel_t* channel, struct udp_packet_header* header) {
    if (!channel || !header) {
        return;
    }
    
    header->channel_id = (uint16_t)sdata->channel_id;
    
    // Check if channel has freqlist (mixers might not have it)
    if (channel->freqlist && channel->freq_count > 0 && channel->freq_idx >= 0 && channel->freq_idx < channel->freq_count) {
        freq_t* fparms = channel->freqlist + channel->freq_idx;
        
        header->frequency_hz = (uint32_t)fparms->frequency;
        
        // Convert signal level to dBFS and clamp to int16_t range
        float signal_dbfs = level_to_dBFS(fparms->squelch.signal_level());
        signal_dbfs = (signal_dbfs < -120.0f) ? -120.0f : (signal_dbfs > 0.0f) ? 0.0f : signal_dbfs;
        header->signal_dbfs = (int16_t)(signal_dbfs * 10.0f);  // Store as 0.1 dB units
        
        // Calculate SNR and clamp to int16_t range
        float noise_dbfs = level_to_dBFS(fparms->squelch.noise_level());
        float snr = signal_dbfs - noise_dbfs;
        snr = (snr < -50.0f) ? -50.0f : (snr > 50.0f) ? 50.0f : snr;
        header->snr_db = (int16_t)(snr * 10.0f);  // Store as 0.1 dB units
    } else {
        // For mixers or channels without freqlist, use default values
        header->frequency_hz = 0;
        header->signal_dbfs = -1200;  // -120.0 dB in 0.1 dB units
        header->snr_db = 0;
    }
}

void udp_stream_write(udp_stream_data* sdata, channel_t* channel, const float* data, size_t len) {
    if (sdata->send_socket == -1 || len == 0) {
        return;
    }

    const char* data_ptr = (const char*)data;
    size_t remaining = len;
    
    if (sdata->enable_chunking) {
        // Calculate max payload per chunk (account for header if enabled)
        size_t max_payload = sdata->enable_headers ? 
            (MAX_UDP_PAYLOAD - UDP_HEADER_SIZE) : MAX_UDP_PAYLOAD;
        
        // Ensure max_payload is aligned to float size
        max_payload = (max_payload / sizeof(float)) * sizeof(float);
        
        while (remaining > 0) {
            size_t chunk_size = (remaining > max_payload) ? max_payload : remaining;
            
            if (sdata->enable_headers && channel) {
                // Allocate buffer for header + chunk
                size_t total_size = UDP_HEADER_SIZE + chunk_size;
                char* packet = (char*)XCALLOC(1, total_size);
                
                // Fill header
                fill_udp_header(sdata, channel, (struct udp_packet_header*)packet);
                
                // Copy audio data
                memcpy(packet + UDP_HEADER_SIZE, data_ptr, chunk_size);
                
                // Send packet
                ssize_t sent = send(sdata->send_socket, packet, total_size, MSG_DONTWAIT | MSG_NOSIGNAL);
                free(packet);
                
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Error occurred, stop sending remaining chunks
                    break;
                }
            } else {
                // No header, send chunk directly
                ssize_t sent = send(sdata->send_socket, data_ptr, chunk_size, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Error occurred, stop sending remaining chunks
                    break;
                } else if (sent > 0 && (size_t)sent < chunk_size) {
                    // Partial send - adjust pointer and continue
                    data_ptr += sent;
                    remaining -= sent;
                    continue;
                }
            }
            
            data_ptr += chunk_size;
            remaining -= chunk_size;
        }
    } else {
        // No chunking - send entire payload as single packet
        if (sdata->enable_headers && channel) {
            size_t total_size = UDP_HEADER_SIZE + len;
            char* packet = (char*)XCALLOC(1, total_size);
            
            // Fill header
            fill_udp_header(sdata, channel, (struct udp_packet_header*)packet);
            
            // Copy audio data
            memcpy(packet + UDP_HEADER_SIZE, data_ptr, len);
            
            // Send packet
            send(sdata->send_socket, packet, total_size, MSG_DONTWAIT | MSG_NOSIGNAL);
            free(packet);
        } else {
            // No header, send directly
            send(sdata->send_socket, data_ptr, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        }
    }
}

void udp_stream_write(udp_stream_data* sdata, channel_t* channel, const float* data_left, const float* data_right, size_t len) {
    if (sdata->send_socket != -1) {
        assert(len * 2 <= sdata->stereo_buffer_len);
        for (size_t i = 0; i < len; ++i) {
            sdata->stereo_buffer[2 * i] = data_left[i];
            sdata->stereo_buffer[2 * i + 1] = data_right[i];
        }
        udp_stream_write(sdata, channel, sdata->stereo_buffer, len * 2 * sizeof(float));
    }
}

void udp_stream_shutdown(udp_stream_data* sdata) {
    if (sdata->send_socket != -1) {
        close(sdata->send_socket);
    }
}

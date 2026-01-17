/*
 * web_server.cpp
 * Web interface server for Boondock Airband
 *
 * Copyright (c) 2026 Boondock Technologies
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

#include "web_server.h"
#include "boondock_airband.h"
#include "config.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <libconfig.h++>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <algorithm>

using namespace std;
using namespace libconfig;

static int server_socket = -1;
static pthread_t server_thread;
static volatile int server_running = 0;
static volatile int server_bind_status = 0;  // 0=unknown, 1=success, -1=failed
static pthread_mutex_t server_bind_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t server_bind_cond = PTHREAD_COND_INITIALIZER;
static int server_port = 5000;

// Error storage
static std::vector<std::string> error_log;
static std::mutex error_log_mutex;

// Config file path storage
static std::string config_file_path;
static std::mutex config_path_mutex;

// Simple HTTP response helper
static void send_response(int client_fd, int status_code, const char* status_text, const char* content_type, const char* body, size_t body_len, const char* content_disposition = NULL) {
    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len,
        content_disposition ? content_disposition : "");
    
    write(client_fd, response, len);
    if (body && body_len > 0) {
        write(client_fd, body, body_len);
    }
}

// Simple URL decode function
static string url_decode(const string& encoded) {
    string decoded;
    for (size_t i = 0; i < encoded.length(); i++) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            int value;
            if (sscanf(encoded.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
                decoded += (char)value;
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

static void send_file_response(int client_fd, const char* content_type, const char* content) {
    size_t len = strlen(content);
    send_response(client_fd, 200, "OK", content_type, content, len);
}

static void send_json_response(int client_fd, const char* json) {
    send_response(client_fd, 200, "OK", "application/json", json, strlen(json));
}

static void send_error(int client_fd, int code, const char* message) {
    char json[512];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message);
    send_response(client_fd, code, code == 404 ? "Not Found" : "Error", "application/json", json, strlen(json));
}

// Parse HTTP request - returns content length if available
static bool parse_request(int client_fd, char* method, char* path, size_t /* path_size */, size_t* content_length = NULL) {
    char buffer[4096];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) return false;
    
    buffer[n] = '\0';
    
    // Simple parsing - just get method and path
    if (sscanf(buffer, "%15s %1023s", method, path) != 2) {
        return false;
    }
    
    // Try to find Content-Length header
    if (content_length) {
        *content_length = 0;
        char* cl_header = strstr(buffer, "Content-Length:");
        if (cl_header) {
            sscanf(cl_header, "Content-Length: %zu", content_length);
        }
    }
    
    return true;
}

// Read HTTP request body (after headers)
static string read_request_body(int client_fd, size_t content_length) {
    string body;
    if (content_length == 0 || content_length > 10 * 1024 * 1024) {  // Limit to 10MB
        return body;
    }
    
    body.resize(content_length);
    size_t total_read = 0;
    while (total_read < content_length) {
        ssize_t n = read(client_fd, &body[total_read], content_length - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    
    if (total_read < content_length) {
        body.resize(total_read);
    }
    
    return body;
}

// Get channel status as JSON
static string get_channels_status_json() {
    std::stringstream json;
    json << "{\"device\":0,\"channels\":[";
    
    bool first = true;
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        for (int i = 0; i < dev->channel_count; i++) {
            channel_t* channel = dev->channels + i;
            freq_t* fparms = channel->freqlist + channel->freq_idx;
            float freq_mhz = fparms->frequency / 1000000.0;
            float signal_dbfs = level_to_dBFS(fparms->squelch.signal_level());
            float noise_dbfs = level_to_dBFS(fparms->squelch.noise_level());
            float snr = signal_dbfs - noise_dbfs;
            const char* status_str = (channel->axcindicate == SIGNAL) ? "signal" : 
                                    (channel->axcindicate == AFC_UP) ? "afc_up" :
                                    (channel->axcindicate == AFC_DOWN) ? "afc_down" : "no_signal";
            
            const char* label = fparms->label ? fparms->label : "";
            
            // Check if channel has file output and is currently recording
            bool is_recording = false;
            bool has_file_output = false;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->type == O_FILE && output->data && output->enabled) {
                    has_file_output = true;
                    file_data* fdata = (file_data*)(output->data);
                    // Recording if file is open and (has signal OR continuous recording)
                    if (fdata->f != NULL) {
                        // Check if continuous or if there's a signal
                        if (fdata->continuous || channel->axcindicate == SIGNAL) {
                            is_recording = true;
                        }
                    }
                }
            }
            
            if (!first) json << ",";
            first = false;
            
            float squelch_dbfs = level_to_dBFS(fparms->squelch.squelch_level());
            size_t ctcss_count = fparms->squelch.ctcss_count();
            
            json << "{\"channel\":" << i 
                 << ",\"frequency\":" << std::fixed << std::setprecision(3) << freq_mhz
                 << ",\"label\":\"" << label << "\""
                 << ",\"signal_level\":" << std::setprecision(1) << signal_dbfs
                 << ",\"noise_level\":" << noise_dbfs
                 << ",\"squelch_level\":" << squelch_dbfs
                 << ",\"snr\":" << snr
                 << ",\"ctcss_count\":" << ctcss_count
                 << ",\"status\":\"" << status_str << "\""
                 << ",\"has_file_output\":" << (has_file_output ? "true" : "false")
                 << ",\"is_recording\":" << (is_recording ? "true" : "false") << "}";
        }
    }
    
    json << "]}";
    return json.str();
}

// Get device info as JSON
static string get_device_info_json() {
    std::stringstream json;
    json << "{\"devices\":[";
    
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        if (d > 0) json << ",";
        
        const char* state_str;
        switch (dev->input->state) {
            case INPUT_RUNNING: state_str = "running"; break;
            case INPUT_FAILED: state_str = "failed"; break;
            case INPUT_STOPPED: state_str = "stopped"; break;
            case INPUT_DISABLED: state_str = "disabled"; break;
            case INPUT_INITIALIZED: state_str = "initialized"; break;
            default: state_str = "unknown"; break;
        }
        
        const char* mode_str = (dev->mode == R_SCAN) ? "scan" : "multichannel";
        
        json << "{\"device\":" << d
             << ",\"state\":\"" << state_str << "\""
             << ",\"mode\":\"" << mode_str << "\""
             << ",\"sample_rate\":" << dev->input->sample_rate
             << ",\"center_freq\":" << dev->input->centerfreq
             << "}";
    }
    
    json << "]}";
    return json.str();
}

// Get recordings list
static string get_recordings_json() {
    std::stringstream json;
    json << "{\"recordings\":[";
    
    // Find all recording directories from device channels
    vector<string> recording_dirs;
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        for (int i = 0; i < dev->channel_count; i++) {
            channel_t* channel = dev->channels + i;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->type == O_FILE && output->data) {
                    file_data* fdata = (file_data*)(output->data);
                    if (!fdata->basedir.empty()) {
                        bool found = false;
                        for (const auto& dir : recording_dirs) {
                            if (dir == fdata->basedir) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            recording_dirs.push_back(fdata->basedir);
                        }
                    }
                }
            }
        }
    }
    
    // Scan directories for recordings
    bool first = true;
    for (const auto& dir : recording_dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            
            string filepath = dir + "/" + entry->d_name;
            struct stat st;
            if (stat(filepath.c_str(), &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) continue;
            
            // Check if it's an audio file
            string name = entry->d_name;
            if (name.length() < 4) continue;
            string ext = name.substr(name.length() - 4);
            if (ext != ".mp3" && ext != ".raw") continue;
            
            if (!first) json << ",";
            first = false;
            
            struct tm* timeinfo = localtime(&st.st_mtime);
            char date_str[64];
            strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", timeinfo);
            
            json << "{\"filename\":\"" << name << "\""
                 << ",\"path\":\"" << filepath << "\""
                 << ",\"size\":" << st.st_size
                 << ",\"datetime\":\"" << date_str << "\""
                 << "}";
        }
        closedir(d);
    }
    
    json << "]}";
    return json.str();
}

// HTML content for web interface - read from file
static const char* get_html_content() {
    static char* html_content = NULL;
    static size_t html_size = 0;
    
    if (html_content != NULL) {
        return html_content;
    }
    
    // Try to read from file in source directory or current directory
    const char* paths[] = {
        "src/web_ui.html",
        "web_ui.html",
        "/usr/local/share/boondock_airband/web_ui.html",
        "/opt/boondock/airband/src/web_ui.html",
        NULL
    };
    
    FILE* f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    
    if (f) {
        fseek(f, 0, SEEK_END);
        html_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        html_content = (char*)XCALLOC(html_size + 1, sizeof(char));
        size_t read = fread(html_content, 1, html_size, f);
        html_content[read] = '\0';
        fclose(f);
        return html_content;
    }
    
    return NULL;
}

// CSS content for web interface - read from file
static const char* get_css_content() {
    static char* css_content = NULL;
    static size_t css_size = 0;
    
    if (css_content != NULL) {
        return css_content;
    }
    
    // Try to read from file in source directory or current directory
    const char* paths[] = {
        "src/web_ui.css",
        "web_ui.css",
        "/usr/local/share/boondock_airband/web_ui.css",
        "/opt/boondock/airband/src/web_ui.css",
        NULL
    };
    
    FILE* f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    
    if (f) {
        fseek(f, 0, SEEK_END);
        css_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        css_content = (char*)XCALLOC(css_size + 1, sizeof(char));
        size_t read = fread(css_content, 1, css_size, f);
        css_content[read] = '\0';
        fclose(f);
        return css_content;
    }
    
    return NULL;
}

// JavaScript content for web interface - read from file
static const char* get_js_content() {
    static char* js_content = NULL;
    static size_t js_size = 0;
    
    if (js_content != NULL) {
        return js_content;
    }
    
    // Try to read from file in source directory or current directory
    const char* paths[] = {
        "src/web_ui.js",
        "web_ui.js",
        "/usr/local/share/boondock_airband/web_ui.js",
        "/opt/boondock/airband/src/web_ui.js",
        NULL
    };
    
    FILE* f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    
    if (f) {
        fseek(f, 0, SEEK_END);
        js_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        js_content = (char*)XCALLOC(js_size + 1, sizeof(char));
        size_t read = fread(js_content, 1, js_size, f);
        js_content[read] = '\0';
        fclose(f);
        return js_content;
    }
    
    return NULL;
}

// Spectrum analyzer JavaScript content - read from file
static const char* get_spectrum_js_content() {
    static char* spectrum_js_content = NULL;
    static size_t spectrum_js_size = 0;
    
    if (spectrum_js_content != NULL) {
        return spectrum_js_content;
    }
    
    // Try to read from file in source directory or current directory
    const char* paths[] = {
        "src/web_spectrum.js",
        "web_spectrum.js",
        "/usr/local/share/boondock_airband/web_spectrum.js",
        "/opt/boondock/airband/src/web_spectrum.js",
        NULL
    };
    
    FILE* f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    
    if (f) {
        fseek(f, 0, SEEK_END);
        spectrum_js_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        spectrum_js_content = (char*)XCALLOC(spectrum_js_size + 1, sizeof(char));
        size_t read = fread(spectrum_js_content, 1, spectrum_js_size, f);
        spectrum_js_content[read] = '\0';
        fclose(f);
        return spectrum_js_content;
    }
    
    return NULL;
    
    // Fallback: return a simple HTML page with embedded JavaScript
    static const char* fallback_html = 
        "<!DOCTYPE html><html><head><title>Boondock Airband</title>"
        "<style>body{font-family:sans-serif;margin:20px;}"
        "table{border-collapse:collapse;width:100%;}th,td{padding:8px;border:1px solid #ddd;}"
        ".btn{padding:8px 16px;background:#0066cc;color:white;border:none;border-radius:4px;cursor:pointer;}"
        "</style></head><body><h1>Boondock Airband Capture</h1>"
        "<div id=\"content\"><p>Loading...</p></div>"
        "<script>"
        "function updateStatus(){fetch('/api/status').then(r=>r.json()).then(d=>{"
        "var t=document.getElementById('content');"
        "t.innerHTML='<h2>Channel Status</h2><table><tr><th>Channel</th><th>Frequency</th><th>Signal</th><th>SNR</th><th>Status</th></tr>';"
        "d.channels.forEach(function(c){"
        "t.innerHTML+='<tr><td>'+(c.label||'Ch '+c.channel)+'</td><td>'+c.frequency+' MHz</td><td>'+c.signal_level.toFixed(1)+' dB</td><td>'+c.snr.toFixed(1)+' dB</td><td>'+c.status+'</td></tr>';"
        "});t.innerHTML+='</table>';"
        "});}"
        "setInterval(updateStatus,1000);updateStatus();"
        "</script></body></html>";
    
    return fallback_html;
}

// Get errors as JSON
static string get_errors_json() {
    std::lock_guard<std::mutex> lock(error_log_mutex);
    stringstream json;
    json << "{\"errors\":[";
    bool first = true;
    for (const auto& err : error_log) {
        if (!first) json << ",";
        first = false;
        // Escape JSON string
        json << "\"";
        for (char c : err) {
            if (c == '"') json << "\\\"";
            else if (c == '\\') json << "\\\\";
            else if (c == '\n') json << "\\n";
            else if (c == '\r') json << "\\r";
            else if (c == '\t') json << "\\t";
            else json << c;
        }
        json << "\"";
    }
    json << "]}";
    return json.str();
}

// Get config file info as JSON
static string get_config_info_json() {
    std::stringstream json;
    const char* config_path = web_server_get_config_path();
    json << "{\"config_path\":\"" << config_path << "\"}";
    return json.str();
}

// Get full channel details from config file
static string get_channels_full_json() {
    std::stringstream json;
    const char* config_path = web_server_get_config_path();
    
    try {
        Config config;
        config.readFile(config_path);
        Setting& root = config.getRoot();
        
        if (!root.exists("devices")) {
            json << "{\"devices\":[]}";
            return json.str();
        }
        
        Setting& devs = root["devices"];
        json << "{\"devices\":[";
        
        for (int d = 0; d < devs.getLength(); d++) {
            if (d > 0) json << ",";
            Setting& dev = devs[d];
            json << "{\"device\":" << d;
            
            bool disabled = dev.exists("disable") && (bool)dev["disable"];
            json << ",\"enabled\":" << (disabled ? "false" : "true");
            
            if (dev.exists("type")) {
                json << ",\"type\":\"" << (const char*)dev["type"] << "\"";
            }
            if (dev.exists("mode")) {
                json << ",\"mode\":\"" << (const char*)dev["mode"] << "\"";
            }
            if (dev.exists("sample_rate")) {
                if (dev["sample_rate"].getType() == Setting::TypeInt) {
                    json << ",\"sample_rate\":" << (int)dev["sample_rate"];
                } else if (dev["sample_rate"].getType() == Setting::TypeFloat) {
                    json << ",\"sample_rate\":" << (double)dev["sample_rate"];
                } else {
                    json << ",\"sample_rate\":\"" << (const char*)dev["sample_rate"] << "\"";
                }
            }
            if (dev.exists("centerfreq")) {
                if (dev["centerfreq"].getType() == Setting::TypeInt) {
                    json << ",\"centerfreq\":" << (int)dev["centerfreq"];
                } else if (dev["centerfreq"].getType() == Setting::TypeFloat) {
                    json << ",\"centerfreq\":" << (double)dev["centerfreq"];
                } else {
                    json << ",\"centerfreq\":\"" << (const char*)dev["centerfreq"] << "\"";
                }
            }
            if (dev.exists("correction")) {
                if (dev["correction"].getType() == Setting::TypeInt) {
                    json << ",\"correction\":" << (int)dev["correction"];
                } else if (dev["correction"].getType() == Setting::TypeFloat) {
                    json << ",\"correction\":" << (double)dev["correction"];
                }
            }
            if (dev.exists("tau")) {
                json << ",\"tau\":" << (int)dev["tau"];
            }
            
            // Device-specific fields
            if (dev.exists("device_string")) {
                json << ",\"device_string\":\"" << (const char*)dev["device_string"] << "\"";
            }
            if (dev.exists("index")) {
                json << ",\"index\":" << (int)dev["index"];
            }
            if (dev.exists("serial")) {
                json << ",\"serial\":\"" << (const char*)dev["serial"] << "\"";
            }
            if (dev.exists("gain")) {
                if (dev["gain"].getType() == Setting::TypeInt) {
                    json << ",\"gain\":" << (int)dev["gain"];
                } else if (dev["gain"].getType() == Setting::TypeFloat) {
                    json << ",\"gain\":" << (double)dev["gain"];
                } else {
                    json << ",\"gain\":\"" << (const char*)dev["gain"] << "\"";
                }
            }
            if (dev.exists("buffers")) {
                json << ",\"buffers\":" << (int)dev["buffers"];
            }
            if (dev.exists("num_buffers")) {
                json << ",\"num_buffers\":" << (int)dev["num_buffers"];
            }
            if (dev.exists("channel")) {
                json << ",\"channel\":" << (int)dev["channel"];
            }
            if (dev.exists("antenna")) {
                json << ",\"antenna\":\"" << (const char*)dev["antenna"] << "\"";
            }
            if (dev.exists("filepath")) {
                json << ",\"filepath\":\"" << (const char*)dev["filepath"] << "\"";
            }
            if (dev.exists("speedup_factor")) {
                if (dev["speedup_factor"].getType() == Setting::TypeInt) {
                    json << ",\"speedup_factor\":" << (int)dev["speedup_factor"];
                } else if (dev["speedup_factor"].getType() == Setting::TypeFloat) {
                    json << ",\"speedup_factor\":" << (double)dev["speedup_factor"];
                }
            }
            
            if (dev.exists("channels")) {
                Setting& chans = dev["channels"];
                json << ",\"channels\":[";
                
                int enabled_count = 0;
                for (int c = 0; c < chans.getLength(); c++) {
                    bool channel_disabled = chans[c].exists("disable") && (bool)chans[c]["disable"];
                    if (channel_disabled) continue;
                    
                    if (enabled_count > 0) json << ",";
                    enabled_count++;
                    
                    json << "{";
                    json << "\"channel_index\":" << c;
                    json << ",\"enabled\":true";
                    
                    if (chans[c].exists("freq")) {
                        json << ",\"freq\":" << (double)chans[c]["freq"];
                    }
                    if (chans[c].exists("label")) {
                        json << ",\"label\":\"" << (const char*)chans[c]["label"] << "\"";
                    }
                    if (chans[c].exists("modulation")) {
                        json << ",\"modulation\":\"" << (const char*)chans[c]["modulation"] << "\"";
                    }
                    if (chans[c].exists("highpass")) {
                        json << ",\"highpass\":" << (int)chans[c]["highpass"];
                    }
                    if (chans[c].exists("lowpass")) {
                        json << ",\"lowpass\":" << (int)chans[c]["lowpass"];
                    }
                    if (chans[c].exists("bandwidth")) {
                        if (chans[c]["bandwidth"].getType() == Setting::TypeInt) {
                            json << ",\"bandwidth\":" << (int)chans[c]["bandwidth"];
                        } else if (chans[c]["bandwidth"].getType() == Setting::TypeFloat) {
                            json << ",\"bandwidth\":" << (double)chans[c]["bandwidth"];
                        }
                    }
                    if (chans[c].exists("squelch_threshold")) {
                        if (chans[c]["squelch_threshold"].getType() == Setting::TypeInt) {
                            json << ",\"squelch_threshold\":" << (int)chans[c]["squelch_threshold"];
                        }
                    }
                    if (chans[c].exists("squelch_snr_threshold")) {
                        if (chans[c]["squelch_snr_threshold"].getType() == Setting::TypeFloat) {
                            json << ",\"squelch_snr_threshold\":" << (double)chans[c]["squelch_snr_threshold"];
                        } else if (chans[c]["squelch_snr_threshold"].getType() == Setting::TypeInt) {
                            json << ",\"squelch_snr_threshold\":" << (int)chans[c]["squelch_snr_threshold"];
                        }
                    }
                    if (chans[c].exists("ampfactor")) {
                        json << ",\"ampfactor\":" << (double)chans[c]["ampfactor"];
                    }
                    if (chans[c].exists("afc")) {
                        json << ",\"afc\":" << (int)chans[c]["afc"];
                    }
                    if (chans[c].exists("notch")) {
                        if (chans[c]["notch"].getType() == Setting::TypeFloat) {
                            json << ",\"notch\":" << (double)chans[c]["notch"];
                        }
                    }
                    if (chans[c].exists("notch_q")) {
                        json << ",\"notch_q\":" << (double)chans[c]["notch_q"];
                    }
                    if (chans[c].exists("ctcss")) {
                        if (chans[c]["ctcss"].getType() == Setting::TypeFloat) {
                            json << ",\"ctcss\":" << (double)chans[c]["ctcss"];
                        }
                    }
                    
                    // Outputs
                    if (chans[c].exists("outputs")) {
                        Setting& outputs = chans[c]["outputs"];
                        json << ",\"outputs\":[";
                        for (int o = 0; o < outputs.getLength(); o++) {
                            if (o > 0) json << ",";
                            json << "{";
                            json << "\"output_index\":" << o;
                            
                            bool out_disabled = outputs[o].exists("disable") && (bool)outputs[o]["disable"];
                            json << ",\"enabled\":" << (out_disabled ? "false" : "true");
                            
                            if (outputs[o].exists("type")) {
                                json << ",\"type\":\"" << (const char*)outputs[o]["type"] << "\"";
                            }
                            if (outputs[o].exists("continuous")) {
                                json << ",\"continuous\":" << ((bool)outputs[o]["continuous"] ? "true" : "false");
                            }
                            if (outputs[o].exists("directory")) {
                                json << ",\"directory\":\"" << (const char*)outputs[o]["directory"] << "\"";
                            }
                            if (outputs[o].exists("filename_template")) {
                                json << ",\"filename_template\":\"" << (const char*)outputs[o]["filename_template"] << "\"";
                            }
                            if (outputs[o].exists("dest_address")) {
                                json << ",\"dest_address\":\"" << (const char*)outputs[o]["dest_address"] << "\"";
                            }
                            if (outputs[o].exists("dest_port")) {
                                json << ",\"dest_port\":" << (int)outputs[o]["dest_port"];
                            }
                            if (outputs[o].exists("udp_headers")) {
                                json << ",\"udp_headers\":" << ((bool)outputs[o]["udp_headers"] ? "true" : "false");
                            }
                            if (outputs[o].exists("udp_chunking")) {
                                json << ",\"udp_chunking\":" << ((bool)outputs[o]["udp_chunking"] ? "true" : "false");
                            }
                            json << "}";
                        }
                        json << "]";
                    }
                    
                    json << "}";
                }
                
                json << "]";
            } else {
                json << ",\"channels\":[]";
            }
            
            json << "}";
        }
        
        json << "]}";
    } catch (const std::exception& e) {
        json.str("");
        json << "{\"error\":\"Failed to read config: " << e.what() << "\"}";
    }
    
    return json.str();
}

// Handle API requests
static void handle_api_request(int client_fd, const char* path, const char* method, size_t content_length) {
    if (strcmp(path, "/api/status") == 0) {
        string json = get_channels_status_json();
        send_json_response(client_fd, json.c_str());
    } else if (strcmp(path, "/api/device") == 0) {
        if (strcmp(method, "GET") == 0) {
            // Try to get full device details from config, fallback to runtime info
            string json = get_channels_full_json();
            // If we got channels, extract device info from it
            if (json.find("\"devices\"") != string::npos) {
                send_json_response(client_fd, json.c_str());
            } else {
                // Fallback to runtime device info
                json = get_device_info_json();
                send_json_response(client_fd, json.c_str());
            }
        } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
            // Save device configuration
            send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Device configuration saved. Restart required.\"}");
        } else {
            send_error(client_fd, 405, "Method not allowed");
        }
    } else if (strncmp(path, "/api/spectrum", 13) == 0) {
        // Spectrum analyzer endpoint: /api/spectrum or /api/spectrum/{device_index}
        int device_idx = -1;
        if (strlen(path) > 13) {
            if (sscanf(path, "/api/spectrum/%d", &device_idx) != 1) {
                device_idx = -1;
            }
        }
        
        if (device_idx < 0 || device_idx >= device_count) {
            // Return list of available devices
            std::stringstream json;
            json << "{\"devices\":[";
            for (int d = 0; d < device_count; d++) {
                if (d > 0) json << ",";
                device_t* dev = devices + d;
                json << "{\"device\":" << d
                     << ",\"sample_rate\":" << dev->input->sample_rate
                     << ",\"center_freq\":" << dev->input->centerfreq
                     << ",\"spectrum_size\":" << dev->spectrum.size << "}";
            }
            json << "]}";
            send_json_response(client_fd, json.str().c_str());
        } else {
            // Return spectrum data for specific device
            device_t* dev = devices + device_idx;
            pthread_mutex_lock(&dev->spectrum.mutex);
            
            std::stringstream json;
            json << "{\"device\":" << device_idx
                 << ",\"sample_rate\":" << dev->input->sample_rate
                 << ",\"center_freq\":" << dev->input->centerfreq
                 << ",\"spectrum_size\":" << dev->spectrum.size
                 << ",\"last_update\":" << dev->spectrum.last_update
                 << ",\"data\":[";
            
            for (size_t i = 0; i < dev->spectrum.size; i++) {
                if (i > 0) json << ",";
                json << std::fixed << std::setprecision(2) << dev->spectrum.magnitude[i];
            }
            json << "]}";
            
            pthread_mutex_unlock(&dev->spectrum.mutex);
            send_json_response(client_fd, json.str().c_str());
        }
    } else if (strcmp(path, "/api/recordings") == 0) {
        string json = get_recordings_json();
        send_json_response(client_fd, json.c_str());
    } else if (strcmp(path, "/api/errors") == 0) {
        string json = get_errors_json();
        send_json_response(client_fd, json.c_str());
    } else if (strcmp(path, "/api/config/info") == 0) {
        string json = get_config_info_json();
        send_json_response(client_fd, json.c_str());
    } else if (strcmp(path, "/api/config/download") == 0) {
        // Download config file
        const char* config_path = web_server_get_config_path();
        FILE* f = fopen(config_path, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            size_t size = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* buffer = (char*)XCALLOC(size, sizeof(char));
            fread(buffer, 1, size, f);
            fclose(f);
            
            // Send as text/plain with filename
            char response[8192];
            int len = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Disposition: attachment; filename=\"boondock_airband.conf\"\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n",
                size);
            write(client_fd, response, len);
            write(client_fd, buffer, size);
            free(buffer);
        } else {
            send_error(client_fd, 404, "Config file not found");
        }
    } else if (strcmp(path, "/api/config/upload") == 0 && strcmp(method, "POST") == 0) {
        // Upload config file - read from request body
        const char* config_path = web_server_get_config_path();
        
        if (content_length > 0 && content_length < 10 * 1024 * 1024) {  // Limit to 10MB
            string body = read_request_body(client_fd, content_length);
            
            if (!body.empty()) {
                // Write to config file
                FILE* f = fopen(config_path, "w");
                if (f) {
                    fwrite(body.c_str(), 1, body.length(), f);
                    fclose(f);
                    send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Config file uploaded successfully\"}");
                } else {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "Failed to write config file: %s", strerror(errno));
                    send_error(client_fd, 500, err_msg);
                }
            } else {
                send_error(client_fd, 400, "Empty file content");
            }
        } else {
            send_error(client_fd, 400, "Config file too large or invalid size");
        }
    } else if (strcmp(path, "/api/config/path") == 0 && strcmp(method, "POST") == 0) {
        // Set config file path - read JSON from body
        if (content_length > 0 && content_length < 2048) {
            string body = read_request_body(client_fd, content_length);
            
            if (!body.empty()) {
                // Simple JSON parsing for "config_path": "value"
                char path_value[1024] = {0};
                if (sscanf(body.c_str(), "{\"config_path\":\"%1023[^\"]\"}", path_value) == 1) {
                    web_server_set_config_path(path_value);
                    send_json_response(client_fd, "{\"status\":\"success\"}");
                } else {
                    send_error(client_fd, 400, "Invalid JSON format");
                }
            } else {
                send_error(client_fd, 400, "Empty request body");
            }
        } else {
            send_error(client_fd, 400, "Invalid request size");
        }
    } else if (strcmp(path, "/api/restart") == 0) {
        // Signal restart (set flag, actual restart handled by main thread)
        send_json_response(client_fd, "{\"status\":\"restart_requested\"}");
    } else if (strcmp(path, "/api/apply") == 0 && strcmp(method, "POST") == 0) {
        // Apply configuration changes and reload
        // Read request body if present (increase limit to handle larger JSON)
        if (content_length > 0) {
            if (content_length < 10240) {  // Increased to 10KB
                string body = read_request_body(client_fd, content_length);
                // Body is read but not parsed - changes are already saved to config file
            } else {
                send_error(client_fd, 400, "Request body too large");
                return;
            }
        }
        
        // Trigger configuration reload using the web_server_trigger_reload function
        // This sets the do_reload flag which is checked by the main thread
        if (web_server_trigger_reload() == 0) {
            log(LOG_INFO, "Configuration reload triggered via do_reload flag\n");
            send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Configuration reload triggered. Changes will be applied.\"}");
        } else {
            log(LOG_ERR, "Failed to trigger configuration reload\n");
            send_error(client_fd, 500, "Failed to trigger configuration reload");
        }
    } else if (strncmp(path, "/api/channels", 13) == 0) {
        // Channel management endpoints
        if (strcmp(path, "/api/channels") == 0 && strcmp(method, "GET") == 0) {
            string json = get_channels_full_json();
            send_json_response(client_fd, json.c_str());
        } else if (strcmp(path, "/api/channels") == 0 && strcmp(method, "POST") == 0) {
            // Add new channel - read JSON body and save to config
            if (content_length == 0 || content_length > 10240) {
                send_error(client_fd, 400, "Invalid request body");
                return;
            }
            
            string body = read_request_body(client_fd, content_length);
            if (body.empty()) {
                send_error(client_fd, 400, "Empty request body");
                return;
            }
            
            const char* config_path = web_server_get_config_path();
            try {
                Config config;
                config.readFile(config_path);
                Setting& root = config.getRoot();
                
                // Extract device_index from JSON
                int device_idx = -1;
                const char* dev_pos = strstr(body.c_str(), "\"device_index\"");
                if (dev_pos && sscanf(dev_pos, "\"device_index\":%d", &device_idx) == 1) {
                    // device_idx is set
                } else {
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Missing device_index\"}");
                    return;
                }
                
                if (!root.exists("devices") || device_idx < 0 || device_idx >= root["devices"].getLength()) {
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid device index\"}");
                    return;
                }
                
                Setting& dev = root["devices"][device_idx];
                if (!dev.exists("channels")) {
                    dev.add("channels", Setting::TypeList);
                }
                
                Setting& channels = dev["channels"];
                Setting& new_channel = channels.add(Setting::TypeGroup);
                
                // Parse and set channel fields (same as PUT handler)
                char label[256] = {0};
                double freq = 0;
                char modulation[16] = "am";
                int highpass = 100, lowpass = 2500, bandwidth = 5000, afc = 0;
                double ampfactor = 1.0, squelch_threshold = 0, squelch_snr_threshold = 0;
                double notch = 0, notch_q = 10.0, ctcss = 0;
                
                // Parse label
                const char* label_pos = strstr(body.c_str(), "\"label\"");
                if (label_pos && sscanf(label_pos, "\"label\":\"%255[^\"]\"", label) == 1) {
                    new_channel.add("label", Setting::TypeString) = label;
                }
                
                // Parse frequency
                const char* freq_pos = strstr(body.c_str(), "\"freq\"");
                if (freq_pos && sscanf(freq_pos, "\"freq\":%lf", &freq) == 1) {
                    if (dev.exists("mode") && strcmp(dev["mode"], "scan") == 0) {
                        Setting& freqs = new_channel.add("freqs", Setting::TypeList);
                        freqs.add(Setting::TypeInt) = (int)(freq * 1000000);
                    } else {
                        new_channel.add("freq", Setting::TypeInt) = (int)(freq * 1000000);
                    }
                }
                
                // Parse modulation
                const char* mod_pos = strstr(body.c_str(), "\"modulation\"");
                if (mod_pos && sscanf(mod_pos, "\"modulation\":\"%15[^\"]\"", modulation) == 1) {
                    new_channel.add("modulation", Setting::TypeString) = modulation;
                }
                
                // Parse other fields - only add if they have valid non-default values
                const char* hp_pos = strstr(body.c_str(), "\"highpass\"");
                if (hp_pos && strstr(hp_pos, ":null") == NULL) {
                    if (sscanf(hp_pos, "\"highpass\":%d", &highpass) == 1 && highpass > 0) {
                        new_channel.add("highpass", Setting::TypeInt) = highpass;
                    }
                }
                
                const char* lp_pos = strstr(body.c_str(), "\"lowpass\"");
                if (lp_pos && strstr(lp_pos, ":null") == NULL) {
                    if (sscanf(lp_pos, "\"lowpass\":%d", &lowpass) == 1 && lowpass > 0) {
                        new_channel.add("lowpass", Setting::TypeInt) = lowpass;
                    }
                }
                
                const char* bw_pos = strstr(body.c_str(), "\"bandwidth\"");
                if (bw_pos && strstr(bw_pos, ":null") == NULL) {
                    if (sscanf(bw_pos, "\"bandwidth\":%d", &bandwidth) == 1 && bandwidth > 0) {
                        new_channel.add("bandwidth", Setting::TypeInt) = bandwidth;
                    }
                }
                
                const char* amp_pos = strstr(body.c_str(), "\"ampfactor\"");
                if (amp_pos && strstr(amp_pos, ":null") == NULL) {
                    if (sscanf(amp_pos, "\"ampfactor\":%lf", &ampfactor) == 1 && ampfactor != 1.0) {
                        new_channel.add("ampfactor", Setting::TypeFloat) = ampfactor;
                    }
                }
                
                const char* sq_pos = strstr(body.c_str(), "\"squelch_threshold\"");
                if (sq_pos && strstr(sq_pos, ":null") == NULL) {
                    if (sscanf(sq_pos, "\"squelch_threshold\":%lf", &squelch_threshold) == 1 && squelch_threshold != 0) {
                        new_channel.add("squelch_threshold", Setting::TypeInt) = (int)squelch_threshold;
                    }
                }
                
                const char* snr_pos = strstr(body.c_str(), "\"squelch_snr_threshold\"");
                if (snr_pos && strstr(snr_pos, ":null") == NULL) {
                    if (sscanf(snr_pos, "\"squelch_snr_threshold\":%lf", &squelch_snr_threshold) == 1 && squelch_snr_threshold != 0) {
                        new_channel.add("squelch_snr_threshold", Setting::TypeFloat) = squelch_snr_threshold;
                    }
                }
                
                const char* afc_pos = strstr(body.c_str(), "\"afc\"");
                if (afc_pos && strstr(afc_pos, ":null") == NULL) {
                    if (sscanf(afc_pos, "\"afc\":%d", &afc) == 1 && afc > 0) {
                        new_channel.add("afc", Setting::TypeInt) = afc;
                    }
                }
                
                const char* notch_pos = strstr(body.c_str(), "\"notch\"");
                if (notch_pos && strstr(notch_pos, ":null") == NULL) {
                    if (sscanf(notch_pos, "\"notch\":%lf", &notch) == 1 && notch > 0) {
                        new_channel.add("notch", Setting::TypeFloat) = notch;
                    }
                }
                
                const char* notchq_pos = strstr(body.c_str(), "\"notch_q\"");
                if (notchq_pos && strstr(notchq_pos, ":null") == NULL) {
                    if (sscanf(notchq_pos, "\"notch_q\":%lf", &notch_q) == 1 && notch_q != 10.0) {
                        new_channel.add("notch_q", Setting::TypeFloat) = notch_q;
                    }
                }
                
                const char* ctcss_pos = strstr(body.c_str(), "\"ctcss\"");
                if (ctcss_pos && strstr(ctcss_pos, ":null") == NULL) {
                    if (sscanf(ctcss_pos, "\"ctcss\":%lf", &ctcss) == 1 && ctcss > 0) {
                        new_channel.add("ctcss", Setting::TypeFloat) = ctcss;
                    }
                }
                
                // Parse enabled
                const char* enabled_pos = strstr(body.c_str(), "\"enabled\"");
                if (enabled_pos && strstr(enabled_pos, ":false") != NULL) {
                    new_channel.add("disable", Setting::TypeBoolean) = true;
                }
                
                // Add outputs - create a basic file output if outputs array exists
                const char* outputs_pos = strstr(body.c_str(), "\"outputs\"");
                if (outputs_pos) {
                    new_channel.add("outputs", Setting::TypeList);
                    // For now, add a placeholder - full output parsing would be more complex
                    // The frontend should send complete output configuration
                } else {
                    // Add default file output
                    Setting& outputs = new_channel.add("outputs", Setting::TypeList);
                    Setting& file_out = outputs.add(Setting::TypeGroup);
                    file_out.add("type", Setting::TypeString) = "file";
                    file_out.add("directory", Setting::TypeString) = "recordings";
                    file_out.add("filename_template", Setting::TypeString) = "${label}_${start:%Y%m%d}_${start:%H}.mp3";
                }
                
                // Write config back to file
                config.writeFile(config_path);
                log(LOG_INFO, "New channel added to device %d in config file\n", device_idx);
                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel added successfully. Click 'Apply Changes' to reload.\"}");
            } catch (const FileIOException& fioex) {
                log(LOG_ERR, "I/O error adding channel: %s\n", fioex.what());
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while updating config file\"}");
            } catch (const ParseException& pex) {
                log(LOG_ERR, "Parse error adding channel: %s\n", pex.what());
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Parse error in config file\"}");
            } catch (const std::exception& ex) {
                log(LOG_ERR, "Error adding channel: %s\n", ex.what());
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Error adding channel\"}");
            }
        } else {
            // Parse device and channel from path like /api/channels/0/1
            int device_idx = -1, channel_idx = -1;
            if (sscanf(path, "/api/channels/%d/%d", &device_idx, &channel_idx) == 2) {
                if (strcmp(method, "GET") == 0) {
                    // Get specific channel
                    send_json_response(client_fd, "{\"status\":\"success\"}");
                } else if (strcmp(method, "PUT") == 0) {
                    // Update channel - read JSON body and save to config
                    if (content_length == 0 || content_length > 10240) {
                        send_error(client_fd, 400, "Invalid request body");
                        return;
                    }
                    
                    string body = read_request_body(client_fd, content_length);
                    if (body.empty()) {
                        send_error(client_fd, 400, "Empty request body");
                        return;
                    }
                    
                    const char* config_path = web_server_get_config_path();
                    try {
                        Config config;
                        config.readFile(config_path);
                        Setting& root = config.getRoot();
                        
                        if (!root.exists("devices") || device_idx < 0 || device_idx >= root["devices"].getLength()) {
                            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid device index\"}");
                            return;
                        }
                        
                        Setting& dev = root["devices"][device_idx];
                        if (!dev.exists("channels") || channel_idx < 0 || channel_idx >= dev["channels"].getLength()) {
                            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid channel index\"}");
                            return;
                        }
                        
                        Setting& channel = dev["channels"][channel_idx];
                        
                        // Parse JSON and update channel settings
                        // Simple JSON parsing - extract key values
                        char label[256] = {0};
                        double freq = 0;
                        char modulation[16] = {0};
                        int highpass = -1, lowpass = -1, bandwidth = 0, afc = 0;
                        double ampfactor = 1.0, squelch_threshold = 0, squelch_snr_threshold = 0;
                        double notch = 0, notch_q = 10.0, ctcss = 0;
                        
                        // Parse label
                        const char* label_pos = strstr(body.c_str(), "\"label\"");
                        if (label_pos && sscanf(label_pos, "\"label\":\"%255[^\"]\"", label) == 1) {
                            if (channel.exists("label")) channel["label"] = label;
                            else channel.add("label", Setting::TypeString) = label;
                        }
                        
                        // Parse frequency
                        const char* freq_pos = strstr(body.c_str(), "\"freq\"");
                        if (freq_pos && sscanf(freq_pos, "\"freq\":%lf", &freq) == 1) {
                            if (dev.exists("mode") && strcmp(dev["mode"], "scan") == 0) {
                                // Scan mode - update freqs array
                                if (channel.exists("freqs") && channel["freqs"].getLength() > 0) {
                                    channel["freqs"][0] = (int)(freq * 1000000);
                                }
                            } else {
                                // Multichannel mode
                                if (channel.exists("freq")) channel["freq"] = (int)(freq * 1000000);
                                else channel.add("freq", Setting::TypeInt) = (int)(freq * 1000000);
                            }
                        }
                        
                        // Parse modulation
                        const char* mod_pos = strstr(body.c_str(), "\"modulation\"");
                        if (mod_pos && sscanf(mod_pos, "\"modulation\":\"%15[^\"]\"", modulation) == 1) {
                            if (channel.exists("modulation")) channel["modulation"] = modulation;
                            else channel.add("modulation", Setting::TypeString) = modulation;
                        }
                        
                        // Parse highpass - remove if -1 or 0, update if > 0
                        const char* hp_pos = strstr(body.c_str(), "\"highpass\"");
                        if (hp_pos) {
                            if (sscanf(hp_pos, "\"highpass\":%d", &highpass) == 1) {
                                if (highpass > 0) {
                                    if (channel.exists("highpass")) channel["highpass"] = highpass;
                                    else channel.add("highpass", Setting::TypeInt) = highpass;
                                } else {
                                    // Remove if blanked out (0 or -1)
                                    if (channel.exists("highpass")) {
                                        channel.remove("highpass");
                                    }
                                }
                            } else if (strstr(hp_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("highpass")) {
                                    channel.remove("highpass");
                                }
                            }
                        }
                        
                        // Parse lowpass - remove if -1 or 0, update if > 0
                        const char* lp_pos = strstr(body.c_str(), "\"lowpass\"");
                        if (lp_pos) {
                            if (sscanf(lp_pos, "\"lowpass\":%d", &lowpass) == 1) {
                                if (lowpass > 0) {
                                    if (channel.exists("lowpass")) channel["lowpass"] = lowpass;
                                    else channel.add("lowpass", Setting::TypeInt) = lowpass;
                                } else {
                                    // Remove if blanked out (0 or -1)
                                    if (channel.exists("lowpass")) {
                                        channel.remove("lowpass");
                                    }
                                }
                            } else if (strstr(lp_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("lowpass")) {
                                    channel.remove("lowpass");
                                }
                            }
                        }
                        
                        // Parse bandwidth - remove if 0, update if > 0
                        const char* bw_pos = strstr(body.c_str(), "\"bandwidth\"");
                        if (bw_pos) {
                            if (sscanf(bw_pos, "\"bandwidth\":%d", &bandwidth) == 1) {
                                if (bandwidth > 0) {
                                    if (channel.exists("bandwidth")) channel["bandwidth"] = bandwidth;
                                    else channel.add("bandwidth", Setting::TypeInt) = bandwidth;
                                } else {
                                    // Remove if blanked out (0)
                                    if (channel.exists("bandwidth")) {
                                        channel.remove("bandwidth");
                                    }
                                }
                            } else if (strstr(bw_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("bandwidth")) {
                                    channel.remove("bandwidth");
                                }
                            }
                        }
                        
                        // Parse ampfactor - only update if explicitly set (not default 1.0)
                        const char* amp_pos = strstr(body.c_str(), "\"ampfactor\"");
                        if (amp_pos) {
                            if (sscanf(amp_pos, "\"ampfactor\":%lf", &ampfactor) == 1) {
                                if (ampfactor != 1.0) {
                                    if (channel.exists("ampfactor")) channel["ampfactor"] = ampfactor;
                                    else channel.add("ampfactor", Setting::TypeFloat) = ampfactor;
                                } else {
                                    // Remove if set to default (1.0)
                                    if (channel.exists("ampfactor")) {
                                        channel.remove("ampfactor");
                                    }
                                }
                            } else if (strstr(amp_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("ampfactor")) {
                                    channel.remove("ampfactor");
                                }
                            }
                        }
                        
                        // Parse squelch_threshold - remove if 0 or null
                        const char* sq_pos = strstr(body.c_str(), "\"squelch_threshold\"");
                        if (sq_pos) {
                            if (sscanf(sq_pos, "\"squelch_threshold\":%lf", &squelch_threshold) == 1) {
                                if (squelch_threshold != 0) {
                                    if (channel.exists("squelch_threshold")) channel["squelch_threshold"] = (int)squelch_threshold;
                                    else channel.add("squelch_threshold", Setting::TypeInt) = (int)squelch_threshold;
                                } else {
                                    // Remove if blanked out (0)
                                    if (channel.exists("squelch_threshold")) {
                                        channel.remove("squelch_threshold");
                                    }
                                }
                            } else if (strstr(sq_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("squelch_threshold")) {
                                    channel.remove("squelch_threshold");
                                }
                            }
                        }
                        
                        // Parse squelch_snr_threshold - remove if 0 or null
                        const char* snr_pos = strstr(body.c_str(), "\"squelch_snr_threshold\"");
                        if (snr_pos) {
                            if (sscanf(snr_pos, "\"squelch_snr_threshold\":%lf", &squelch_snr_threshold) == 1) {
                                if (squelch_snr_threshold != 0) {
                                    if (channel.exists("squelch_snr_threshold")) channel["squelch_snr_threshold"] = squelch_snr_threshold;
                                    else channel.add("squelch_snr_threshold", Setting::TypeFloat) = squelch_snr_threshold;
                                } else {
                                    // Remove if blanked out (0)
                                    if (channel.exists("squelch_snr_threshold")) {
                                        channel.remove("squelch_snr_threshold");
                                    }
                                }
                            } else if (strstr(snr_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("squelch_snr_threshold")) {
                                    channel.remove("squelch_snr_threshold");
                                }
                            }
                        }
                        
                        // Parse afc - remove if 0 (disabled), update if > 0
                        const char* afc_pos = strstr(body.c_str(), "\"afc\"");
                        if (afc_pos) {
                            if (sscanf(afc_pos, "\"afc\":%d", &afc) == 1) {
                                if (afc > 0) {
                                    if (channel.exists("afc")) channel["afc"] = afc;
                                    else channel.add("afc", Setting::TypeInt) = afc;
                                } else {
                                    // Remove if disabled (0)
                                    if (channel.exists("afc")) {
                                        channel.remove("afc");
                                    }
                                }
                            } else if (strstr(afc_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("afc")) {
                                    channel.remove("afc");
                                }
                            }
                        }
                        
                        // Parse notch - remove if 0 or null, update if > 0
                        const char* notch_pos = strstr(body.c_str(), "\"notch\"");
                        if (notch_pos) {
                            if (sscanf(notch_pos, "\"notch\":%lf", &notch) == 1) {
                                if (notch > 0) {
                                    if (channel.exists("notch")) channel["notch"] = notch;
                                    else channel.add("notch", Setting::TypeFloat) = notch;
                                } else {
                                    // Remove if blanked out (0)
                                    if (channel.exists("notch")) {
                                        channel.remove("notch");
                                    }
                                }
                            } else if (strstr(notch_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("notch")) {
                                    channel.remove("notch");
                                }
                            }
                        }
                        
                        // Parse notch_q - remove if default (10.0) or null
                        const char* notchq_pos = strstr(body.c_str(), "\"notch_q\"");
                        if (notchq_pos) {
                            if (sscanf(notchq_pos, "\"notch_q\":%lf", &notch_q) == 1) {
                                if (notch_q != 10.0) {
                                    if (channel.exists("notch_q")) channel["notch_q"] = notch_q;
                                    else channel.add("notch_q", Setting::TypeFloat) = notch_q;
                                } else {
                                    // Remove if set to default (10.0)
                                    if (channel.exists("notch_q")) {
                                        channel.remove("notch_q");
                                    }
                                }
                            } else if (strstr(notchq_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("notch_q")) {
                                    channel.remove("notch_q");
                                }
                            }
                        }
                        
                        // Parse ctcss - remove if 0 or null, update if > 0
                        const char* ctcss_pos = strstr(body.c_str(), "\"ctcss\"");
                        if (ctcss_pos) {
                            if (sscanf(ctcss_pos, "\"ctcss\":%lf", &ctcss) == 1) {
                                if (ctcss > 0) {
                                    if (channel.exists("ctcss")) channel["ctcss"] = ctcss;
                                    else channel.add("ctcss", Setting::TypeFloat) = ctcss;
                                } else {
                                    // Remove if blanked out (0)
                                    if (channel.exists("ctcss")) {
                                        channel.remove("ctcss");
                                    }
                                }
                            } else if (strstr(ctcss_pos, ":null") != NULL) {
                                // Explicitly null - remove it
                                if (channel.exists("ctcss")) {
                                    channel.remove("ctcss");
                                }
                            }
                        }
                        
                        // Parse enabled/disable
                        const char* enabled_pos = strstr(body.c_str(), "\"enabled\"");
                        if (enabled_pos) {
                            bool is_enabled = (strstr(enabled_pos, ":true") != NULL);
                            if (channel.exists("disable")) {
                                channel["disable"] = !is_enabled;
                            } else if (!is_enabled) {
                                channel.add("disable", Setting::TypeBoolean) = true;
                            }
                        }
                        
                        // Parse outputs - this is more complex, need to handle array
                        // For now, we'll update outputs if they exist in the JSON
                        const char* outputs_pos = strstr(body.c_str(), "\"outputs\"");
                        if (outputs_pos && channel.exists("outputs")) {
                            // Outputs parsing would be more complex - for now, we'll leave it
                            // The outputs structure is complex with nested objects
                        }
                        
                        // Write config back to file
                        config.writeFile(config_path);
                        log(LOG_INFO, "Channel %d/%d updated in config file\n", device_idx, channel_idx);
                        send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel updated successfully. Click 'Apply Changes' to reload.\"}");
                    } catch (const FileIOException& fioex) {
                        log(LOG_ERR, "I/O error updating channel: %s\n", fioex.what());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while updating config file\"}");
                    } catch (const ParseException& pex) {
                        log(LOG_ERR, "Parse error updating channel: %s\n", pex.what());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Parse error in config file\"}");
                    } catch (const std::exception& ex) {
                        log(LOG_ERR, "Error updating channel: %s\n", ex.what());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Error updating channel\"}");
                    }
                } else if (strcmp(method, "DELETE") == 0) {
                    // Delete channel - set disable = true in config
                    const char* config_path = web_server_get_config_path();
                    try {
                        Config config;
                        config.readFile(config_path);
                        Setting& root = config.getRoot();
                        
                        if (root.exists("devices") && device_idx >= 0 && device_idx < root["devices"].getLength()) {
                            Setting& dev = root["devices"][device_idx];
                            if (dev.exists("channels") && channel_idx >= 0 && channel_idx < dev["channels"].getLength()) {
                                Setting& channel = dev["channels"][channel_idx];
                                // Set disable = true to mark channel as deleted
                                if (channel.exists("disable")) {
                                    channel["disable"] = true;
                                } else {
                                    channel.add("disable", Setting::TypeBoolean) = true;
                                }
                                
                                // Write config back to file
                                config.writeFile(config_path);
                                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel deleted. Restart required.\"}");
                            } else {
                                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid channel index\"}");
                            }
                        } else {
                            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid device index\"}");
                        }
                    } catch (const FileIOException& fioex) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while reading config file\"}");
                    } catch (const ParseException& pex) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Parse error in config file\"}");
                    } catch (...) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Unknown error modifying config\"}");
                    }
                } else {
                    send_error(client_fd, 405, "Method not allowed");
                }
            } else if (sscanf(path, "/api/channels/%d/%d/enable", &device_idx, &channel_idx) == 2 && strcmp(method, "POST") == 0) {
                // Enable channel
                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel enabled. Restart required.\"}");
            } else if (sscanf(path, "/api/channels/%d/%d/disable", &device_idx, &channel_idx) == 2 && strcmp(method, "POST") == 0) {
                // Disable channel
                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel disabled. Restart required.\"}");
            } else {
                int output_idx = -1;
                if (sscanf(path, "/api/channels/%d/%d/outputs/%d/enable", &device_idx, &channel_idx, &output_idx) == 3 && strcmp(method, "POST") == 0) {
                    // Enable output
                    send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Output enabled. Restart required.\"}");
                } else if (sscanf(path, "/api/channels/%d/%d/outputs/%d/disable", &device_idx, &channel_idx, &output_idx) == 3 && strcmp(method, "POST") == 0) {
                    // Disable output
                    send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Output disabled. Restart required.\"}");
                } else {
                    send_error(client_fd, 404, "Invalid channel endpoint");
                }
            }
        }
    } else if (strncmp(path, "/api/outputs", 12) == 0) {
        // Output settings endpoints
        if (strcmp(path, "/api/outputs/settings") == 0) {
            if (strcmp(method, "GET") == 0) {
                // Get output settings
                const char* config_path = web_server_get_config_path();
                try {
                    Config config;
                    config.readFile(config_path);
                    Setting& root = config.getRoot();
                    
                    int chunk_duration = 60;  // Default
                    if (root.exists("file_chunk_duration_minutes")) {
                        chunk_duration = (int)root["file_chunk_duration_minutes"];
                    }
                    
                    stringstream json;
                    json << "{\"file_chunk_duration_minutes\":" << chunk_duration << "}";
                    send_json_response(client_fd, json.str().c_str());
                } catch (const std::exception& e) {
                    send_json_response(client_fd, "{\"file_chunk_duration_minutes\":60}");
                }
            } else if (strcmp(method, "PUT") == 0) {
                // Update output settings
                const char* config_path = web_server_get_config_path();
                char* body = (char*)XCALLOC(content_length + 1, sizeof(char));
                ssize_t bytes_read = read(client_fd, body, content_length);
                
                if (bytes_read > 0) {
                    body[bytes_read] = '\0';
                    try {
                        Config config;
                        config.readFile(config_path);
                        Setting& root = config.getRoot();
                        
                        // Parse JSON (simple parsing for this specific case)
                        int chunk_duration = 60;
                        const char* chunk_key = "\"file_chunk_duration_minutes\"";
                        char* chunk_pos = strstr(body, chunk_key);
                        if (chunk_pos) {
                            char* colon = strchr(chunk_pos, ':');
                            if (colon) {
                                chunk_duration = atoi(colon + 1);
                                // Validate range: 5-60 minutes, in 5-minute increments
                                if (chunk_duration < 5 || chunk_duration > 60 || (chunk_duration % 5 != 0)) {
                                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"file_chunk_duration_minutes must be between 5 and 60, in 5-minute increments\"}");
                                    free(body);
                                    return;
                                }
                            }
                        }
                        
                        // Set or update the setting
                        if (root.exists("file_chunk_duration_minutes")) {
                            root["file_chunk_duration_minutes"] = chunk_duration;
                        } else {
                            root.add("file_chunk_duration_minutes", Setting::TypeInt) = chunk_duration;
                        }
                        
                        // Write config back to file
                        config.writeFile(config_path);
                        send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Output settings updated. Restart required.\"}");
                    } catch (const FileIOException& fioex) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while reading/writing config file\"}");
                    } catch (const ParseException& pex) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Parse error in config file\"}");
                    } catch (...) {
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Unknown error modifying config\"}");
                    }
                } else {
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Failed to read request body\"}");
                }
                free(body);
            } else {
                send_error(client_fd, 405, "Method not allowed");
            }
        } else {
            send_error(client_fd, 404, "Invalid output endpoint");
        }
    } else {
        send_error(client_fd, 404, "API endpoint not found");
    }
}

// Handle client connection
static void handle_client(int client_fd) {
    char method[16] = {0};
    char path[1024] = {0};
    size_t content_length = 0;
    
    if (!parse_request(client_fd, method, path, sizeof(path), &content_length)) {
        close(client_fd);
        return;
    }
    
    // Handle API requests
    if (strncmp(path, "/api/", 5) == 0) {
        // Check for DELETE method on errors endpoint
        if (strcmp(path, "/api/errors") == 0 && strcmp(method, "DELETE") == 0) {
            web_server_clear_errors();
            send_json_response(client_fd, "{\"status\":\"cleared\"}");
        } else {
            handle_api_request(client_fd, path, method, content_length);
        }
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        // Serve main HTML page
        const char* html = get_html_content();
        if (html) {
            send_file_response(client_fd, "text/html", html);
        } else {
            send_error(client_fd, 404, "HTML file not found");
        }
    } else if (strcmp(path, "/web_ui.css") == 0) {
        // Serve CSS file
        const char* css = get_css_content();
        if (css) {
            send_file_response(client_fd, "text/css", css);
        } else {
            send_error(client_fd, 404, "CSS file not found");
        }
    } else if (strcmp(path, "/web_ui.js") == 0) {
        // Serve JavaScript file
        const char* js = get_js_content();
        if (js) {
            send_file_response(client_fd, "application/javascript", js);
        } else {
            send_error(client_fd, 404, "JavaScript file not found");
        }
    } else if (strcmp(path, "/web_spectrum.js") == 0) {
        // Serve spectrum analyzer JavaScript file
        const char* spectrum_js = get_spectrum_js_content();
        if (spectrum_js) {
            send_file_response(client_fd, "application/javascript", spectrum_js);
        } else {
            send_error(client_fd, 404, "Spectrum JavaScript file not found");
        }
    } else if (strncmp(path, "/recordings/", 12) == 0) {
        // Serve recording files - find from device channels
        string path_str = path + 12;
        
        // Check for download query parameter
        bool is_download = false;
        size_t query_pos = path_str.find('?');
        if (query_pos != string::npos) {
            string query = path_str.substr(query_pos + 1);
            if (query == "download=1" || query.find("download=1&") == 0 || query.find("&download=1") != string::npos) {
                is_download = true;
            }
            path_str = path_str.substr(0, query_pos);
        }
        
        // URL decode the filename
        string filename = url_decode(path_str);
        
        FILE* f = NULL;
        string found_filepath;
        
        // Try to find the file in recording directories
        for (int d = 0; d < device_count && !f; d++) {
            device_t* dev = devices + d;
            for (int i = 0; i < dev->channel_count && !f; i++) {
                channel_t* channel = dev->channels + i;
                for (int k = 0; k < channel->output_count && !f; k++) {
                    output_t* output = channel->outputs + k;
                    if (output->type == O_FILE && output->data) {
                        file_data* fdata = (file_data*)(output->data);
                        if (!fdata->basedir.empty()) {
                            string filepath = fdata->basedir + "/" + filename;
                            f = fopen(filepath.c_str(), "rb");
                            if (f) {
                                found_filepath = filepath;
                                break;
                            }
                        }
                    }
                }
            }
        }
        
        if (f) {
            fseek(f, 0, SEEK_END);
            size_t size = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* buffer = (char*)XCALLOC(size, sizeof(char));
            fread(buffer, 1, size, f);
            fclose(f);
            
            // Determine MIME type from extension
            const char* content_type = "audio/mpeg";
            size_t ext_pos = filename.rfind('.');
            if (ext_pos != string::npos) {
                string ext = filename.substr(ext_pos);
                if (ext == ".mp3") {
                    content_type = "audio/mpeg";
                } else if (ext == ".raw") {
                    content_type = "application/octet-stream";
                }
            }
            
            // Add Content-Disposition header for downloads
            char content_disposition[512] = "";
            if (is_download) {
                snprintf(content_disposition, sizeof(content_disposition),
                    "Content-Disposition: attachment; filename=\"%s\"\r\n",
                    filename.c_str());
            }
            
            send_response(client_fd, 200, "OK", content_type, buffer, size, content_disposition);
            free(buffer);
        } else {
            send_error(client_fd, 404, "Recording not found");
        }
    } else {
        send_error(client_fd, 404, "Not found");
    }
    
    close(client_fd);
}

// Web server thread
void* web_server_thread(void* params) {
    int port = *(int*)params;
    server_port = port;
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        log(LOG_ERR, "Failed to create web server socket: %s\n", strerror(errno));
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    pthread_mutex_lock(&server_bind_mutex);
    
    if (bind(server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log(LOG_ERR, "Failed to bind web server to port %d: %s\n", port, strerror(errno));
        close(server_socket);
        server_socket = -1;
        server_bind_status = -1;
        pthread_cond_signal(&server_bind_cond);
        pthread_mutex_unlock(&server_bind_mutex);
        return NULL;
    }
    
    if (listen(server_socket, 10) < 0) {
        log(LOG_ERR, "Failed to listen on web server socket: %s\n", strerror(errno));
        close(server_socket);
        server_socket = -1;
        server_bind_status = -1;
        pthread_cond_signal(&server_bind_cond);
        pthread_mutex_unlock(&server_bind_mutex);
        return NULL;
    }
    
    // Get local IP addresses and display bindings
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* host = gethostbyname(hostname);
        if (host) {
            fprintf(stderr, "Web interface available at:\n");
            fprintf(stderr, "  http://localhost:%d\n", port);
            fprintf(stderr, "  http://127.0.0.1:%d\n", port);
            for (int i = 0; host->h_addr_list[i] != NULL; i++) {
                struct in_addr ip_addr;
                memcpy(&ip_addr, host->h_addr_list[i], sizeof(struct in_addr));
                char* ip_str = inet_ntoa(ip_addr);
                if (ip_str && strcmp(ip_str, "127.0.0.1") != 0) {
                    fprintf(stderr, "  http://%s:%d\n", ip_str, port);
                }
            }
        } else {
            fprintf(stderr, "Web interface started on port %d\n", port);
        }
    } else {
        fprintf(stderr, "Web interface started on port %d\n", port);
    }
    log(LOG_INFO, "Web interface started on port %d\n", port);
    server_running = 1;
    server_bind_status = 1;
    pthread_cond_signal(&server_bind_cond);
    pthread_mutex_unlock(&server_bind_mutex);
    
    log(LOG_INFO, "Web server entering main loop, waiting for connections on port %d...\n", port);
    
    while (server_running && !do_exit) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(server_socket + 1, &readfds, NULL, NULL, &timeout);
        
        if (activity > 0 && FD_ISSET(server_socket, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd >= 0) {
                handle_client(client_fd);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log(LOG_WARNING, "accept() failed: %s\n", strerror(errno));
                }
            }
        }
        
        // Check for errors on select
        if (activity < 0 && errno != EINTR) {
            log(LOG_ERR, "select() error in web server: %s\n", strerror(errno));
            break;
        }
    }
    
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    
    log(LOG_INFO, "Web interface stopped\n");
    return NULL;
}

int web_server_start(int port) {
    if (server_running) {
        return 0;  // Already running
    }
    
    server_bind_status = 0;  // Reset status
    
    int* port_ptr = (int*)XCALLOC(1, sizeof(int));
    *port_ptr = port;
    
    if (pthread_create(&server_thread, NULL, web_server_thread, port_ptr) != 0) {
        log(LOG_ERR, "Failed to create web server thread: %s\n", strerror(errno));
        free(port_ptr);
        return -1;
    }
    
    // Wait for bind to complete (or fail)
    pthread_mutex_lock(&server_bind_mutex);
    while (server_bind_status == 0) {
        pthread_cond_wait(&server_bind_cond, &server_bind_mutex);
    }
    int status = server_bind_status;
    pthread_mutex_unlock(&server_bind_mutex);
    
    if (status < 0) {
        return -1;  // Bind failed
    }
    
    return 0;  // Success
}

void web_server_stop(void) {
    server_running = 0;
    if (server_thread) {
        pthread_join(server_thread, NULL);
    }
}

void web_server_add_error(const char* error_msg) {
    if (!error_msg) return;
    std::lock_guard<std::mutex> lock(error_log_mutex);
    // Add timestamp
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    string error_entry = string(timestamp) + " - " + error_msg;
    error_log.push_back(error_entry);
    
    // Keep only last 100 errors
    if (error_log.size() > 100) {
        error_log.erase(error_log.begin());
    }
}

void web_server_clear_errors(void) {
    std::lock_guard<std::mutex> lock(error_log_mutex);
    error_log.clear();
}

void web_server_set_config_path(const char* config_path) {
    if (!config_path) return;
    std::lock_guard<std::mutex> lock(config_path_mutex);
    config_file_path = config_path;
}

const char* web_server_get_config_path(void) {
    std::lock_guard<std::mutex> lock(config_path_mutex);
    static std::string default_path = CFGFILE;
    if (config_file_path.empty()) {
        return default_path.c_str();
    }
    return config_file_path.c_str();
}

// Trigger configuration reload
int web_server_trigger_reload(void) {
    extern volatile int do_reload;
    if (do_reload == 0) {
        do_reload = 1;
        log(LOG_INFO, "Configuration reload requested\n");
    }
    return 0;
}

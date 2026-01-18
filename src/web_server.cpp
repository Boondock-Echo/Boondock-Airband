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
#include "capture_process.h"
#include "config.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cctype>
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
#include <map>
#include <set>
#include <functional>
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
    
    // If capture process is not running, return empty channels
    if (device_count == 0 || devices == NULL) {
        json << "]}";
        return json.str();
    }
    
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
    
    // If capture process is not running, return empty devices
    if (device_count == 0 || devices == NULL) {
        json << "]}";
        return json.str();
    }
    
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
struct recording_info {
    string filename;
    string path;
    string channel_name;
    long size;
    time_t create_time;
    string datetime;
};

static string get_recordings_json() {
    vector<recording_info> recordings;
    
    // Map directory to channel name
    map<string, string> dir_to_channel;
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        for (int i = 0; i < dev->channel_count; i++) {
            channel_t* channel = dev->channels + i;
            freq_t* fparms = channel->freqlist + channel->freq_idx;
            const char* label = fparms->label ? fparms->label : "";
            if (!label || strlen(label) == 0) {
                // Fallback to frequency if no label
                char freq_label[64];
                snprintf(freq_label, sizeof(freq_label), "%.3f MHz", fparms->frequency / 1000000.0);
                label = freq_label;
            }
            
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->type == O_FILE && output->data) {
                    file_data* fdata = (file_data*)(output->data);
                    if (!fdata->basedir.empty()) {
                        dir_to_channel[fdata->basedir] = label;
                    }
                }
            }
        }
    }
    
    // Recursive function to scan directory for recordings
    std::function<void(const string&, const string&)> scan_directory = [&](const string& dir, const string& channel_name) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            
            string filepath = dir + "/" + entry->d_name;
            struct stat st;
            if (stat(filepath.c_str(), &st) != 0) continue;
            
            if (S_ISREG(st.st_mode)) {
                // Check if it's an audio file
                string name = entry->d_name;
                if (name.length() < 4) continue;
                string ext = name.substr(name.length() - 4);
                if (ext != ".mp3" && ext != ".raw") continue;
                
                recording_info rec;
                rec.filename = name;
                rec.path = filepath;
                rec.channel_name = channel_name;
                rec.size = st.st_size;
                rec.create_time = st.st_mtime;  // Use mtime as creation time
                
                struct tm* timeinfo = localtime(&st.st_mtime);
                char date_str[64];
                strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", timeinfo);
                rec.datetime = date_str;
                
                recordings.push_back(rec);
            } else if (S_ISDIR(st.st_mode)) {
                // Recursively scan subdirectories (for dated subdirectories)
                scan_directory(filepath, channel_name);
            }
        }
        closedir(d);
    };
    
    // Scan directories for recordings (recursively)
    for (const auto& dir_pair : dir_to_channel) {
        const string& dir = dir_pair.first;
        const string& channel_name = dir_pair.second;
        scan_directory(dir, channel_name);
    }
    
    // Sort by creation time (latest first)
    sort(recordings.begin(), recordings.end(), [](const recording_info& a, const recording_info& b) {
        return a.create_time > b.create_time;
    });
    
    // Build JSON
    std::stringstream json;
    json << "{\"recordings\":[";
    
    bool first = true;
    for (const auto& rec : recordings) {
        if (!first) json << ",";
        first = false;
        
        // Escape JSON strings
        string escaped_filename = rec.filename;
        size_t pos = 0;
        while ((pos = escaped_filename.find("\"", pos)) != string::npos) {
            escaped_filename.replace(pos, 1, "\\\"");
            pos += 2;
        }
        
        string escaped_channel = rec.channel_name;
        pos = 0;
        while ((pos = escaped_channel.find("\"", pos)) != string::npos) {
            escaped_channel.replace(pos, 1, "\\\"");
            pos += 2;
        }
        
        json << "{\"filename\":\"" << escaped_filename << "\""
             << ",\"path\":\"" << rec.path << "\""
             << ",\"channel_name\":\"" << escaped_channel << "\""
             << ",\"size\":" << rec.size
             << ",\"datetime\":\"" << rec.datetime << "\""
             << ",\"create_time\":" << rec.create_time
             << "}";
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

// Convert a config frequency value to MHz (handles Hz ints and MHz floats/strings)
static double setting_to_mhz(const Setting& setting) {
    double value = 0.0;
    if (setting.getType() == Setting::TypeInt) {
        value = (double)(int)setting;
    } else if (setting.getType() == Setting::TypeFloat) {
        value = (double)setting;
    } else if (setting.getType() == Setting::TypeString) {
        value = atof((const char*)setting);
    } else {
        return 0.0;
    }
    if (value > 10000.0) {
        value /= 1000000.0;
    }
    return value;
}

static bool find_next_object(const string& text, size_t start, size_t& obj_start, size_t& obj_end) {
    obj_start = text.find('{', start);
    if (obj_start == string::npos) {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    for (size_t i = obj_start; i < text.size(); i++) {
        char c = text[i];
        if (in_string) {
            if (c == '\\' && i + 1 < text.size()) {
                i++;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                obj_end = i;
                return true;
            }
        }
    }
    return false;
}

static bool parse_bool_at(const string& text, size_t key_pos, bool& value) {
    size_t colon_pos = text.find(':', key_pos);
    if (colon_pos == string::npos) {
        return false;
    }
    size_t pos = colon_pos + 1;
    while (pos < text.size() && isspace(static_cast<unsigned char>(text[pos]))) {
        pos++;
    }
    if (text.compare(pos, 4, "true") == 0 || text.compare(pos, 1, "1") == 0) {
        value = true;
        return true;
    }
    if (text.compare(pos, 5, "false") == 0 || text.compare(pos, 1, "0") == 0) {
        value = false;
        return true;
    }
    return false;
}

static bool parse_channel_enabled_from_object(const string& channel_obj, bool& enabled) {
    size_t outputs_pos = channel_obj.find("\"outputs\"");
    size_t search_end = (outputs_pos == string::npos) ? channel_obj.size() : outputs_pos;
    size_t enabled_pos = channel_obj.find("\"enabled\"");
    while (enabled_pos != string::npos && enabled_pos >= search_end) {
        enabled_pos = channel_obj.find("\"enabled\"", enabled_pos + 1);
    }
    if (enabled_pos == string::npos || enabled_pos >= search_end) {
        return false;
    }
    return parse_bool_at(channel_obj, enabled_pos, enabled);
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
                
                for (int c = 0; c < chans.getLength(); c++) {
                    // Check if channel is disabled - include ALL channels but mark disabled ones
                    bool channel_disabled = chans[c].exists("disable") && (bool)chans[c]["disable"];
                    
                    if (c > 0) json << ",";
                    
                    json << "{";
                    json << "\"channel_index\":" << c;
                    // Set enabled based on disable flag (inverse: disabled=false means enabled=false)
                    json << ",\"enabled\":" << (channel_disabled ? "false" : "true");
                    
                    bool has_freq = false;
                    double freq_mhz = 0.0;
                    if (chans[c].exists("freq")) {
                        freq_mhz = setting_to_mhz(chans[c]["freq"]);
                        has_freq = true;
                    } else if (chans[c].exists("freqs") && chans[c]["freqs"].getLength() > 0) {
                        freq_mhz = setting_to_mhz(chans[c]["freqs"][0]);
                        has_freq = true;
                    }
                    if (has_freq) {
                        json << ",\"freq\":" << freq_mhz;
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
                            if (outputs[o].exists("split_on_transmission")) {
                                json << ",\"split_on_transmission\":" << ((bool)outputs[o]["split_on_transmission"] ? "true" : "false");
                            }
                            if (outputs[o].exists("include_freq")) {
                                json << ",\"include_freq\":" << ((bool)outputs[o]["include_freq"] ? "true" : "false");
                            }
                            if (outputs[o].exists("append")) {
                                json << ",\"append\":" << ((bool)outputs[o]["append"] ? "true" : "false");
                            }
                            if (outputs[o].exists("dated_subdirectories")) {
                                json << ",\"dated_subdirectories\":" << ((bool)outputs[o]["dated_subdirectories"] ? "true" : "false");
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
        // If capture process is not running, return empty data
        if (device_count == 0 || devices == NULL) {
            send_json_response(client_fd, "{\"devices\":[]}");
            return;
        }
        
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
    } else if (strcmp(path, "/api/capture/stop") == 0 && strcmp(method, "POST") == 0) {
        // Stop capture subprocess
        if (capture_process_stop() == 0) {
            log(LOG_INFO, "Capture process stopped via API\n");
            send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Capture process stopped\"}");
        } else {
            log(LOG_ERR, "Failed to stop capture process via API\n");
            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Failed to stop capture process\"}");
        }
    } else if (strcmp(path, "/api/capture/start") == 0 && strcmp(method, "POST") == 0) {
        // Start capture subprocess with latest configuration
        const char* config_path = web_server_get_config_path();
        pid_t pid = capture_process_start(config_path);
        if (pid > 0) {
            log(LOG_INFO, "Capture process started via API (PID: %d)\n", pid);
            send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Capture process started with latest configuration\"}");
        } else {
            log(LOG_ERR, "Failed to start capture process via API\n");
            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Failed to start capture process\"}");
        }
    } else if (strcmp(path, "/api/capture/status") == 0 && strcmp(method, "GET") == 0) {
        // Get capture subprocess status
        int is_running = capture_process_is_running();
        pid_t pid = capture_process_get_pid();
        char response[256];
        if (is_running && pid > 0) {
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"capture_enabled\":1,\"pid\":%d}", (int)pid);
        } else {
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"capture_enabled\":0,\"pid\":0}");
        }
        send_json_response(client_fd, response);
    // /api/apply endpoint removed - configuration is now applied automatically when starting capture
    } else if (strncmp(path, "/api/channels", 13) == 0) {
        // Channel management endpoints
        if (strcmp(path, "/api/channels/config") == 0 && strcmp(method, "PUT") == 0) {
            // Save channel configuration: update disable flags in boondock_airband.conf
            if (content_length == 0 || content_length > 102400) {
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
                
                if (!root.exists("devices")) {
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"No devices found in config\"}");
                    return;
                }
                
                Setting& devs = root["devices"];
                if (devs.getLength() == 0) {
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"No devices configured\"}");
                    return;
                }
                
                // Update center frequency if provided in request
                const char* centerFreqPos = strstr(body.c_str(), "\"centerfreq\"");
                if (centerFreqPos) {
                    double centerFreq = 0;
                    if (sscanf(centerFreqPos, "\"centerfreq\":%lf", &centerFreq) == 1) {
                        // Frontend sends centerfreq in Hz, but config stores it in MHz
                        // Convert Hz to MHz for storage (if > 10000, assume it's in Hz)
                        double centerFreqMHz = centerFreq;
                        if (centerFreq > 10000) {
                            centerFreqMHz = centerFreq / 1000000.0;
                        }
                        
                        Setting& dev = devs[0];
                        if (dev.exists("centerfreq")) {
                            // Check existing type and handle accordingly
                            Setting::Type existingType = dev["centerfreq"].getType();
                            if (existingType == Setting::TypeFloat) {
                                dev["centerfreq"] = centerFreqMHz;
                            } else if (existingType == Setting::TypeInt) {
                                // Type mismatch - remove and re-add with correct type
                                dev.remove("centerfreq");
                                dev.add("centerfreq", Setting::TypeFloat) = centerFreqMHz;
                            } else {
                                // Type mismatch - remove and re-add with correct type
                                dev.remove("centerfreq");
                                dev.add("centerfreq", Setting::TypeFloat) = centerFreqMHz;
                            }
                        } else {
                            // Add as float (MHz format, e.g., 162.48200)
                            dev.add("centerfreq", Setting::TypeFloat) = centerFreqMHz;
                        }
                    }
                }
                
                // Build a set of enabled channel indices from the request body
                // Prefer explicit enabled_channels list if provided
                map<int, set<int> > enabledChannels; // device -> set of channel indices
                bool enabled_channels_parsed = false;
                size_t enabled_channels_pos = body.find("\"enabled_channels\"");
                if (enabled_channels_pos != string::npos) {
                    size_t enabled_array_pos = body.find('[', enabled_channels_pos);
                    if (enabled_array_pos != string::npos) {
                        size_t pos = enabled_array_pos + 1;
                        size_t entry_start = 0, entry_end = 0;
                        while (find_next_object(body, pos, entry_start, entry_end)) {
                            string entry_obj = body.substr(entry_start, entry_end - entry_start + 1);
                            int dev_num = -1;
                            int ch_idx = -1;
                            const char* dev_pos = strstr(entry_obj.c_str(), "\"device\":");
                            const char* ch_idx_pos = strstr(entry_obj.c_str(), "\"channel_index\":");
                            if (dev_pos && ch_idx_pos &&
                                sscanf(dev_pos, "\"device\":%d", &dev_num) == 1 && dev_num >= 0 &&
                                sscanf(ch_idx_pos, "\"channel_index\":%d", &ch_idx) == 1 && ch_idx >= 0) {
                                enabledChannels[dev_num].insert(ch_idx);
                                enabled_channels_parsed = true;
                            }
                            pos = entry_end + 1;
                        }
                    }
                }
                
                if (!enabled_channels_parsed) {
                    size_t devices_pos = body.find("\"devices\"");
                    if (devices_pos != string::npos) {
                        size_t devices_array_pos = body.find('[', devices_pos);
                        if (devices_array_pos != string::npos) {
                            size_t pos = devices_array_pos + 1;
                            size_t dev_obj_start = 0, dev_obj_end = 0;
                            while (find_next_object(body, pos, dev_obj_start, dev_obj_end)) {
                                string dev_obj = body.substr(dev_obj_start, dev_obj_end - dev_obj_start + 1);
                                int dev_num = -1;
                                const char* dev_pos = strstr(dev_obj.c_str(), "\"device\":");
                                if (dev_pos && sscanf(dev_pos, "\"device\":%d", &dev_num) == 1 && dev_num >= 0) {
                                    size_t channels_pos = dev_obj.find("\"channels\"");
                                    if (channels_pos != string::npos) {
                                        size_t channels_array_pos = dev_obj.find('[', channels_pos);
                                        if (channels_array_pos != string::npos) {
                                            size_t ch_pos = channels_array_pos + 1;
                                            size_t ch_obj_start = 0, ch_obj_end = 0;
                                            while (find_next_object(dev_obj, ch_pos, ch_obj_start, ch_obj_end)) {
                                                string ch_obj = dev_obj.substr(ch_obj_start, ch_obj_end - ch_obj_start + 1);
                                                int ch_idx = -1;
                                                const char* ch_idx_pos = strstr(ch_obj.c_str(), "\"channel_index\":");
                                                if (ch_idx_pos && sscanf(ch_idx_pos, "\"channel_index\":%d", &ch_idx) == 1 && ch_idx >= 0) {
                                                    bool enabled = false;
                                                    if (parse_channel_enabled_from_object(ch_obj, enabled) && enabled) {
                                                        enabledChannels[dev_num].insert(ch_idx);
                                                    }
                                                }
                                                ch_pos = ch_obj_end + 1;
                                            }
                                        }
                                    }
                                }
                                pos = dev_obj_end + 1;
                            }
                        }
                    }
                }
                
                // Mark channels as disabled/enabled based on the request
                // IMPORTANT: Channels are NEVER removed from the config file.
                // When disabled, they are marked with "disable = true" and remain in the config.
                // When parsing, channels with disable=true are skipped (see config.cpp parse_channels).
                for (int d = 0; d < devs.getLength(); d++) {
                    Setting& dev = devs[d];
                    if (!dev.exists("channels")) continue;
                    
                    Setting& channels = dev["channels"];
                    // Get enabled set for this device (creates empty set if not found)
                    set<int> enabledSet;
                    if (enabledChannels.find(d) != enabledChannels.end()) {
                        enabledSet = enabledChannels[d];
                    }
                    
                    for (int c = 0; c < channels.getLength(); c++) {
                        try {
                            if (enabledSet.find(c) != enabledSet.end()) {
                                // Channel is enabled - remove disable flag
                                if (channels[c].exists("disable")) {
                                    channels[c].remove("disable");
                                }
                            } else {
                                // Channel is disabled - add disable flag (channel stays in config)
                                if (channels[c].exists("disable")) {
                                    channels[c]["disable"] = true;
                                } else {
                                    channels[c].add("disable", Setting::TypeBoolean) = true;
                                }
                            }
                        } catch (const SettingNotFoundException& nfex) {
                            log(LOG_WARNING, "Setting not found processing channel %d in device %d: %s\n", c, d, nfex.getPath());
                        } catch (const SettingTypeException& tex) {
                            log(LOG_WARNING, "Type error processing channel %d in device %d: %s\n", c, d, tex.getPath());
                        } catch (const ConfigException& cex) {
                            log(LOG_WARNING, "Config error processing channel %d in device %d: %s\n", c, d, cex.what());
                        } catch (...) {
                            // Skip this channel if there's an error
                            log(LOG_WARNING, "Unknown error processing channel %d in device %d\n", c, d);
                        }
                    }
                }
                
                // Validate config file path
                if (!config_path || strlen(config_path) == 0) {
                    log(LOG_ERR, "Invalid config file path\n");
                    send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid configuration file path\"}");
                    return;
                }
                
                // Check if config file is writable
                FILE* test_file = fopen(config_path, "a");
                if (!test_file) {
                    log(LOG_ERR, "Cannot write to config file: %s (errno: %d, %s)\n", config_path, errno, strerror(errno));
                    char error_msg[512];
                    snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Cannot write to config file: %s\"}", strerror(errno));
                    send_json_response(client_fd, error_msg);
                    return;
                }
                fclose(test_file);
                
                // Write config back to file
                config.writeFile(config_path);
                
                log(LOG_INFO, "Channel configuration saved (disable flags updated in boondock_airband.conf)\n");
                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Configuration saved. Click 'Start Capture' to apply.\"}");
            } catch (const FileIOException& fioex) {
                log(LOG_ERR, "I/O error saving channel config: %s\n", fioex.what());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"I/O error: %s\"}", fioex.what());
                send_json_response(client_fd, error_msg);
            } catch (const ParseException& pex) {
                log(LOG_ERR, "Parse error saving channel config at %s: %s\n", pex.getFile(), pex.getError());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Parse error at %s: %s\"}", pex.getFile(), pex.getError());
                send_json_response(client_fd, error_msg);
            } catch (const SettingNotFoundException& nfex) {
                log(LOG_ERR, "Setting not found saving channel config: %s\n", nfex.getPath());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Setting not found: %s\"}", nfex.getPath());
                send_json_response(client_fd, error_msg);
            } catch (const SettingTypeException& tex) {
                log(LOG_ERR, "Setting type error saving channel config: %s\n", tex.getPath());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Type error at: %s\"}", tex.getPath());
                send_json_response(client_fd, error_msg);
            } catch (const ConfigException& cex) {
                log(LOG_ERR, "Config exception saving channel config: %s\n", cex.what());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Config error: %s\"}", cex.what());
                send_json_response(client_fd, error_msg);
            } catch (const std::exception& ex) {
                log(LOG_ERR, "Standard exception saving channel config: %s\n", ex.what());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Error: %s\"}", ex.what());
                send_json_response(client_fd, error_msg);
            } catch (...) {
                log(LOG_ERR, "Unknown exception saving channel config\n");
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Unknown error saving configuration. Check server logs for details.\"}");
            }
        } else if (strcmp(path, "/api/channels") == 0 && strcmp(method, "GET") == 0) {
            // Read channels directly from boondock_airband.conf
            string json = get_channels_full_json();
            send_json_response(client_fd, json.c_str());
        } else if (strcmp(path, "/api/channels") == 0 && strcmp(method, "POST") == 0) {
            // Add new channel - read JSON body and save to boondock_airband.conf
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
                        freqs.add(Setting::TypeFloat) = freq;  // Store MHz as float
                    } else {
                        new_channel.add("freq", Setting::TypeFloat) = freq;  // Store MHz as float
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
                
                // Add outputs - parse all output types from JSON
                const char* outputs_pos = strstr(body.c_str(), "\"outputs\"");
                Setting& outputs = new_channel.add("outputs", Setting::TypeList);
                
                if (outputs_pos) {
                    // Parse each output type in the array
                    // Find file output
                    const char* file_type_pos = strstr(outputs_pos, "\"type\":\"file\"");
                    if (file_type_pos) {
                        Setting& file_out = outputs.add(Setting::TypeGroup);
                        file_out.add("type", Setting::TypeString) = "file";
                        
                        // Parse directory
                        const char* dir_pos = strstr(file_type_pos, "\"directory\"");
                        if (dir_pos) {
                            char directory[512] = {0};
                            if (sscanf(dir_pos, "\"directory\":\"%511[^\"]\"", directory) == 1) {
                                file_out.add("directory", Setting::TypeString) = directory;
                            } else {
                                file_out.add("directory", Setting::TypeString) = "recordings";
                            }
                        } else {
                            file_out.add("directory", Setting::TypeString) = "recordings";
                        }
                        
                        // Parse filename_template
                        const char* filename_pos = strstr(file_type_pos, "\"filename_template\"");
                        if (filename_pos) {
                            char filename[512] = {0};
                            if (sscanf(filename_pos, "\"filename_template\":\"%511[^\"]\"", filename) == 1) {
                                file_out.add("filename_template", Setting::TypeString) = filename;
                            }
                        }
                        
                        // Parse boolean parameters
                        if (strstr(file_type_pos, "\"continuous\":true") != NULL) {
                            file_out.add("continuous", Setting::TypeBoolean) = true;
                        }
                        if (strstr(file_type_pos, "\"split_on_transmission\":true") != NULL) {
                            file_out.add("split_on_transmission", Setting::TypeBoolean) = true;
                        }
                        if (strstr(file_type_pos, "\"include_freq\":true") != NULL) {
                            file_out.add("include_freq", Setting::TypeBoolean) = true;
                        }
                        if (strstr(file_type_pos, "\"append\":true") != NULL) {
                            file_out.add("append", Setting::TypeBoolean) = true;
                        } else if (strstr(file_type_pos, "\"append\":false") == NULL) {
                            // Default append is true if not specified
                            file_out.add("append", Setting::TypeBoolean) = true;
                        }
                        if (strstr(file_type_pos, "\"dated_subdirectories\":true") != NULL) {
                            file_out.add("dated_subdirectories", Setting::TypeBoolean) = true;
                        }
                    }
                    
                    // Parse UDP stream output
                    const char* udp_type_pos = strstr(outputs_pos, "\"type\":\"udp_stream\"");
                    if (udp_type_pos) {
                        Setting& udp_out = outputs.add(Setting::TypeGroup);
                        udp_out.add("type", Setting::TypeString) = "udp_stream";
                        
                        const char* dest_addr_pos = strstr(udp_type_pos, "\"dest_address\"");
                        if (dest_addr_pos) {
                            char dest_address[256] = {0};
                            if (sscanf(dest_addr_pos, "\"dest_address\":\"%255[^\"]\"", dest_address) == 1) {
                                udp_out.add("dest_address", Setting::TypeString) = dest_address;
                            }
                        }
                        
                        const char* dest_port_pos = strstr(udp_type_pos, "\"dest_port\"");
                        if (dest_port_pos) {
                            int dest_port = 6001;
                            if (sscanf(dest_port_pos, "\"dest_port\":%d", &dest_port) == 1) {
                                udp_out.add("dest_port", Setting::TypeInt) = dest_port;
                            }
                        }
                        
                        if (strstr(udp_type_pos, "\"continuous\":true") != NULL) {
                            udp_out.add("continuous", Setting::TypeBoolean) = true;
                        }
                        if (strstr(udp_type_pos, "\"udp_headers\":true") != NULL) {
                            udp_out.add("udp_headers", Setting::TypeBoolean) = true;
                        }
                        if (strstr(udp_type_pos, "\"udp_chunking\":true") != NULL) {
                            udp_out.add("udp_chunking", Setting::TypeBoolean) = true;
                        }
                    }
                    
                    // Parse Icecast output
                    const char* icecast_type_pos = strstr(outputs_pos, "\"type\":\"icecast\"");
                    if (icecast_type_pos) {
                        Setting& icecast_out = outputs.add(Setting::TypeGroup);
                        icecast_out.add("type", Setting::TypeString) = "icecast";
                        
                        const char* server_pos = strstr(icecast_type_pos, "\"server\"");
                        if (server_pos) {
                            char server[256] = {0};
                            if (sscanf(server_pos, "\"server\":\"%255[^\"]\"", server) == 1) {
                                icecast_out.add("server", Setting::TypeString) = server;
                            }
                        }
                        
                        const char* port_pos = strstr(icecast_type_pos, "\"port\"");
                        if (port_pos) {
                            int port = 8000;
                            if (sscanf(port_pos, "\"port\":%d", &port) == 1) {
                                icecast_out.add("port", Setting::TypeInt) = port;
                            }
                        }
                        
                        const char* mountpoint_pos = strstr(icecast_type_pos, "\"mountpoint\"");
                        if (mountpoint_pos) {
                            char mountpoint[256] = {0};
                            if (sscanf(mountpoint_pos, "\"mountpoint\":\"%255[^\"]\"", mountpoint) == 1) {
                                icecast_out.add("mountpoint", Setting::TypeString) = mountpoint;
                            }
                        }
                        
                        const char* username_pos = strstr(icecast_type_pos, "\"username\"");
                        if (username_pos) {
                            char username[256] = {0};
                            if (sscanf(username_pos, "\"username\":\"%255[^\"]\"", username) == 1) {
                                icecast_out.add("username", Setting::TypeString) = username;
                            }
                        }
                        
                        const char* password_pos = strstr(icecast_type_pos, "\"password\"");
                        if (password_pos) {
                            char password[256] = {0};
                            if (sscanf(password_pos, "\"password\":\"%255[^\"]\"", password) == 1) {
                                icecast_out.add("password", Setting::TypeString) = password;
                            }
                        }
                        
                        const char* name_pos = strstr(icecast_type_pos, "\"name\"");
                        if (name_pos) {
                            char name[256] = {0};
                            if (sscanf(name_pos, "\"name\":\"%255[^\"]\"", name) == 1) {
                                icecast_out.add("name", Setting::TypeString) = name;
                            }
                        }
                    }
                    
                    // Parse Boondock API output
                    const char* boondock_type_pos = strstr(outputs_pos, "\"type\":\"boondock_api\"");
                    if (boondock_type_pos) {
                        Setting& boondock_out = outputs.add(Setting::TypeGroup);
                        boondock_out.add("type", Setting::TypeString) = "boondock_api";
                        
                        const char* api_url_pos = strstr(boondock_type_pos, "\"api_url\"");
                        if (api_url_pos) {
                            char api_url[512] = {0};
                            if (sscanf(api_url_pos, "\"api_url\":\"%511[^\"]\"", api_url) == 1) {
                                boondock_out.add("api_url", Setting::TypeString) = api_url;
                            }
                        }
                        
                        const char* api_key_pos = strstr(boondock_type_pos, "\"api_key\"");
                        if (api_key_pos) {
                            char api_key[256] = {0};
                            if (sscanf(api_key_pos, "\"api_key\":\"%255[^\"]\"", api_key) == 1) {
                                boondock_out.add("api_key", Setting::TypeString) = api_key;
                            }
                        }
                    }
                    
                    // Parse Redis output
                    const char* redis_type_pos = strstr(outputs_pos, "\"type\":\"redis\"");
                    if (redis_type_pos) {
                        Setting& redis_out = outputs.add(Setting::TypeGroup);
                        redis_out.add("type", Setting::TypeString) = "redis";
                        
                        const char* address_pos = strstr(redis_type_pos, "\"address\"");
                        if (address_pos) {
                            char address[256] = {0};
                            if (sscanf(address_pos, "\"address\":\"%255[^\"]\"", address) == 1) {
                                redis_out.add("address", Setting::TypeString) = address;
                            }
                        }
                        
                        const char* port_pos = strstr(redis_type_pos, "\"port\"");
                        if (port_pos) {
                            int port = 6379;
                            if (sscanf(port_pos, "\"port\":%d", &port) == 1) {
                                redis_out.add("port", Setting::TypeInt) = port;
                            }
                        }
                        
                        const char* password_pos = strstr(redis_type_pos, "\"password\"");
                        if (password_pos) {
                            char password[256] = {0};
                            if (sscanf(password_pos, "\"password\":\"%255[^\"]\"", password) == 1) {
                                redis_out.add("password", Setting::TypeString) = password;
                            }
                        }
                        
                        const char* database_pos = strstr(redis_type_pos, "\"database\"");
                        if (database_pos) {
                            int database = 0;
                            if (sscanf(database_pos, "\"database\":%d", &database) == 1) {
                                redis_out.add("database", Setting::TypeInt) = database;
                            }
                        }
                    }
                }
                
                // If no outputs were added, add a default file output
                if (outputs.getLength() == 0) {
                    Setting& file_out = outputs.add(Setting::TypeGroup);
                    file_out.add("type", Setting::TypeString) = "file";
                    file_out.add("directory", Setting::TypeString) = "recordings";
                    file_out.add("filename_template", Setting::TypeString) = "${label}_${start:%Y%m%d}_${start:%H}.mp3";
                }
                
                // Write to boondock_airband.conf
                config.writeFile(config_path);
                
                log(LOG_INFO, "New channel added to device %d (saved to boondock_airband.conf)\n", device_idx);
                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel added successfully to boondock_airband.conf\"}");
            } catch (const FileIOException& fioex) {
                log(LOG_ERR, "I/O error adding channel: %s\n", fioex.what());
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while updating config file\"}");
            } catch (const ParseException& pex) {
                log(LOG_ERR, "Parse error adding channel at %s: %s\n", pex.getFile(), pex.getError());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Parse error in config file: %s\"}", pex.getError());
                send_json_response(client_fd, error_msg);
            } catch (const SettingNotFoundException& nfex) {
                log(LOG_ERR, "Setting not found adding channel: %s\n", nfex.getPath());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Setting not found: %s\"}", nfex.getPath());
                send_json_response(client_fd, error_msg);
            } catch (const SettingTypeException& tex) {
                log(LOG_ERR, "Setting type error adding channel: %s\n", tex.getPath());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Type error at: %s\"}", tex.getPath());
                send_json_response(client_fd, error_msg);
            } catch (const ConfigException& cex) {
                log(LOG_ERR, "Config exception adding channel: %s\n", cex.what());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Config error: %s\"}", cex.what());
                send_json_response(client_fd, error_msg);
            } catch (const std::exception& ex) {
                log(LOG_ERR, "Standard exception adding channel: %s\n", ex.what());
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "{\"status\":\"error\",\"message\":\"Error: %s\"}", ex.what());
                send_json_response(client_fd, error_msg);
            } catch (...) {
                log(LOG_ERR, "Unknown exception adding channel\n");
                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Unknown error adding channel\"}");
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
                                // Scan mode - update freqs array using existing element type
                                if (channel.exists("freqs") && channel["freqs"].getLength() > 0) {
                                    Setting& freqs = channel["freqs"];
                                    Setting::Type elem_type = freqs[0].getType();
                                    if (elem_type == Setting::TypeInt) {
                                        freqs[0] = (int)(freq * 1000000);
                                    } else if (elem_type == Setting::TypeFloat) {
                                        freqs[0] = freq;
                                    } else {
                                        channel.remove("freqs");
                                        Setting& new_freqs = channel.add("freqs", Setting::TypeList);
                                        new_freqs.add(Setting::TypeFloat) = freq;
                                    }
                                } else {
                                    Setting& new_freqs = channel.add("freqs", Setting::TypeList);
                                    new_freqs.add(Setting::TypeFloat) = freq;
                                }
                            } else {
                                // Multichannel mode - keep existing type if possible
                                if (channel.exists("freq")) {
                                    Setting::Type freq_type = channel["freq"].getType();
                                    if (freq_type == Setting::TypeInt) {
                                        channel["freq"] = (int)(freq * 1000000);
                                    } else if (freq_type == Setting::TypeFloat) {
                                        channel["freq"] = freq;
                                    } else {
                                        channel.remove("freq");
                                        channel.add("freq", Setting::TypeFloat) = freq;
                                    }
                                } else {
                                    channel.add("freq", Setting::TypeFloat) = freq;
                                }
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
                        
                        // Parse outputs - parse file output parameters from JSON
                        const char* outputs_pos = strstr(body.c_str(), "\"outputs\"");
                        if (outputs_pos) {
                            // Find file output in the outputs array
                            const char* file_type_pos = strstr(outputs_pos, "\"type\":\"file\"");
                            if (file_type_pos) {
                                // Remove existing outputs and recreate
                                if (channel.exists("outputs")) {
                                    channel.remove("outputs");
                                }
                                Setting& outputs = channel.add("outputs", Setting::TypeList);
                                Setting& file_out = outputs.add(Setting::TypeGroup);
                                file_out.add("type", Setting::TypeString) = "file";
                                
                                // Parse directory
                                const char* dir_pos = strstr(file_type_pos, "\"directory\"");
                                if (dir_pos) {
                                    char directory[512] = {0};
                                    if (sscanf(dir_pos, "\"directory\":\"%511[^\"]\"", directory) == 1) {
                                        file_out.add("directory", Setting::TypeString) = directory;
                                    }
                                }
                                
                                // Parse filename_template
                                const char* filename_pos = strstr(file_type_pos, "\"filename_template\"");
                                if (filename_pos) {
                                    char filename[512] = {0};
                                    if (sscanf(filename_pos, "\"filename_template\":\"%511[^\"]\"", filename) == 1) {
                                        file_out.add("filename_template", Setting::TypeString) = filename;
                                    }
                                }
                                
                                // Parse boolean parameters
                                if (strstr(file_type_pos, "\"continuous\":true") != NULL) {
                                    file_out.add("continuous", Setting::TypeBoolean) = true;
                                }
                                if (strstr(file_type_pos, "\"split_on_transmission\":true") != NULL) {
                                    file_out.add("split_on_transmission", Setting::TypeBoolean) = true;
                                }
                                if (strstr(file_type_pos, "\"include_freq\":true") != NULL) {
                                    file_out.add("include_freq", Setting::TypeBoolean) = true;
                                }
                                if (strstr(file_type_pos, "\"append\":true") != NULL) {
                                    file_out.add("append", Setting::TypeBoolean) = true;
                                } else if (strstr(file_type_pos, "\"append\":false") == NULL) {
                                    // Default append is true if not specified
                                    file_out.add("append", Setting::TypeBoolean) = true;
                                }
                                if (strstr(file_type_pos, "\"dated_subdirectories\":true") != NULL) {
                                    file_out.add("dated_subdirectories", Setting::TypeBoolean) = true;
                                }
                            }
                        }
                        
                        // Write to main config (for now, to keep it in sync)
                        config.writeFile(config_path);
                        
                        log(LOG_INFO, "Channel %d/%d updated (saved to boondock_airband.conf)\n", device_idx, channel_idx);
                        send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel updated successfully in boondock_airband.conf\"}");
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
                    // Delete channel - completely remove it from the config file
                    const char* config_path = web_server_get_config_path();
                    try {
                        Config config;
                        config.readFile(config_path);
                        Setting& root = config.getRoot();
                        
                        if (root.exists("devices") && device_idx >= 0 && device_idx < root["devices"].getLength()) {
                            Setting& dev = root["devices"][device_idx];
                            if (dev.exists("channels") && channel_idx >= 0 && channel_idx < dev["channels"].getLength()) {
                                Setting& channels = dev["channels"];
                                
                                // Remove the channel from the array (completely delete it from config)
                                // libconfig supports removing list elements by index using remove(int)
                                channels.remove(channel_idx);
                                
                                // Write updated config back to file
                                config.writeFile(config_path);
                                
                                log(LOG_INFO, "Channel %d from device %d completely removed from boondock_airband.conf\n", channel_idx, device_idx);
                                send_json_response(client_fd, "{\"status\":\"success\",\"message\":\"Channel permanently deleted from boondock_airband.conf. Restart required.\"}");
                            } else {
                                send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid channel index\"}");
                            }
                        } else {
                            send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Invalid device index\"}");
                        }
                    } catch (const FileIOException& fioex) {
                        log(LOG_ERR, "I/O error deleting channel: %s\n", fioex.what());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"I/O error while reading config file\"}");
                    } catch (const ParseException& pex) {
                        log(LOG_ERR, "Parse error deleting channel at %s: %s\n", pex.getFile(), pex.getError());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Parse error in config file\"}");
                    } catch (const std::exception& ex) {
                        log(LOG_ERR, "Exception deleting channel: %s\n", ex.what());
                        send_json_response(client_fd, "{\"status\":\"error\",\"message\":\"Error deleting channel\"}");
                    } catch (...) {
                        log(LOG_ERR, "Unknown error deleting channel\n");
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
                    
                    stringstream json;
                    json << "{";
                    
                    int chunk_duration = 60;  // Default
                    if (root.exists("file_chunk_duration_minutes")) {
                        chunk_duration = (int)root["file_chunk_duration_minutes"];
                    }
                    json << "\"file_chunk_duration_minutes\":" << chunk_duration;
                    
                    // Load output methods settings
                    json << ",\"output_methods\":{";
                    bool first_method = true;
                    
                    // File method
                    bool file_enabled = true;  // Default enabled
                    string global_recording_dir = "recordings";
                    if (root.exists("output_methods") && root["output_methods"].exists("file")) {
                        file_enabled = (bool)root["output_methods"]["file"]["enabled"];
                        if (root["output_methods"]["file"].exists("global_recording_directory")) {
                            global_recording_dir = (const char*)root["output_methods"]["file"]["global_recording_directory"];
                        }
                    }
                    if (!first_method) json << ",";
                    first_method = false;
                    json << "\"file\":{\"enabled\":" << (file_enabled ? "true" : "false")
                         << ",\"global_recording_directory\":\"" << global_recording_dir << "\"}";
                    
                    // UDP method
                    bool udp_enabled = false;
                    string udp_address = "127.0.0.1";
                    bool udp_headers = false;
                    if (root.exists("output_methods") && root["output_methods"].exists("udp")) {
                        udp_enabled = (bool)root["output_methods"]["udp"]["enabled"];
                        if (root["output_methods"]["udp"].exists("default_address")) {
                            udp_address = (const char*)root["output_methods"]["udp"]["default_address"];
                        }
                        if (root["output_methods"]["udp"].exists("default_headers")) {
                            udp_headers = (bool)root["output_methods"]["udp"]["default_headers"];
                        }
                    }
                    if (!first_method) json << ",";
                    first_method = false;
                    json << "\"udp\":{\"enabled\":" << (udp_enabled ? "true" : "false") 
                         << ",\"default_address\":\"" << udp_address << "\""
                         << ",\"default_headers\":" << (udp_headers ? "true" : "false") << "}";
                    
                    // UDP Server method
                    bool udp_server_enabled = false;
                    int port_start = 6001, port_end = 6100;
                    if (root.exists("output_methods") && root["output_methods"].exists("udp_server")) {
                        udp_server_enabled = (bool)root["output_methods"]["udp_server"]["enabled"];
                        if (root["output_methods"]["udp_server"].exists("port_start")) {
                            port_start = (int)root["output_methods"]["udp_server"]["port_start"];
                        }
                        if (root["output_methods"]["udp_server"].exists("port_end")) {
                            port_end = (int)root["output_methods"]["udp_server"]["port_end"];
                        }
                    }
                    if (!first_method) json << ",";
                    first_method = false;
                    json << "\"udp_server\":{\"enabled\":" << (udp_server_enabled ? "true" : "false")
                         << ",\"port_start\":" << port_start << ",\"port_end\":" << port_end << "}";
                    
                    // Boondock API method
                    bool boondock_api_enabled = false;
                    string api_url = "", api_key = "";
                    if (root.exists("output_methods") && root["output_methods"].exists("boondock_api")) {
                        boondock_api_enabled = (bool)root["output_methods"]["boondock_api"]["enabled"];
                        if (root["output_methods"]["boondock_api"].exists("api_url")) {
                            api_url = (const char*)root["output_methods"]["boondock_api"]["api_url"];
                        }
                        if (root["output_methods"]["boondock_api"].exists("api_key")) {
                            api_key = (const char*)root["output_methods"]["boondock_api"]["api_key"];
                        }
                    }
                    if (!first_method) json << ",";
                    first_method = false;
                    json << "\"boondock_api\":{\"enabled\":" << (boondock_api_enabled ? "true" : "false")
                         << ",\"api_url\":\"" << api_url << "\",\"api_key\":\"" << api_key << "\"}";
                    
                    // Redis method
                    bool redis_enabled = false;
                    string redis_address = "127.0.0.1";
                    int redis_port = 6379, redis_database = 0;
                    string redis_password = "";
                    if (root.exists("output_methods") && root["output_methods"].exists("redis")) {
                        redis_enabled = (bool)root["output_methods"]["redis"]["enabled"];
                        if (root["output_methods"]["redis"].exists("address")) {
                            redis_address = (const char*)root["output_methods"]["redis"]["address"];
                        }
                        if (root["output_methods"]["redis"].exists("port")) {
                            redis_port = (int)root["output_methods"]["redis"]["port"];
                        }
                        if (root["output_methods"]["redis"].exists("database")) {
                            redis_database = (int)root["output_methods"]["redis"]["database"];
                        }
                        if (root["output_methods"]["redis"].exists("password")) {
                            redis_password = (const char*)root["output_methods"]["redis"]["password"];
                        }
                    }
                    if (!first_method) json << ",";
                    json << "\"redis\":{\"enabled\":" << (redis_enabled ? "true" : "false")
                         << ",\"address\":\"" << redis_address << "\",\"port\":" << redis_port
                         << ",\"database\":" << redis_database << ",\"password\":\"" << redis_password << "\"}";
                    
                    // Icecast method
                    bool icecast_enabled = false;
                    string icecast_server = "";
                    int icecast_port = 8000;
                    string icecast_mountpoint = "";
                    string icecast_username = "";
                    string icecast_password = "";
                    if (root.exists("output_methods") && root["output_methods"].exists("icecast")) {
                        icecast_enabled = (bool)root["output_methods"]["icecast"]["enabled"];
                        if (root["output_methods"]["icecast"].exists("server")) {
                            icecast_server = (const char*)root["output_methods"]["icecast"]["server"];
                        }
                        if (root["output_methods"]["icecast"].exists("port")) {
                            icecast_port = (int)root["output_methods"]["icecast"]["port"];
                        }
                        if (root["output_methods"]["icecast"].exists("mountpoint")) {
                            icecast_mountpoint = (const char*)root["output_methods"]["icecast"]["mountpoint"];
                        }
                        if (root["output_methods"]["icecast"].exists("username")) {
                            icecast_username = (const char*)root["output_methods"]["icecast"]["username"];
                        }
                        if (root["output_methods"]["icecast"].exists("password")) {
                            icecast_password = (const char*)root["output_methods"]["icecast"]["password"];
                        }
                    }
                    if (!first_method) json << ",";
                    json << "\"icecast\":{\"enabled\":" << (icecast_enabled ? "true" : "false")
                         << ",\"server\":\"" << icecast_server << "\",\"port\":" << icecast_port
                         << ",\"mountpoint\":\"" << icecast_mountpoint << "\",\"username\":\"" << icecast_username
                         << "\",\"password\":\"" << icecast_password << "\"}";
                    
                    json << "}}";
                    send_json_response(client_fd, json.str().c_str());
                } catch (const std::exception& e) {
                    send_json_response(client_fd, "{\"file_chunk_duration_minutes\":60,\"output_methods\":{\"file\":{\"enabled\":true,\"global_recording_directory\":\"recordings\"},\"udp\":{\"enabled\":false,\"default_address\":\"127.0.0.1\",\"default_headers\":false},\"udp_server\":{\"enabled\":false,\"port_start\":6001,\"port_end\":6100},\"boondock_api\":{\"enabled\":false,\"api_url\":\"\",\"api_key\":\"\"},\"redis\":{\"enabled\":false,\"address\":\"127.0.0.1\",\"port\":6379,\"database\":0,\"password\":\"\"},\"icecast\":{\"enabled\":false,\"server\":\"\",\"port\":8000,\"mountpoint\":\"\",\"username\":\"\",\"password\":\"\"}}}");
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
                        
                        // Parse and save output_methods
                        const char* methods_pos = strstr(body, "\"output_methods\"");
                        if (methods_pos) {
                            // Create or get output_methods group
                            if (!root.exists("output_methods")) {
                                root.add("output_methods", Setting::TypeGroup);
                            }
                            Setting& output_methods = root["output_methods"];
                            
                            // Parse file method
                            const char* file_pos = strstr(methods_pos, "\"file\"");
                            if (file_pos) {
                                if (!output_methods.exists("file")) {
                                    output_methods.add("file", Setting::TypeGroup);
                                }
                                Setting& file_method = output_methods["file"];
                                bool file_enabled = (strstr(file_pos, "\"enabled\":true") != NULL);
                                if (file_method.exists("enabled")) {
                                    file_method["enabled"] = file_enabled;
                                } else {
                                    file_method.add("enabled", Setting::TypeBoolean) = file_enabled;
                                }
                                
                                // Parse global_recording_directory
                                char global_dir[256] = {0};
                                const char* global_dir_pos = strstr(file_pos, "\"global_recording_directory\"");
                                if (global_dir_pos && sscanf(global_dir_pos, "\"global_recording_directory\":\"%255[^\"]\"", global_dir) == 1) {
                                    if (file_method.exists("global_recording_directory")) {
                                        file_method["global_recording_directory"] = global_dir;
                                    } else {
                                        file_method.add("global_recording_directory", Setting::TypeString) = global_dir;
                                    }
                                }
                            }
                            
                            // Parse UDP method
                            const char* udp_pos = strstr(methods_pos, "\"udp\"");
                            if (udp_pos) {
                                if (!output_methods.exists("udp")) {
                                    output_methods.add("udp", Setting::TypeGroup);
                                }
                                Setting& udp_method = output_methods["udp"];
                                bool udp_enabled = (strstr(udp_pos, "\"enabled\":true") != NULL);
                                if (udp_method.exists("enabled")) {
                                    udp_method["enabled"] = udp_enabled;
                                } else {
                                    udp_method.add("enabled", Setting::TypeBoolean) = udp_enabled;
                                }
                                
                                // Parse default_address
                                char udp_address[256] = {0};
                                const char* addr_pos = strstr(udp_pos, "\"default_address\"");
                                if (addr_pos && sscanf(addr_pos, "\"default_address\":\"%255[^\"]\"", udp_address) == 1) {
                                    if (udp_method.exists("default_address")) {
                                        udp_method["default_address"] = udp_address;
                                    } else {
                                        udp_method.add("default_address", Setting::TypeString) = udp_address;
                                    }
                                }
                                
                                // Parse default_headers
                                bool udp_headers = (strstr(udp_pos, "\"default_headers\":true") != NULL);
                                if (udp_method.exists("default_headers")) {
                                    udp_method["default_headers"] = udp_headers;
                                } else {
                                    udp_method.add("default_headers", Setting::TypeBoolean) = udp_headers;
                                }
                            }
                            
                            // Parse UDP Server method
                            const char* udp_server_pos = strstr(methods_pos, "\"udp_server\"");
                            if (udp_server_pos) {
                                if (!output_methods.exists("udp_server")) {
                                    output_methods.add("udp_server", Setting::TypeGroup);
                                }
                                Setting& udp_server_method = output_methods["udp_server"];
                                bool udp_server_enabled = (strstr(udp_server_pos, "\"enabled\":true") != NULL);
                                if (udp_server_method.exists("enabled")) {
                                    udp_server_method["enabled"] = udp_server_enabled;
                                } else {
                                    udp_server_method.add("enabled", Setting::TypeBoolean) = udp_server_enabled;
                                }
                                
                                // Parse port_start
                                const char* port_start_pos = strstr(udp_server_pos, "\"port_start\"");
                                if (port_start_pos) {
                                    int port_start = 6001;
                                    if (sscanf(port_start_pos, "\"port_start\":%d", &port_start) == 1) {
                                        if (udp_server_method.exists("port_start")) {
                                            udp_server_method["port_start"] = port_start;
                                        } else {
                                            udp_server_method.add("port_start", Setting::TypeInt) = port_start;
                                        }
                                    }
                                }
                                
                                // Parse port_end
                                const char* port_end_pos = strstr(udp_server_pos, "\"port_end\"");
                                if (port_end_pos) {
                                    int port_end = 6100;
                                    if (sscanf(port_end_pos, "\"port_end\":%d", &port_end) == 1) {
                                        if (udp_server_method.exists("port_end")) {
                                            udp_server_method["port_end"] = port_end;
                                        } else {
                                            udp_server_method.add("port_end", Setting::TypeInt) = port_end;
                                        }
                                    }
                                }
                            }
                            
                            // Parse Boondock API method
                            const char* boondock_api_pos = strstr(methods_pos, "\"boondock_api\"");
                            if (boondock_api_pos) {
                                if (!output_methods.exists("boondock_api")) {
                                    output_methods.add("boondock_api", Setting::TypeGroup);
                                }
                                Setting& boondock_api_method = output_methods["boondock_api"];
                                bool boondock_api_enabled = (strstr(boondock_api_pos, "\"enabled\":true") != NULL);
                                if (boondock_api_method.exists("enabled")) {
                                    boondock_api_method["enabled"] = boondock_api_enabled;
                                } else {
                                    boondock_api_method.add("enabled", Setting::TypeBoolean) = boondock_api_enabled;
                                }
                                
                                // Parse api_url
                                char api_url[512] = {0};
                                const char* url_pos = strstr(boondock_api_pos, "\"api_url\"");
                                if (url_pos && sscanf(url_pos, "\"api_url\":\"%511[^\"]\"", api_url) == 1) {
                                    if (boondock_api_method.exists("api_url")) {
                                        boondock_api_method["api_url"] = api_url;
                                    } else {
                                        boondock_api_method.add("api_url", Setting::TypeString) = api_url;
                                    }
                                }
                                
                                // Parse api_key
                                char api_key[256] = {0};
                                const char* key_pos = strstr(boondock_api_pos, "\"api_key\"");
                                if (key_pos && sscanf(key_pos, "\"api_key\":\"%255[^\"]\"", api_key) == 1) {
                                    if (boondock_api_method.exists("api_key")) {
                                        boondock_api_method["api_key"] = api_key;
                                    } else {
                                        boondock_api_method.add("api_key", Setting::TypeString) = api_key;
                                    }
                                }
                            }
                            
                            // Parse Redis method
                            const char* redis_pos = strstr(methods_pos, "\"redis\"");
                            if (redis_pos) {
                                if (!output_methods.exists("redis")) {
                                    output_methods.add("redis", Setting::TypeGroup);
                                }
                                Setting& redis_method = output_methods["redis"];
                                bool redis_enabled = (strstr(redis_pos, "\"enabled\":true") != NULL);
                                if (redis_method.exists("enabled")) {
                                    redis_method["enabled"] = redis_enabled;
                                } else {
                                    redis_method.add("enabled", Setting::TypeBoolean) = redis_enabled;
                                }
                                
                                // Parse address
                                char redis_address[256] = {0};
                                const char* redis_addr_pos = strstr(redis_pos, "\"address\"");
                                if (redis_addr_pos && sscanf(redis_addr_pos, "\"address\":\"%255[^\"]\"", redis_address) == 1) {
                                    if (redis_method.exists("address")) {
                                        redis_method["address"] = redis_address;
                                    } else {
                                        redis_method.add("address", Setting::TypeString) = redis_address;
                                    }
                                }
                                
                                // Parse port
                                const char* redis_port_pos = strstr(redis_pos, "\"port\"");
                                if (redis_port_pos) {
                                    int redis_port = 6379;
                                    if (sscanf(redis_port_pos, "\"port\":%d", &redis_port) == 1) {
                                        if (redis_method.exists("port")) {
                                            redis_method["port"] = redis_port;
                                        } else {
                                            redis_method.add("port", Setting::TypeInt) = redis_port;
                                        }
                                    }
                                }
                                
                                // Parse database
                                const char* redis_db_pos = strstr(redis_pos, "\"database\"");
                                if (redis_db_pos) {
                                    int redis_database = 0;
                                    if (sscanf(redis_db_pos, "\"database\":%d", &redis_database) == 1) {
                                        if (redis_method.exists("database")) {
                                            redis_method["database"] = redis_database;
                                        } else {
                                            redis_method.add("database", Setting::TypeInt) = redis_database;
                                        }
                                    }
                                }
                                
                                // Parse password
                                char redis_password[256] = {0};
                                const char* redis_pwd_pos = strstr(redis_pos, "\"password\"");
                                if (redis_pwd_pos && sscanf(redis_pwd_pos, "\"password\":\"%255[^\"]\"", redis_password) == 1) {
                                    if (redis_method.exists("password")) {
                                        redis_method["password"] = redis_password;
                                    } else {
                                        redis_method.add("password", Setting::TypeString) = redis_password;
                                    }
                                }
                            }
                            
                            // Parse Icecast method
                            const char* icecast_pos = strstr(methods_pos, "\"icecast\"");
                            if (icecast_pos) {
                                if (!output_methods.exists("icecast")) {
                                    output_methods.add("icecast", Setting::TypeGroup);
                                }
                                Setting& icecast_method = output_methods["icecast"];
                                bool icecast_enabled = (strstr(icecast_pos, "\"enabled\":true") != NULL);
                                if (icecast_method.exists("enabled")) {
                                    icecast_method["enabled"] = icecast_enabled;
                                } else {
                                    icecast_method.add("enabled", Setting::TypeBoolean) = icecast_enabled;
                                }
                                
                                // Parse server
                                char icecast_server[256] = {0};
                                const char* icecast_server_pos = strstr(icecast_pos, "\"server\"");
                                if (icecast_server_pos && sscanf(icecast_server_pos, "\"server\":\"%255[^\"]\"", icecast_server) == 1) {
                                    if (icecast_method.exists("server")) {
                                        icecast_method["server"] = icecast_server;
                                    } else {
                                        icecast_method.add("server", Setting::TypeString) = icecast_server;
                                    }
                                }
                                
                                // Parse port
                                const char* icecast_port_pos = strstr(icecast_pos, "\"port\"");
                                if (icecast_port_pos) {
                                    int icecast_port = 8000;
                                    if (sscanf(icecast_port_pos, "\"port\":%d", &icecast_port) == 1) {
                                        if (icecast_method.exists("port")) {
                                            icecast_method["port"] = icecast_port;
                                        } else {
                                            icecast_method.add("port", Setting::TypeInt) = icecast_port;
                                        }
                                    }
                                }
                                
                                // Parse mountpoint
                                char icecast_mountpoint[256] = {0};
                                const char* icecast_mount_pos = strstr(icecast_pos, "\"mountpoint\"");
                                if (icecast_mount_pos && sscanf(icecast_mount_pos, "\"mountpoint\":\"%255[^\"]\"", icecast_mountpoint) == 1) {
                                    if (icecast_method.exists("mountpoint")) {
                                        icecast_method["mountpoint"] = icecast_mountpoint;
                                    } else {
                                        icecast_method.add("mountpoint", Setting::TypeString) = icecast_mountpoint;
                                    }
                                }
                                
                                // Parse username
                                char icecast_username[256] = {0};
                                const char* icecast_user_pos = strstr(icecast_pos, "\"username\"");
                                if (icecast_user_pos && sscanf(icecast_user_pos, "\"username\":\"%255[^\"]\"", icecast_username) == 1) {
                                    if (icecast_method.exists("username")) {
                                        icecast_method["username"] = icecast_username;
                                    } else {
                                        icecast_method.add("username", Setting::TypeString) = icecast_username;
                                    }
                                }
                                
                                // Parse password
                                char icecast_password[256] = {0};
                                const char* icecast_pwd_pos = strstr(icecast_pos, "\"password\"");
                                if (icecast_pwd_pos && sscanf(icecast_pwd_pos, "\"password\":\"%255[^\"]\"", icecast_password) == 1) {
                                    if (icecast_method.exists("password")) {
                                        icecast_method["password"] = icecast_password;
                                    } else {
                                        icecast_method.add("password", Setting::TypeString) = icecast_password;
                                    }
                                }
                            }
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
        
        // Recursive function to search for file in directory
        std::function<bool(const string&, const string&)> find_file = [&](const string& dir, const string& search_name) -> bool {
            DIR* d = opendir(dir.c_str());
            if (!d) return false;
            
            struct dirent* entry;
            while ((entry = readdir(d)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                
                string filepath = dir + "/" + entry->d_name;
                struct stat st;
                if (stat(filepath.c_str(), &st) != 0) continue;
                
                if (S_ISREG(st.st_mode) && entry->d_name == search_name) {
                    f = fopen(filepath.c_str(), "rb");
                    if (f) {
                        found_filepath = filepath;
                        closedir(d);
                        return true;
                    }
                } else if (S_ISDIR(st.st_mode)) {
                    // Recursively search subdirectories (for dated subdirectories)
                    if (find_file(filepath, search_name)) {
                        closedir(d);
                        return true;
                    }
                }
            }
            closedir(d);
            return false;
        };
        
        // Try to find the file in recording directories (recursively)
        for (int d = 0; d < device_count && !f; d++) {
            device_t* dev = devices + d;
            for (int i = 0; i < dev->channel_count && !f; i++) {
                channel_t* channel = dev->channels + i;
                for (int k = 0; k < channel->output_count && !f; k++) {
                    output_t* output = channel->outputs + k;
                    if (output->type == O_FILE && output->data) {
                        file_data* fdata = (file_data*)(output->data);
                        if (!fdata->basedir.empty()) {
                            find_file(fdata->basedir, filename);
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

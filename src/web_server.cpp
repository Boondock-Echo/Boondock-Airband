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
static void send_response(int client_fd, int status_code, const char* status_text, const char* content_type, const char* body, size_t body_len) {
    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
    
    write(client_fd, response, len);
    if (body && body_len > 0) {
        write(client_fd, body, body_len);
    }
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
            
            if (!first) json << ",";
            first = false;
            
            json << "{\"channel\":" << i 
                 << ",\"frequency\":" << std::fixed << std::setprecision(3) << freq_mhz
                 << ",\"label\":\"" << label << "\""
                 << ",\"signal_level\":" << std::setprecision(1) << signal_dbfs
                 << ",\"noise_level\":" << noise_dbfs
                 << ",\"snr\":" << snr
                 << ",\"status\":\"" << status_str << "\"}";
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

// Handle API requests
static void handle_api_request(int client_fd, const char* path, const char* method, size_t content_length) {
    if (strcmp(path, "/api/status") == 0) {
        string json = get_channels_status_json();
        send_json_response(client_fd, json.c_str());
    } else if (strcmp(path, "/api/device") == 0) {
        string json = get_device_info_json();
        send_json_response(client_fd, json.c_str());
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
        send_file_response(client_fd, "text/html", html);
    } else if (strncmp(path, "/recordings/", 12) == 0) {
        // Serve recording files - find from device channels
        const char* filename = path + 12;
        FILE* f = NULL;
        
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
                            if (f) break;
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
            send_response(client_fd, 200, "OK", "audio/mpeg", buffer, size);
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
            }
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

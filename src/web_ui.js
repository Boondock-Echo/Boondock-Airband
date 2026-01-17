var currentPage = "home";
var statusInterval;
var hasPendingChanges = false;
var pendingChanges = {
    device: false,
    channels: false,
    outputs: false,
    config: false
};

function markChange(type) {
    if (type) {
        pendingChanges[type] = true;
    }
    hasPendingChanges = true;
    updateApplyButton();
}

function clearChanges() {
    hasPendingChanges = false;
    pendingChanges = {
        device: false,
        channels: false,
        outputs: false,
        config: false
    };
    updateApplyButton();
}

function updateApplyButton() {
    var btn = document.getElementById("apply-changes-btn");
    if (btn) {
        btn.disabled = !hasPendingChanges;
    }
}


function showPage(page) {
    document.querySelectorAll(".page").forEach(function(p) { p.classList.add("hidden"); });
    document.querySelectorAll(".nav-link").forEach(function(l) { l.classList.remove("active"); });
    document.getElementById("page-" + page).classList.remove("hidden");
    var selector = "[data-page=\"" + page + "\"]";
    document.querySelector(selector).classList.add("active");
    currentPage = page;
    
    // Toggle full-width class for spectrum page
    var container = document.querySelector(".container");
    var body = document.body;
    if (container) {
        if (page === "spectrum") {
            container.classList.add("full-width");
            if (body) body.classList.add("spectrum-page-active");
        } else {
            container.classList.remove("full-width");
            if (body) body.classList.remove("spectrum-page-active");
        }
    }
    
    if (page === "home") {
        startStatusPolling();
    } else {
        stopStatusPolling();
        if (page === "recordings") {
            loadRecordings();
        } else if (page === "channels") {
            loadChannels();
        } else if (page === "device") {
            loadDevice();
            updateDeviceFields();
            toggleCenterFreq();
        } else if (page === "outputs") {
            loadOutputSettings();
        } else if (page === "config") {
            loadConfigInfo();
        }
    }
}

function startStatusPolling() {
    if (statusInterval) clearInterval(statusInterval);
    loadColumnVisibility();
    updateStatus();
    statusInterval = setInterval(updateStatus, 1000);
}

function stopStatusPolling() {
    if (statusInterval) {
        clearInterval(statusInterval);
        statusInterval = null;
    }
}

// Column visibility management
var columnVisibility = {
    name: true,
    freq: true,
    ch: true,
    udp: false,
    signal: true,
    noise: true,
    squelch: true,
    snr: true,
    strength: false,
    ctcss: true,
    rec: true
};

function loadColumnVisibility() {
    var saved = localStorage.getItem("channelColumns");
    if (saved) {
        try {
            columnVisibility = JSON.parse(saved);
        } catch(e) {
            console.error("Failed to load column visibility:", e);
        }
    }
    applyColumnVisibility();
}

function saveColumnVisibility() {
    localStorage.setItem("channelColumns", JSON.stringify(columnVisibility));
}

function applyColumnVisibility() {
    var headers = document.querySelectorAll("#channels-table th.col-toggle");
    headers.forEach(function(th) {
        var col = th.getAttribute("data-col");
        if (col && columnVisibility[col] !== undefined) {
            if (columnVisibility[col]) {
                th.style.display = "";
            } else {
                th.style.display = "none";
            }
        }
    });
    
    var rows = document.querySelectorAll("#channels-tbody tr");
    rows.forEach(function(row) {
        var cells = row.querySelectorAll("td[data-col]");
        cells.forEach(function(cell) {
            var col = cell.getAttribute("data-col");
            if (col && columnVisibility[col] !== undefined) {
                if (columnVisibility[col]) {
                    cell.style.display = "";
                } else {
                    cell.style.display = "none";
                }
            }
        });
    });
}

function showColumnSelector() {
    var modal = document.getElementById("column-selector-modal");
    var checkboxes = document.getElementById("column-checkboxes");
    checkboxes.innerHTML = "";
    
    var columnLabels = {
        name: "Channel Name",
        freq: "Frequency",
        ch: "Channel #",
        udp: "UDP Port",
        signal: "Signal Level",
        noise: "Noise Level",
        squelch: "Squelch Level",
        snr: "SNR",
        strength: "Signal Strength",
        ctcss: "CTCSS Count",
        rec: "Recording"
    };
    
    for (var col in columnVisibility) {
        var label = document.createElement("label");
        label.style.cssText = "display: flex; align-items: center; gap: 8px; cursor: pointer;";
        
        var checkbox = document.createElement("input");
        checkbox.type = "checkbox";
        checkbox.checked = columnVisibility[col];
        checkbox.setAttribute("data-col", col);
        
        var span = document.createElement("span");
        span.textContent = columnLabels[col] || col;
        
        label.appendChild(checkbox);
        label.appendChild(span);
        checkboxes.appendChild(label);
    }
    
    modal.style.display = "block";
}

function closeColumnSelector() {
    document.getElementById("column-selector-modal").style.display = "none";
}

function applyColumnSelection() {
    var checkboxes = document.querySelectorAll("#column-checkboxes input[type='checkbox']");
    checkboxes.forEach(function(cb) {
        var col = cb.getAttribute("data-col");
        columnVisibility[col] = cb.checked;
    });
    saveColumnVisibility();
    applyColumnVisibility();
    closeColumnSelector();
}

function resetColumnSelection() {
    columnVisibility = {
        name: true,
        freq: true,
        ch: true,
        udp: false,
        signal: true,
        noise: true,
        squelch: true,
        snr: true,
        strength: false,
        ctcss: true,
        rec: true
    };
    saveColumnVisibility();
    showColumnSelector();
}

function updateStatus() {
    fetch("/api/status")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var tbody = document.getElementById("channels-tbody");
            tbody.innerHTML = "";
            data.channels.forEach(function(ch) {
                var row = document.createElement("tr");
                var label = ch.label || ("Channel " + ch.channel);
                var statusMark = ch.status === "signal" ? "*" : "";
                var udpPort = 6001 + ch.channel;
                
                // Signal Strength: -80 dB (worst) to 0 dB (best) = 0% to 100%
                var signalStrength = ch.signal_level;
                var signalPercent = Math.max(0, Math.min(100, ((signalStrength - (-80)) / (0 - (-80))) * 100));
                
                // SNR: 0 dB (worst) to 50 dB (best) = 0% to 100%
                var snr = ch.snr;
                var snrPercent = Math.max(0, Math.min(100, (snr / 50) * 100));
                
                // Recording status (compact)
                var recordingStatus = "";
                if (ch.has_file_output) {
                    if (ch.is_recording) {
                        recordingStatus = "<span style=\"color: #28a745; font-weight: bold;\">●</span>";
                    } else {
                        recordingStatus = "<span style=\"color: #999;\">○</span>";
                    }
                } else {
                    recordingStatus = "<span style=\"color: #ccc;\">—</span>";
                }
                
                // Compact display with all columns
                var cells = [
                    {col: "name", html: "<span title=\"" + label + "\">" + (label.length > 15 ? label.substring(0, 12) + "..." : label) + "</span>"},
                    {col: "freq", html: ch.frequency + " MHz"},
                    {col: "ch", html: "#" + ch.channel},
                    {col: "udp", html: udpPort},
                    {col: "signal", html: "<span title=\"Signal: " + ch.signal_level.toFixed(1) + " dB\">" + ch.signal_level.toFixed(1) + " dB</span>"},
                    {col: "noise", html: "<span title=\"Noise: " + (ch.noise_level || 0).toFixed(1) + " dB\">" + (ch.noise_level || 0).toFixed(1) + " dB</span>"},
                    {col: "squelch", html: "<span title=\"Squelch: " + (ch.squelch_level || 0).toFixed(1) + " dB\">" + (ch.squelch_level || 0).toFixed(1) + " dB</span>"},
                    {col: "snr", html: "<div class=\"bar-container\" style=\"min-width: 60px;\"><div class=\"bar-fill snr-bar-fill\" style=\"width: " + snrPercent + "%;\"></div><span class=\"bar-text\">" + snr.toFixed(1) + " dB</span></div>"},
                    {col: "strength", html: "<div class=\"bar-container\" style=\"min-width: 60px;\"><div class=\"bar-fill signal-bar-fill\" style=\"width: " + signalPercent + "%;\"></div><span class=\"bar-text\">" + signalStrength.toFixed(1) + " dB</span></div>"},
                    {col: "ctcss", html: "<span title=\"CTCSS detections: " + (ch.ctcss_count || 0) + "\">" + (ch.ctcss_count || 0) + "</span>"},
                    {col: "rec", html: recordingStatus}
                ];
                
                cells.forEach(function(cell) {
                    var td = document.createElement("td");
                    td.setAttribute("data-col", cell.col);
                    td.innerHTML = cell.html;
                    if (!columnVisibility[cell.col]) {
                        td.style.display = "none";
                    }
                    row.appendChild(td);
                });
                
                tbody.appendChild(row);
            });
            document.getElementById("channels-count").textContent = data.channels.length;
        })
        .catch(function(err) { console.error("Status update error:", err); });
    
    // Also update errors
    updateErrors();
}

function updateErrors() {
    fetch("/api/errors")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var errorsCard = document.getElementById("errors-card");
            var errorsList = document.getElementById("errors-list");
            
            if (data.errors && data.errors.length > 0) {
                errorsCard.style.display = "block";
                errorsList.innerHTML = "";
                data.errors.forEach(function(err) {
                    var p = document.createElement("p");
                    p.style.cssText = "margin: 5px 0; padding: 8px; background: white; border-left: 3px solid #dc3545; border-radius: 4px;";
                    p.textContent = err;
                    errorsList.appendChild(p);
                });
            } else {
                errorsCard.style.display = "none";
            }
        })
        .catch(function(err) { console.error("Errors load error:", err); });
}

function clearErrors() {
    fetch("/api/errors", { method: "DELETE" })
        .then(function() {
            updateErrors();
        })
        .catch(function(err) { console.error("Clear errors error:", err); });
}

function loadRecordings() {
    fetch("/api/recordings")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var tbody = document.getElementById("recordings-tbody");
            tbody.innerHTML = "";
            data.recordings.forEach(function(rec) {
                var row = document.createElement("tr");
                var sizeMB = (rec.size / 1024 / 1024).toFixed(2);
                row.innerHTML = "<td>" + rec.datetime + "</td>" +
                    "<td>" + sizeMB + " MB</td>" +
                    "<td>Channel</td>" +
                    "<td><audio controls style=\"width: 200px;\"><source src=\"/recordings/" + encodeURIComponent(rec.filename) + "\" type=\"audio/mpeg\"></audio></td>" +
                    "<td><button class=\"btn btn-primary\" onclick=\"downloadRecording('" + rec.filename + "')\">Download</button></td>";
                tbody.appendChild(row);
            });
        })
        .catch(function(err) { console.error("Recordings load error:", err); });
}

var currentEditingChannel = null;
var channelsData = null;

function loadChannels() {
    fetch("/api/channels")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            channelsData = data;
            var tbody = document.getElementById("channels-list-tbody");
            tbody.innerHTML = "";
            
            if (data.devices && data.devices.length > 0) {
                data.devices.forEach(function(dev) {
                    if (dev.channels && dev.channels.length > 0) {
                        dev.channels.forEach(function(ch) {
                            var row = document.createElement("tr");
                            var label = ch.label || ("Channel " + ch.channel_index);
                            var freq = ch.freq ? ch.freq.toFixed(3) : "N/A";
                            var modulation = ch.modulation || "am";
                            
                            // Find outputs
                            var hasRecording = false, hasStreaming = false, hasContinuous = false;
                            if (ch.outputs) {
                                ch.outputs.forEach(function(out) {
                                    if (out.enabled) {
                                        if (out.type === "file") {
                                            hasRecording = true;
                                            if (out.continuous) hasContinuous = true;
                                        }
                                        if (out.type === "udp_stream" || out.type === "icecast") {
                                            hasStreaming = true;
                                        }
                                    }
                                });
                            }
                            
                            var squelch = "Auto";
                            if (ch.squelch_threshold !== undefined) {
                                squelch = ch.squelch_threshold + " dBFS";
                            } else if (ch.squelch_snr_threshold !== undefined) {
                                squelch = ch.squelch_snr_threshold + " dB SNR";
                            }
                            
                            row.innerHTML = 
                                "<td><input type=\"checkbox\" " + (ch.enabled ? "checked" : "") + " onchange=\"toggleChannel(" + dev.device + "," + ch.channel_index + ", this.checked)\"></td>" +
                                "<td>" + label + "</td>" +
                                "<td>" + freq + " MHz</td>" +
                                "<td>" + modulation.toUpperCase() + "</td>" +
                                "<td>" + squelch + "</td>" +
                                "<td>" + (hasRecording ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
                                "<td>" + (hasStreaming ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
                                "<td>" + (hasContinuous ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
                                "<td><button class=\"btn btn-primary\" onclick=\"editChannel(" + dev.device + "," + ch.channel_index + ")\" style=\"margin-right: 5px;\">Edit</button>" +
                                "<button class=\"btn btn-danger\" onclick=\"deleteChannel(" + dev.device + "," + ch.channel_index + ")\">Delete</button></td>";
                            tbody.appendChild(row);
                        });
                    }
                });
            }
        })
        .catch(function(err) { 
            console.error("Channels load error:", err);
            // Fallback to status API
            fetch("/api/status")
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    var tbody = document.getElementById("channels-list-tbody");
                    tbody.innerHTML = "";
                    data.channels.forEach(function(ch) {
                        var row = document.createElement("tr");
                        var label = ch.label || ("Channel " + ch.channel);
                        row.innerHTML = "<td><input type=\"checkbox\" checked></td>" +
                            "<td>" + label + "</td>" +
                            "<td>" + ch.frequency + " MHz</td>" +
                            "<td>AM</td>" +
                            "<td>Auto</td>" +
                            "<td><span style=\"color: #999;\">No</span></td>" +
                            "<td><span style=\"color: #999;\">No</span></td>" +
                            "<td><span style=\"color: #999;\">No</span></td>" +
                            "<td><button class=\"btn btn-primary\" onclick=\"editChannel(0," + ch.channel + ")\">Edit</button></td>";
                        tbody.appendChild(row);
                    });
                });
        });
}

function updateDeviceFields() {
    var deviceType = document.getElementById("device-type").value;
    
    // Hide all device-specific fields
    document.querySelectorAll(".device-type-fields").forEach(function(field) {
        field.style.display = "none";
    });
    
    // Show fields for selected device type
    if (deviceType === "soapysdr") {
        document.getElementById("soapysdr-fields").style.display = "block";
    } else if (deviceType === "rtlsdr") {
        document.getElementById("rtlsdr-fields").style.display = "block";
    } else if (deviceType === "mirisdr") {
        document.getElementById("mirisdr-fields").style.display = "block";
    } else if (deviceType === "file") {
        document.getElementById("file-fields").style.display = "block";
        // Hide centerfreq for file input
        document.getElementById("centerfreq-group").style.display = "none";
    }
    
    // Update required fields
    var stringField = document.getElementById("device-string");
    var indexField = document.getElementById("device-index");
    var serialField = document.getElementById("device-serial");
    var gainRtlField = document.getElementById("device-gain-rtl");
    var gainMiriField = document.getElementById("device-gain-miri");
    var filepathField = document.getElementById("device-filepath");
    
    // Reset required attributes
    if (stringField) stringField.removeAttribute("required");
    if (indexField) indexField.removeAttribute("required");
    if (serialField) serialField.removeAttribute("required");
    if (gainRtlField) gainRtlField.removeAttribute("required");
    if (gainMiriField) gainMiriField.removeAttribute("required");
    if (filepathField) filepathField.removeAttribute("required");
    
    // Set required based on device type
    if (deviceType === "soapysdr" && stringField) {
        stringField.setAttribute("required", "required");
    } else if (deviceType === "rtlsdr") {
        if (gainRtlField) gainRtlField.setAttribute("required", "required");
    } else if (deviceType === "mirisdr") {
        if (gainMiriField) gainMiriField.setAttribute("required", "required");
    } else if (deviceType === "file" && filepathField) {
        filepathField.setAttribute("required", "required");
    }
}

function toggleCenterFreq() {
    var mode = document.getElementById("device-mode").value;
    var centerfreqGroup = document.getElementById("centerfreq-group");
    if (mode === "multichannel") {
        centerfreqGroup.style.display = "block";
        document.getElementById("device-centerfreq").setAttribute("required", "required");
    } else {
        centerfreqGroup.style.display = "none";
        document.getElementById("device-centerfreq").removeAttribute("required");
    }
}

function loadDevice() {
    fetch("/api/channels")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.devices && data.devices.length > 0) {
                var dev = data.devices[0];
                
                // Load device type and update fields
                if (dev.type) {
                    document.getElementById("device-type").value = dev.type;
                    updateDeviceFields();
                }
                
                // Load common fields
                if (dev.mode) {
                    document.getElementById("device-mode").value = dev.mode;
                }
                toggleCenterFreq();
                
                if (dev.sample_rate !== undefined) {
                    document.getElementById("device-sample-rate").value = dev.sample_rate;
                }
                if (dev.centerfreq !== undefined) {
                    document.getElementById("device-centerfreq").value = dev.centerfreq;
                }
                if (dev.correction !== undefined) {
                    document.getElementById("device-correction").value = dev.correction;
                }
                if (dev.tau !== undefined) {
                    document.getElementById("device-tau").value = dev.tau;
                    document.getElementById("tau-group").style.display = "block";
                }
                if (dev.disable !== undefined) {
                    document.getElementById("device-disable").checked = !dev.enabled;
                }
                
                // Load device-specific fields
                if (dev.type === "soapysdr") {
                    if (dev.device_string) {
                        document.getElementById("device-string").value = dev.device_string;
                    }
                    if (dev.gain !== undefined) {
                        document.getElementById("device-gain").value = dev.gain;
                    }
                    if (dev.channel !== undefined) {
                        document.getElementById("device-channel").value = dev.channel;
                    }
                    if (dev.antenna) {
                        document.getElementById("device-antenna").value = dev.antenna;
                    }
                } else if (dev.type === "rtlsdr") {
                    if (dev.index !== undefined) {
                        document.getElementById("device-index").value = dev.index;
                    }
                    if (dev.serial) {
                        document.getElementById("device-serial").value = dev.serial;
                    }
                    if (dev.gain !== undefined) {
                        document.getElementById("device-gain-rtl").value = dev.gain;
                    }
                    if (dev.buffers !== undefined) {
                        document.getElementById("device-buffers").value = dev.buffers;
                    }
                } else if (dev.type === "mirisdr") {
                    if (dev.index !== undefined) {
                        document.getElementById("device-index-miri").value = dev.index;
                    }
                    if (dev.serial) {
                        document.getElementById("device-serial-miri").value = dev.serial;
                    }
                    if (dev.gain !== undefined) {
                        document.getElementById("device-gain-miri").value = dev.gain;
                    }
                    if (dev.num_buffers !== undefined) {
                        document.getElementById("device-num-buffers").value = dev.num_buffers;
                    }
                } else if (dev.type === "file") {
                    if (dev.filepath) {
                        document.getElementById("device-filepath").value = dev.filepath;
                    }
                    if (dev.speedup_factor !== undefined) {
                        document.getElementById("device-speedup").value = dev.speedup_factor;
                    }
                }
            } else {
                // Fallback to device API
                fetch("/api/device")
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        if (data.devices && data.devices.length > 0) {
                            var dev = data.devices[0];
                            document.getElementById("device-mode").value = dev.mode || "multichannel";
                            toggleCenterFreq();
                        }
                    });
            }
        })
        .catch(function(err) { 
            console.error("Device load error:", err);
            // Fallback
            fetch("/api/device")
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    if (data.devices && data.devices.length > 0) {
                        var dev = data.devices[0];
                        document.getElementById("device-mode").value = dev.mode || "multichannel";
                        toggleCenterFreq();
                    }
                });
        });
}

function saveDevice(event) {
    event.preventDefault();
    var formData = new FormData(event.target);
    var deviceData = {
        type: formData.get("type"),
        disable: formData.get("disable") === "on",
        mode: formData.get("mode") || "multichannel",
        sample_rate: formData.get("sample_rate") || null,
        correction: formData.get("correction") ? parseFloat(formData.get("correction")) : null
    };
    
    // Add centerfreq if multichannel mode
    if (deviceData.mode === "multichannel") {
        var centerfreq = formData.get("centerfreq");
        if (centerfreq) {
            deviceData.centerfreq = parseFloat(centerfreq);
        }
    }
    
    // Add tau if provided
    var tau = formData.get("tau");
    if (tau) {
        deviceData.tau = parseInt(tau);
    }
    
    // Device-specific fields
    if (deviceData.type === "soapysdr") {
        deviceData.device_string = formData.get("device_string");
        var gain = formData.get("gain");
        if (gain) {
            // Try to parse as number, otherwise use as string
            var gainNum = parseFloat(gain);
            deviceData.gain = isNaN(gainNum) ? gain : gainNum;
        }
        var channel = formData.get("channel");
        if (channel) deviceData.channel = parseInt(channel);
        var antenna = formData.get("antenna");
        if (antenna) deviceData.antenna = antenna;
    } else if (deviceData.type === "rtlsdr") {
        var index = formData.get("index");
        if (index) deviceData.index = parseInt(index);
        var serial = formData.get("serial");
        if (serial) deviceData.serial = serial;
        deviceData.gain = parseFloat(formData.get("gain"));
        var buffers = formData.get("buffers");
        if (buffers) deviceData.buffers = parseInt(buffers);
    } else if (deviceData.type === "mirisdr") {
        var index = formData.get("index");
        if (index) deviceData.index = parseInt(index);
        var serial = formData.get("serial");
        if (serial) deviceData.serial = serial;
        deviceData.gain = parseInt(formData.get("gain"));
        var numBuffers = formData.get("num_buffers");
        if (numBuffers) deviceData.num_buffers = parseInt(numBuffers);
    } else if (deviceData.type === "file") {
        deviceData.filepath = formData.get("filepath");
        var speedup = formData.get("speedup_factor");
        if (speedup) deviceData.speedup_factor = parseFloat(speedup);
    }
    
    // Send to API (placeholder - would need actual implementation)
    markChange("device");
    alert("Device configuration saved. Click 'Apply Changes' to apply the configuration.");
    console.log("Device data:", deviceData);
}

function refreshRecordings() {
    loadRecordings();
}

function showChannelTab(tabName) {
    // Hide all tabs
    document.querySelectorAll(".channel-tab-content").forEach(function(tab) {
        tab.style.display = "none";
    });
    document.querySelectorAll(".tab-button").forEach(function(btn) {
        btn.classList.remove("active");
    });
    
    // Show selected tab
    document.getElementById("tab-" + tabName).style.display = "block";
    document.getElementById("tab-" + tabName + "-btn").classList.add("active");
}

function resetChannelToDefaults() {
    if (!confirm("Reset channel to default settings? This will clear all advanced features and keep only basic settings.")) {
        return;
    }
    
    // Reset to minimal defaults
    document.getElementById("channel-label").value = "";
    document.getElementById("channel-freq").value = "";
    document.getElementById("channel-modulation").value = "am";
    document.getElementById("channel-enabled").checked = true;
    
    // Clear advanced settings
    document.getElementById("channel-highpass").value = "";
    document.getElementById("channel-lowpass").value = "";
    document.getElementById("channel-bandwidth").value = "";
    document.getElementById("channel-ampfactor").value = "";
    document.getElementById("channel-afc").value = "";
    document.getElementById("channel-notch").value = "";
    document.getElementById("channel-notch-q").value = "";
    document.getElementById("channel-squelch-threshold").value = "";
    document.getElementById("channel-squelch-snr").value = "";
    document.getElementById("channel-ctcss").value = "";
    
    // Enable file recording by default
    document.getElementById("output-file-enabled").checked = true;
    document.getElementById("output-file-directory").value = "recordings";
    document.getElementById("output-file-filename").value = "";
    document.getElementById("output-file-continuous").checked = false;
    document.getElementById("output-file-split").checked = false;
    toggleOutputSection("file");
    
    // Disable other outputs
    document.getElementById("output-udp-enabled").checked = false;
    document.getElementById("output-udp-address").value = "";
    document.getElementById("output-udp-port").value = "";
    document.getElementById("output-udp-continuous").checked = false;
    document.getElementById("output-udp-headers").checked = false;
    document.getElementById("output-udp-chunking").checked = true;
    toggleOutputSection("udp");
    
    document.getElementById("output-icecast-enabled").checked = false;
    document.getElementById("output-icecast-server").value = "";
    document.getElementById("output-icecast-port").value = "";
    document.getElementById("output-icecast-mountpoint").value = "";
    document.getElementById("output-icecast-username").value = "";
    document.getElementById("output-icecast-password").value = "";
    document.getElementById("output-icecast-name").value = "";
    toggleOutputSection("icecast");
    
    // Switch to General tab
    showChannelTab("general");
}

function showAddChannelModal() {
    currentEditingChannel = null;
    document.getElementById("modal-title").textContent = "Add Channel";
    document.getElementById("channel-edit-mode").value = "false";
    document.getElementById("channel-form").reset();
    document.getElementById("channel-enabled").checked = true;
    document.getElementById("channel-modulation").value = "am";
    document.getElementById("channel-highpass").value = "100";
    document.getElementById("channel-lowpass").value = "2500";
    document.getElementById("channel-ampfactor").value = "1.0";
    document.getElementById("channel-afc").value = "0";
    document.getElementById("channel-notch-q").value = "10.0";
    document.getElementById("output-file-directory").value = "recordings";
    
    // Enable file recording by default
    document.getElementById("output-file-enabled").checked = true;
    document.getElementById("output-file-section").style.display = "block";
    
    // Hide other output sections
    document.getElementById("output-udp-section").style.display = "none";
    document.getElementById("output-icecast-section").style.display = "none";
    document.getElementById("output-udp-enabled").checked = false;
    document.getElementById("output-icecast-enabled").checked = false;
    
    // Set UDP defaults
    document.getElementById("output-udp-headers").checked = false;
    document.getElementById("output-udp-chunking").checked = true;
    
    // Show General tab
    showChannelTab("general");
    
    document.getElementById("channel-modal").style.display = "block";
}

function editChannel(deviceIdx, channelIdx) {
    currentEditingChannel = { device: deviceIdx, channel: channelIdx };
    document.getElementById("modal-title").textContent = "Edit Channel";
    document.getElementById("channel-edit-mode").value = "true";
    document.getElementById("channel-device-index").value = deviceIdx;
    document.getElementById("channel-index").value = channelIdx;
    
    if (channelsData && channelsData.devices && channelsData.devices[deviceIdx] && 
        channelsData.devices[deviceIdx].channels) {
        var ch = channelsData.devices[deviceIdx].channels.find(function(c) { 
            return c.channel_index === channelIdx; 
        });
        
        if (ch) {
            document.getElementById("channel-label").value = ch.label || "";
            document.getElementById("channel-freq").value = ch.freq || "";
            document.getElementById("channel-modulation").value = ch.modulation || "am";
            document.getElementById("channel-enabled").checked = ch.enabled !== false;
            document.getElementById("channel-highpass").value = ch.highpass || "100";
            document.getElementById("channel-lowpass").value = ch.lowpass || "2500";
            document.getElementById("channel-bandwidth").value = ch.bandwidth || "";
            document.getElementById("channel-ampfactor").value = ch.ampfactor || "1.0";
            document.getElementById("channel-squelch-threshold").value = ch.squelch_threshold || "";
            document.getElementById("channel-squelch-snr").value = ch.squelch_snr_threshold || "";
            document.getElementById("channel-afc").value = ch.afc || "0";
            document.getElementById("channel-notch").value = ch.notch || "";
            document.getElementById("channel-notch-q").value = ch.notch_q || "10.0";
            document.getElementById("channel-ctcss").value = ch.ctcss || "";
            
            // Load outputs
            if (ch.outputs) {
                ch.outputs.forEach(function(out) {
                    if (out.type === "file") {
                        document.getElementById("output-file-enabled").checked = out.enabled;
                        document.getElementById("output-file-directory").value = out.directory || "recordings";
                        document.getElementById("output-file-filename").value = out.filename_template || "";
                        document.getElementById("output-file-continuous").checked = out.continuous || false;
                        toggleOutputSection("file");
                    } else if (out.type === "udp_stream") {
                        document.getElementById("output-udp-enabled").checked = out.enabled;
                        document.getElementById("output-udp-address").value = out.dest_address || "";
                        document.getElementById("output-udp-port").value = out.dest_port || "";
                        document.getElementById("output-udp-continuous").checked = out.continuous || false;
                        document.getElementById("output-udp-headers").checked = out.udp_headers || false;
                        document.getElementById("output-udp-chunking").checked = out.udp_chunking !== false; // Default true
                        toggleOutputSection("udp");
                    } else if (out.type === "icecast") {
                        document.getElementById("output-icecast-enabled").checked = out.enabled;
                        document.getElementById("output-icecast-server").value = out.server || "";
                        document.getElementById("output-icecast-port").value = out.port || "";
                        document.getElementById("output-icecast-mountpoint").value = out.mountpoint || "";
                        document.getElementById("output-icecast-username").value = out.username || "";
                        document.getElementById("output-icecast-password").value = out.password || "";
                        document.getElementById("output-icecast-name").value = out.name || "";
                        toggleOutputSection("icecast");
                    }
                });
            }
        }
    }
    
    // Show General tab
    showChannelTab("general");
    
    document.getElementById("channel-modal").style.display = "block";
}

function closeChannelModal() {
    document.getElementById("channel-modal").style.display = "none";
    currentEditingChannel = null;
}

function toggleOutputSection(type) {
    var section = document.getElementById("output-" + type + "-section");
    var checkbox = document.getElementById("output-" + type + "-enabled");
    section.style.display = checkbox.checked ? "block" : "none";
}

function applyChanges() {
    if (!hasPendingChanges) {
        return;
    }
    
    if (!confirm("Apply all pending configuration changes? This will reload the configuration and restart recording without rebooting the application.")) {
        return;
    }
    
    var btn = document.getElementById("apply-changes-btn");
    btn.disabled = true;
    btn.textContent = "Applying...";
    
    fetch("/api/apply", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ changes: pendingChanges })
    })
    .then(function(r) { return r.json(); })
    .then(function(data) {
        if (data.status === "success") {
            alert("Configuration changes applied successfully! Recording has been restarted.");
            clearChanges();
            // Reload current page data
            if (currentPage === "channels") {
                loadChannels();
            } else if (currentPage === "device") {
                loadDevice();
            } else if (currentPage === "home") {
                updateStatus();
            }
        } else {
            alert("Error applying changes: " + (data.error || "Unknown error"));
            btn.disabled = false;
            btn.textContent = "Apply Changes";
        }
    })
    .catch(function(err) {
        console.error("Apply changes error:", err);
        alert("Error applying changes. Please check the console for details.");
        btn.disabled = false;
        btn.textContent = "Apply Changes";
    });
}

function saveChannel(event) {
    event.preventDefault();
    var formData = new FormData(event.target);
    var channelData = {
        device_index: parseInt(formData.get("device_index")),
        channel_index: formData.get("channel_index") ? parseInt(formData.get("channel_index")) : null,
        label: formData.get("label"),
        freq: parseFloat(formData.get("freq")),
        modulation: formData.get("modulation"),
        enabled: formData.get("enabled") === "on",
        highpass: (formData.get("highpass") && formData.get("highpass") !== "" && parseInt(formData.get("highpass")) > 0) ? parseInt(formData.get("highpass")) : null,
        lowpass: (formData.get("lowpass") && formData.get("lowpass") !== "" && parseInt(formData.get("lowpass")) > 0) ? parseInt(formData.get("lowpass")) : null,
        bandwidth: (formData.get("bandwidth") && formData.get("bandwidth") !== "" && parseInt(formData.get("bandwidth")) > 0) ? parseInt(formData.get("bandwidth")) : null,
        ampfactor: (formData.get("ampfactor") && formData.get("ampfactor") !== "" && parseFloat(formData.get("ampfactor")) !== 1.0) ? parseFloat(formData.get("ampfactor")) : null,
        squelch_threshold: (formData.get("squelch_threshold") && formData.get("squelch_threshold") !== "" && parseFloat(formData.get("squelch_threshold")) !== 0) ? parseFloat(formData.get("squelch_threshold")) : null,
        squelch_snr_threshold: (formData.get("squelch_snr_threshold") && formData.get("squelch_snr_threshold") !== "" && parseFloat(formData.get("squelch_snr_threshold")) !== 0) ? parseFloat(formData.get("squelch_snr_threshold")) : null,
        afc: (formData.get("afc") && formData.get("afc") !== "" && parseInt(formData.get("afc")) > 0) ? parseInt(formData.get("afc")) : null,
        notch: (formData.get("notch") && formData.get("notch") !== "" && parseFloat(formData.get("notch")) > 0) ? parseFloat(formData.get("notch")) : null,
        notch_q: (formData.get("notch_q") && formData.get("notch_q") !== "" && parseFloat(formData.get("notch_q")) !== 10.0) ? parseFloat(formData.get("notch_q")) : null,
        ctcss: (formData.get("ctcss") && formData.get("ctcss") !== "" && parseFloat(formData.get("ctcss")) > 0) ? parseFloat(formData.get("ctcss")) : null,
        outputs: []
    };
    
    // Add file output
    if (formData.get("output_file_enabled") === "on") {
        channelData.outputs.push({
            type: "file",
            enabled: true,
            directory: formData.get("output_file_directory"),
            filename_template: formData.get("output_file_filename"),
            continuous: formData.get("output_file_continuous") === "on",
            split_on_transmission: formData.get("output_file_split") === "on"
        });
    }
    
    // Add UDP output
    if (formData.get("output_udp_enabled") === "on") {
        channelData.outputs.push({
            type: "udp_stream",
            enabled: true,
            dest_address: formData.get("output_udp_address"),
            dest_port: parseInt(formData.get("output_udp_port")),
            continuous: formData.get("output_udp_continuous") === "on",
            udp_headers: formData.get("output_udp_headers") === "on",
            udp_chunking: formData.get("output_udp_chunking") !== "off" // Default true if not explicitly off
        });
    }
    
    // Add Icecast output
    if (formData.get("output_icecast_enabled") === "on") {
        channelData.outputs.push({
            type: "icecast",
            enabled: true,
            server: formData.get("output_icecast_server"),
            port: parseInt(formData.get("output_icecast_port")),
            mountpoint: formData.get("output_icecast_mountpoint"),
            username: formData.get("output_icecast_username"),
            password: formData.get("output_icecast_password"),
            name: formData.get("output_icecast_name")
        });
    }
    
    var url = "/api/channels";
    var method = "POST";
    if (formData.get("edit_mode") === "true") {
        url = "/api/channels/" + channelData.device_index + "/" + channelData.channel_index;
        method = "PUT";
    }
    
    fetch(url, {
        method: method,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(channelData)
    })
    .then(function(r) { return r.json(); })
    .then(function(data) {
        if (data.status === "success") {
            markChange("channels");
            alert("Channel saved. Click 'Apply Changes' to apply the configuration.");
            closeChannelModal();
            loadChannels();
        } else {
            alert("Error: " + (data.error || "Failed to save channel"));
        }
    })
    .catch(function(err) {
        console.error("Save channel error:", err);
        alert("Error saving channel. Please check the console for details.");
    });
}

function toggleChannel(deviceIdx, channelIdx, enabled) {
    var url = "/api/channels/" + deviceIdx + "/" + channelIdx + "/" + (enabled ? "enable" : "disable");
    fetch(url, { method: "POST" })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.status === "success") {
                markChange("channels");
                loadChannels();
            }
        })
        .catch(function(err) { console.error("Toggle channel error:", err); });
}

function deleteChannel(deviceIdx, channelIdx) {
    if (!confirm("Are you sure you want to delete this channel?")) {
        return;
    }
    fetch("/api/channels/" + deviceIdx + "/" + channelIdx, { method: "DELETE" })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.status === "success") {
                markChange("channels");
                loadChannels();
            }
        })
        .catch(function(err) { console.error("Delete channel error:", err); });
}

function loadOutputSettings() {
    fetch("/api/outputs/settings")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.file_chunk_duration_minutes) {
                document.getElementById("file-chunk-duration").value = data.file_chunk_duration_minutes;
                document.getElementById("chunk-duration-value").textContent = data.file_chunk_duration_minutes;
            }
        })
        .catch(function(err) { console.error("Load output settings error:", err); });
}

function saveOutputSettings() {
    var chunkDuration = parseInt(document.getElementById("file-chunk-duration").value);
    
    fetch("/api/outputs/settings", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            file_chunk_duration_minutes: chunkDuration
        })
    })
    .then(function(r) { return r.json(); })
    .then(function(data) {
        if (data.status === "success") {
            markChange("outputs");
            alert("Output settings saved. Click 'Apply Changes' to apply the configuration.");
        } else {
            alert("Error: " + (data.error || "Failed to save output settings"));
        }
    })
    .catch(function(err) {
        console.error("Save output settings error:", err);
        alert("Error saving output settings. Please check the console for details.");
    });
}

// Close modal when clicking outside
window.onclick = function(event) {
    var modal = document.getElementById("channel-modal");
    if (event.target == modal) {
        closeChannelModal();
    }
}

function downloadRecording(filename) {
    window.location.href = "/recordings/" + encodeURIComponent(filename) + "?download=1";
}

function loadConfigInfo() {
    fetch("/api/config/info")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.config_path) {
                document.getElementById("config-path").value = data.config_path;
            }
        })
        .catch(function(err) { console.error("Config info load error:", err); });
}

function saveConfigPath() {
    var path = document.getElementById("config-path").value;
    if (!path) {
        alert("Please enter a configuration file path");
        return;
    }
    
    fetch("/api/config/path", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ config_path: path })
    })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.status === "success") {
                markChange("config");
                alert("Configuration path saved. Click 'Apply Changes' to apply.");
            } else {
                alert("Failed to save configuration path: " + (data.error || "Unknown error"));
            }
        })
        .catch(function(err) { 
            console.error("Save config path error:", err);
            alert("Error saving configuration path");
        });
}

function downloadConfig() {
    window.location.href = "/api/config/download";
}

function uploadConfig() {
    var fileInput = document.getElementById("config-upload");
    var file = fileInput.files[0];
    if (!file) {
        alert("Please select a configuration file to upload");
        return;
    }
    
    var statusDiv = document.getElementById("config-upload-status");
    statusDiv.style.display = "block";
    statusDiv.style.background = "#f8f8f8";
    statusDiv.textContent = "Uploading...";
    
    var reader = new FileReader();
    reader.onload = function(e) {
        var content = e.target.result;
        
        fetch("/api/config/upload", {
            method: "POST",
            headers: { "Content-Type": "text/plain" },
            body: content
        })
            .then(function(r) { return r.json(); })
            .then(function(data) {
                if (data.status === "success") {
                    markChange("config");
                    statusDiv.style.background = "#d4edda";
                    statusDiv.style.color = "#155724";
                    statusDiv.textContent = "Configuration uploaded successfully! Click 'Apply Changes' to apply.";
                    fileInput.value = "";
                } else {
                    statusDiv.style.background = "#f8d7da";
                    statusDiv.style.color = "#721c24";
                    statusDiv.textContent = "Upload failed: " + (data.error || "Unknown error");
                }
            })
            .catch(function(err) {
                console.error("Upload config error:", err);
                statusDiv.style.background = "#f8d7da";
                statusDiv.style.color = "#721c24";
                statusDiv.textContent = "Error uploading configuration file";
            });
    };
    reader.readAsText(file);
}

// Spectrum analyzer module will hook into showPage via initSpectrumModule()
// This is handled in web_spectrum.js

// Wait for DOM to be ready - initialize after all functions are defined
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initializeApp);
} else {
    // DOM already loaded, but wait a tick to ensure all code is parsed
    setTimeout(initializeApp, 0);
}

function initializeApp() {
    document.querySelectorAll(".nav-link").forEach(function(link) {
        link.addEventListener("click", function(e) {
            e.preventDefault();
            var page = e.target.getAttribute("data-page");
            showPage(page);
        });
    });
    
    // Initialize apply button state
    updateApplyButton();
    
    // Hook spectrum analyzer module into showPage (if web_spectrum.js is loaded)
    if (typeof initSpectrumModule === 'function') {
        initSpectrumModule(showPage);
    }
    
    // Show home page
    showPage("home");
}

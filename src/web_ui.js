var currentPage = "home";
var statusInterval;
var hasPendingChanges = false;
var pendingChanges = {
    device: false,
    channels: false,
    outputs: false,
    config: false
};

// Custom Modal System
var customModalCallback = null;

function showCustomModal(title, message, type, buttons) {
    var modal = document.getElementById("custom-modal");
    var titleEl = document.getElementById("custom-modal-title");
    var messageEl = document.getElementById("custom-modal-message");
    var footerEl = document.getElementById("custom-modal-footer");
    
    titleEl.textContent = title;
    messageEl.textContent = message;
    footerEl.innerHTML = "";
    
    // Default buttons based on type
    if (!buttons) {
        if (type === "confirm") {
            buttons = [
                { text: "Cancel", class: "btn-secondary", action: "cancel" },
                { text: "OK", class: "btn-primary", action: "ok" }
            ];
        } else if (type === "yesno") {
            buttons = [
                { text: "No", class: "btn-secondary", action: "no" },
                { text: "Yes", class: "btn-primary", action: "yes" }
            ];
        } else {
            buttons = [
                { text: "OK", class: "btn-primary", action: "ok" }
            ];
        }
    }
    
    // Create buttons
    buttons.forEach(function(btn) {
        var button = document.createElement("button");
        button.className = "btn " + btn.class;
        button.textContent = btn.text;
        button.onclick = function() {
            if (customModalCallback) {
                customModalCallback(btn.action);
            }
            closeCustomModal();
        };
        footerEl.appendChild(button);
    });
    
    modal.style.display = "block";
}

function closeCustomModal() {
    var modal = document.getElementById("custom-modal");
    modal.style.display = "none";
    customModalCallback = null;
}

// Close modal when clicking outside
window.onclick = function(event) {
    var modal = document.getElementById("custom-modal");
    if (event.target === modal) {
        closeCustomModal();
        if (customModalCallback) {
            customModalCallback("cancel");
        }
    }
}

// Replace alert() function
function showAlert(message, title) {
    return new Promise(function(resolve) {
        customModalCallback = function(action) {
            resolve(action);
        };
        showCustomModal(title || "Alert", message, "alert");
    });
}

// Replace confirm() function
function showConfirm(message, title) {
    return new Promise(function(resolve) {
        customModalCallback = function(action) {
            resolve(action === "ok" || action === "yes");
        };
        showCustomModal(title || "Confirm", message, "confirm");
    });
}

// Yes/No confirmation
function showYesNo(message, title) {
    return new Promise(function(resolve) {
        customModalCallback = function(action) {
            resolve(action === "yes");
        };
        showCustomModal(title || "Confirm", message, "yesno");
    });
}

function markChange(type) {
    // No-op: Changes are now applied automatically when starting capture
    // This function is kept for compatibility but does nothing
}

function updateApplyButton() {
    // No-op: apply button removed; keep stub for legacy calls
}

function clearChanges() {
    // No-op: Changes are now applied automatically when starting capture
    // This function is kept for compatibility but does nothing
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
    checkCaptureStatus();  // Check initial capture status
    statusInterval = setInterval(updateStatus, 1000);
    setInterval(checkCaptureStatus, 3000);  // Check capture status periodically
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

var allRecordings = [];
var filteredRecordings = [];
var selectedChannels = new Set();
var currentRecordingsPage = 1;
var recordingsPerPage = 10;

function loadRecordings() {
    fetch("/api/recordings")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            allRecordings = data.recordings || [];
            
            // Extract unique channel names
            var channelNames = new Set();
            allRecordings.forEach(function(rec) {
                if (rec.channel_name) {
                    channelNames.add(rec.channel_name);
                }
            });
            
            // Build channel filter checkboxes
            var filterContainer = document.getElementById("channel-filter-checkboxes");
            filterContainer.innerHTML = "";
            var sortedChannels = Array.from(channelNames).sort();
            sortedChannels.forEach(function(channelName) {
                var label = document.createElement("label");
                label.style.display = "block";
                label.style.padding = "5px 0";
                label.style.cursor = "pointer";
                label.style.color = "#e0e0e0";
                
                var checkbox = document.createElement("input");
                checkbox.type = "checkbox";
                checkbox.value = channelName;
                checkbox.checked = true;  // All selected by default
                checkbox.onchange = function() {
                    if (this.checked) {
                        selectedChannels.add(channelName);
                    } else {
                        selectedChannels.delete(channelName);
                    }
                    applyRecordingsFilter();
                };
                
                selectedChannels.add(channelName);
                
                label.appendChild(checkbox);
                label.appendChild(document.createTextNode(" " + channelName));
                filterContainer.appendChild(label);
            });
            
            // Apply filter and show recordings
            applyRecordingsFilter();
        })
        .catch(function(err) { console.error("Recordings load error:", err); });
}

function applyRecordingsFilter() {
    // Filter recordings by selected channels
    if (selectedChannels.size === 0) {
        filteredRecordings = [];
    } else {
        filteredRecordings = allRecordings.filter(function(rec) {
            return selectedChannels.has(rec.channel_name);
        });
    }
    
    // Update filter count
    var filterCount = document.getElementById("channel-filter-count");
    var allChannelNames = new Set();
    allRecordings.forEach(function(rec) {
        if (rec.channel_name) {
            allChannelNames.add(rec.channel_name);
        }
    });
    
    if (selectedChannels.size === 0) {
        filterCount.textContent = "(None)";
    } else if (selectedChannels.size === allChannelNames.size) {
        filterCount.textContent = "(All)";
    } else {
        filterCount.textContent = "(" + selectedChannels.size + " selected)";
    }
    
    // Reset to first page
    currentRecordingsPage = 1;
    displayRecordings();
}

function displayRecordings() {
            var tbody = document.getElementById("recordings-tbody");
            tbody.innerHTML = "";
    
    // Calculate pagination
    var totalPages = Math.ceil(filteredRecordings.length / recordingsPerPage);
    var startIdx = (currentRecordingsPage - 1) * recordingsPerPage;
    var endIdx = Math.min(startIdx + recordingsPerPage, filteredRecordings.length);
    
    // Update pagination info
    document.getElementById("recordings-range-start").textContent = filteredRecordings.length > 0 ? (startIdx + 1) : 0;
    document.getElementById("recordings-range-end").textContent = endIdx;
    document.getElementById("recordings-total").textContent = filteredRecordings.length;
    document.getElementById("recordings-current-page").textContent = currentRecordingsPage;
    document.getElementById("recordings-total-pages").textContent = totalPages || 1;
    
    // Update pagination buttons
    var prevBtn = document.getElementById("recordings-prev-btn");
    var nextBtn = document.getElementById("recordings-next-btn");
    prevBtn.disabled = (currentRecordingsPage <= 1);
    nextBtn.disabled = (currentRecordingsPage >= totalPages);
    
    // Display current page of recordings
    for (var i = startIdx; i < endIdx; i++) {
        var rec = filteredRecordings[i];
                var row = document.createElement("tr");
                var sizeMB = (rec.size / 1024 / 1024).toFixed(2);
        
        // Build file path - need to include channel name in path
        var filePath = rec.path || ("/recordings/" + encodeURIComponent(rec.filename));
        
        // Use filename for path - server will search all directories
        var audioPath = "/recordings/" + encodeURIComponent(rec.filename);
        
        row.innerHTML = "<td>" + (rec.channel_name || "Unknown") + "</td>" +
            "<td>" + rec.datetime + "</td>" +
                    "<td>" + sizeMB + " MB</td>" +
            "<td><audio controls style=\"width: 200px;\"><source src=\"" + audioPath + "\" type=\"audio/mpeg\"></audio></td>" +
            "<td><button class=\"btn btn-primary\" onclick=\"downloadRecording('" + rec.filename.replace(/'/g, "\\'") + "')\" style=\"font-size: 12px; padding: 5px 10px;\">Download</button></td>";
                tbody.appendChild(row);
    }
}

function changeRecordingsPage(delta) {
    var totalPages = Math.ceil(filteredRecordings.length / recordingsPerPage);
    var newPage = currentRecordingsPage + delta;
    if (newPage >= 1 && newPage <= totalPages) {
        currentRecordingsPage = newPage;
        displayRecordings();
    }
}

function toggleChannelFilter() {
    var dropdown = document.getElementById("channel-filter-dropdown");
    dropdown.style.display = dropdown.style.display === "none" ? "block" : "none";
}

function selectAllChannels() {
    var checkboxes = document.querySelectorAll("#channel-filter-checkboxes input[type='checkbox']");
    checkboxes.forEach(function(cb) {
        cb.checked = true;
        selectedChannels.add(cb.value);
    });
    applyRecordingsFilter();
}

function deselectAllChannels() {
    var checkboxes = document.querySelectorAll("#channel-filter-checkboxes input[type='checkbox']");
    checkboxes.forEach(function(cb) {
        cb.checked = false;
        selectedChannels.delete(cb.value);
    });
    applyRecordingsFilter();
}

// Close filter dropdown when clicking outside
document.addEventListener('click', function(event) {
    var filterBtn = document.querySelector('button[onclick="toggleChannelFilter()"]');
    var dropdown = document.getElementById("channel-filter-dropdown");
    if (dropdown && filterBtn && !filterBtn.contains(event.target) && !dropdown.contains(event.target)) {
        dropdown.style.display = "none";
    }
});

var currentEditingChannel = null;
var channelsData = null;
var deviceInfo = null; // Store device center freq and sample rate
var allChannelsList = []; // Store all channels for pagination
var channelsPerPage = 10;
var currentChannelsPage = 1;
var MAX_ACTIVE_CHANNELS = 8;

function loadChannels() {
    fetch("/api/channels")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            channelsData = data;
            
            // Store device info (center freq, sample rate) for filtering
            if (data.devices && data.devices.length > 0) {
                deviceInfo = data.devices[0]; // Use first device
                
                // Initialize center frequency input
                if (deviceInfo.centerfreq) {
                    var centerFreqMhz = parseFloat(deviceInfo.centerfreq);
                    if (typeof deviceInfo.centerfreq === 'string') {
                        // Parse if it's a string like "120.0" or "120000000"
                        centerFreqMhz = parseFloat(deviceInfo.centerfreq);
                        if (centerFreqMhz > 10000) {
                            // Likely in Hz, convert to MHz
                            centerFreqMhz = centerFreqMhz / 1000000;
                        }
                    } else {
                        // Already a number, might be in Hz
                        if (centerFreqMhz > 10000) {
                            centerFreqMhz = centerFreqMhz / 1000000;
                        }
                    }
                    document.getElementById("channels-center-freq").value = centerFreqMhz.toFixed(4);
                }
            }
            
            updateChannelFiltering();
        })
        .catch(function(err) { 
            console.error("Channels load error:", err);
        });
}

function renderChannelsTable() {
            var tbody = document.getElementById("channels-list-tbody");
            tbody.innerHTML = "";
            
    if (!channelsData || !channelsData.devices || channelsData.devices.length === 0) {
        allChannelsList = [];
        updateChannelsPagination();
        return;
    }
    
    // Get center frequency and range selection
    var centerFreqInput = document.getElementById("channels-center-freq");
    var centerFreqMhz = parseFloat(centerFreqInput.value) || 0;
    var centerFreqHz = centerFreqMhz * 1000000;
    
    // Get selected range (2.5 MHz or 10 MHz on each side)
    var rangeInput = document.querySelector('input[name="frequency-range"]:checked');
    var rangeMhz = rangeInput ? parseFloat(rangeInput.value) : 10; // Default to 10 MHz
    var bandwidthHz = rangeMhz * 1000000; // Convert to Hz
    var freqMin = centerFreqHz - bandwidthHz;
    var freqMax = centerFreqHz + bandwidthHz;
    
    // Build list of all channels with metadata
    allChannelsList = [];
    channelsData.devices.forEach(function(dev) {
                    if (dev.channels && dev.channels.length > 0) {
                        dev.channels.forEach(function(ch) {
                            var label = ch.label || ("Channel " + ch.channel_index);
                            var freq = ch.freq ? ch.freq.toFixed(3) : "N/A";
                            var modulation = ch.modulation || "am";
                
                // Check if channel is within range
                var channelFreqHz = ch.freq ? ch.freq * 1000000 : 0;
                var isInRange = (channelFreqHz >= freqMin && channelFreqHz <= freqMax);
                            
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
                            
                allChannelsList.push({
                    device: dev.device,
                    channel_index: ch.channel_index,
                    label: label,
                    freq: freq,
                    modulation: modulation,
                    squelch: squelch,
                    hasRecording: hasRecording,
                    hasStreaming: hasStreaming,
                    hasContinuous: hasContinuous,
                    enabled: ch.enabled,
                    isInRange: isInRange,
                    channelData: ch,
                    channelFreqHz: channelFreqHz
                });
            });
        }
    });
    
    // Sort channels: enabled and in-range first, then by frequency
    allChannelsList.sort(function(a, b) {
        // First, sort by enabled status (enabled first)
        if (a.enabled && !b.enabled) return -1;
        if (!a.enabled && b.enabled) return 1;
        // If both enabled or both disabled, sort by frequency
        return (a.channelFreqHz || 0) - (b.channelFreqHz || 0);
    });
    
    // Reset to first page if current page is out of bounds
    var totalPages = Math.ceil(allChannelsList.length / channelsPerPage);
    if (currentChannelsPage > totalPages && totalPages > 0) {
        currentChannelsPage = 1;
    }
    
    // Calculate pagination
    var startIdx = (currentChannelsPage - 1) * channelsPerPage;
    var endIdx = Math.min(startIdx + channelsPerPage, allChannelsList.length);
    
    // Render current page
    for (var i = startIdx; i < endIdx; i++) {
        var ch = allChannelsList[i];
        var row = document.createElement("tr");
        
        // Apply grayed out style if outside range
        var rowStyle = ch.isInRange ? "" : "opacity: 0.4; color: #666;";
        var checkboxDisabled = ch.isInRange ? "" : "disabled";
        
        row.style.cssText = rowStyle;
                            row.innerHTML = 
            "<td><input type=\"checkbox\" class=\"channel-enable-checkbox\" data-device=\"" + ch.device + "\" data-channel=\"" + ch.channel_index + "\" " + (ch.enabled ? "checked" : "") + " " + checkboxDisabled + " onchange=\"toggleChannel(" + ch.device + "," + ch.channel_index + ", this.checked)\"></td>" +
            "<td>" + ch.label + "</td>" +
            "<td>" + ch.freq + " MHz</td>" +
            "<td>" + ch.modulation.toUpperCase() + "</td>" +
            "<td>" + ch.squelch + "</td>" +
            "<td>" + (ch.hasRecording ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
            "<td>" + (ch.hasStreaming ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
            "<td>" + (ch.hasContinuous ? "<span class=\"status-enabled\">Yes</span>" : "<span style=\"color: #999;\">No</span>") + "</td>" +
            "<td><button class=\"btn btn-primary\" onclick=\"editChannel(" + ch.device + "," + ch.channel_index + ")\" style=\"margin-right: 5px;\">Edit</button>" +
            "<button class=\"btn btn-danger\" onclick=\"deleteChannel(" + ch.device + "," + ch.channel_index + ")\">Delete</button></td>";
                            tbody.appendChild(row);
    }
    
    // Update pagination controls
    updateChannelsPagination();
    
    // Update frequency band visualization
    drawFrequencyBand(centerFreqHz, freqMin, freqMax);
}

function updateChannelsPagination() {
    var totalPages = Math.ceil(allChannelsList.length / channelsPerPage);
    var startIdx = (currentChannelsPage - 1) * channelsPerPage;
    var endIdx = Math.min(startIdx + channelsPerPage, allChannelsList.length);
    
    // Update pagination info
    document.getElementById("channels-range-start").textContent = allChannelsList.length > 0 ? (startIdx + 1) : 0;
    document.getElementById("channels-range-end").textContent = endIdx;
    document.getElementById("channels-total").textContent = allChannelsList.length;
    document.getElementById("channels-current-page").textContent = currentChannelsPage;
    document.getElementById("channels-total-pages").textContent = totalPages || 1;
    
    // Update pagination buttons
    var prevBtn = document.getElementById("channels-prev-btn");
    var nextBtn = document.getElementById("channels-next-btn");
    if (prevBtn) prevBtn.disabled = (currentChannelsPage <= 1);
    if (nextBtn) nextBtn.disabled = (currentChannelsPage >= totalPages);
}

function changeChannelsPage(delta) {
    var totalPages = Math.ceil(allChannelsList.length / channelsPerPage);
    var newPage = currentChannelsPage + delta;
    if (newPage >= 1 && newPage <= totalPages) {
        currentChannelsPage = newPage;
        renderChannelsTable();
    }
}

function exportChannelsToCSV() {
    if (!channelsData || !channelsData.devices || channelsData.devices.length === 0) {
        showAlert("No channels to export", "Export Channels");
        return;
    }
    
    // Escape commas and quotes in CSV
    var escapeCSV = function(str) {
        if (str === null || str === undefined) return "";
        str = String(str);
        if (str.indexOf(",") !== -1 || str.indexOf('"') !== -1 || str.indexOf("\n") !== -1) {
            return '"' + str.replace(/"/g, '""') + '"';
        }
        return str;
    };
    
    // Build CSV content with all parameters
    var csvRows = [];
    // Header row - include all channel parameters
    csvRows.push("Label,Frequency (MHz),Modulation,Enabled,Bandwidth,Highpass,Lowpass,AmpFactor,Squelch Threshold,Squelch SNR Threshold,AFC,Notch,Notch Q,CTCSS,File Output Enabled,File Directory,File Template,File Continuous,File Split on Transmission,File Include Freq,File Append,File Dated Subdirs,UDP Output Enabled,UDP Address,UDP Port,UDP Continuous,UDP Headers,UDP Chunking,Icecast Output Enabled,Icecast Server,Icecast Port,Icecast Mountpoint,Icecast Username,Icecast Password,Icecast Name,Boondock API Enabled,Boondock API URL,Boondock API Key,Redis Output Enabled,Redis Address,Redis Port,Redis Password,Redis Database");
    
    // Data rows
    channelsData.devices.forEach(function(dev) {
        if (dev.channels && dev.channels.length > 0) {
            dev.channels.forEach(function(ch) {
                var label = ch.label || ("Channel " + ch.channel_index);
                var freq = ch.freq ? ch.freq.toFixed(5) : "";
                var modulation = ch.modulation || "am";
                var enabled = ch.enabled ? "Yes" : "No";
                var bandwidth = ch.bandwidth || "";
                var highpass = ch.highpass !== undefined ? ch.highpass : "";
                var lowpass = ch.lowpass !== undefined ? ch.lowpass : "";
                var ampfactor = ch.ampfactor !== undefined ? ch.ampfactor : "";
                var squelch_threshold = ch.squelch_threshold !== undefined ? ch.squelch_threshold : "";
                var squelch_snr_threshold = ch.squelch_snr_threshold !== undefined ? ch.squelch_snr_threshold : "";
                var afc = ch.afc !== undefined ? ch.afc : "";
                var notch = ch.notch !== undefined ? ch.notch : "";
                var notch_q = ch.notch_q !== undefined ? ch.notch_q : "";
                var ctcss = ch.ctcss !== undefined ? ch.ctcss : "";
                
                // Initialize output fields
                var fileEnabled = "No", fileDir = "", fileTemplate = "", fileContinuous = "No", fileSplit = "No", fileIncludeFreq = "No", fileAppend = "No", fileDatedSubdirs = "No";
                var udpEnabled = "No", udpAddress = "", udpPort = "", udpContinuous = "No", udpHeaders = "No", udpChunking = "No";
                var icecastEnabled = "No", icecastServer = "", icecastPort = "", icecastMountpoint = "", icecastUsername = "", icecastPassword = "", icecastName = "";
                var boondockEnabled = "No", boondockUrl = "", boondockKey = "";
                var redisEnabled = "No", redisAddress = "", redisPort = "", redisPassword = "", redisDatabase = "";
                
                // Parse outputs
                if (ch.outputs) {
                    ch.outputs.forEach(function(out) {
                        if (out.enabled) {
                            if (out.type === "file") {
                                fileEnabled = "Yes";
                                fileDir = out.directory || "";
                                fileTemplate = out.filename_template || "";
                                fileContinuous = out.continuous ? "Yes" : "No";
                                fileSplit = out.split_on_transmission ? "Yes" : "No";
                                fileIncludeFreq = out.include_freq ? "Yes" : "No";
                                fileAppend = out.append ? "Yes" : "No";
                                fileDatedSubdirs = out.dated_subdirectories ? "Yes" : "No";
                            } else if (out.type === "udp_stream") {
                                udpEnabled = "Yes";
                                udpAddress = out.dest_address || "";
                                udpPort = out.dest_port || "";
                                udpContinuous = out.continuous ? "Yes" : "No";
                                udpHeaders = out.udp_headers ? "Yes" : "No";
                                udpChunking = out.udp_chunking ? "Yes" : "No";
                            } else if (out.type === "icecast") {
                                icecastEnabled = "Yes";
                                icecastServer = out.server || "";
                                icecastPort = out.port || "";
                                icecastMountpoint = out.mountpoint || "";
                                icecastUsername = out.username || "";
                                icecastPassword = out.password || "";
                                icecastName = out.name || "";
                            } else if (out.type === "boondock_api") {
                                boondockEnabled = "Yes";
                                boondockUrl = out.api_url || "";
                                boondockKey = out.api_key || "";
                            } else if (out.type === "redis") {
                                redisEnabled = "Yes";
                                redisAddress = out.address || "";
                                redisPort = out.port || "";
                                redisPassword = out.password || "";
                                redisDatabase = out.database || "";
                            }
                        }
                    });
                }
                
                csvRows.push([
                    escapeCSV(label),
                    escapeCSV(freq),
                    escapeCSV(modulation),
                    escapeCSV(enabled),
                    escapeCSV(bandwidth),
                    escapeCSV(highpass),
                    escapeCSV(lowpass),
                    escapeCSV(ampfactor),
                    escapeCSV(squelch_threshold),
                    escapeCSV(squelch_snr_threshold),
                    escapeCSV(afc),
                    escapeCSV(notch),
                    escapeCSV(notch_q),
                    escapeCSV(ctcss),
                    escapeCSV(fileEnabled),
                    escapeCSV(fileDir),
                    escapeCSV(fileTemplate),
                    escapeCSV(fileContinuous),
                    escapeCSV(fileSplit),
                    escapeCSV(fileIncludeFreq),
                    escapeCSV(fileAppend),
                    escapeCSV(fileDatedSubdirs),
                    escapeCSV(udpEnabled),
                    escapeCSV(udpAddress),
                    escapeCSV(udpPort),
                    escapeCSV(udpContinuous),
                    escapeCSV(udpHeaders),
                    escapeCSV(udpChunking),
                    escapeCSV(icecastEnabled),
                    escapeCSV(icecastServer),
                    escapeCSV(icecastPort),
                    escapeCSV(icecastMountpoint),
                    escapeCSV(icecastUsername),
                    escapeCSV(icecastPassword),
                    escapeCSV(icecastName),
                    escapeCSV(boondockEnabled),
                    escapeCSV(boondockUrl),
                    escapeCSV(boondockKey),
                    escapeCSV(redisEnabled),
                    escapeCSV(redisAddress),
                    escapeCSV(redisPort),
                    escapeCSV(redisPassword),
                    escapeCSV(redisDatabase)
                ].join(","));
            });
        }
    });
    
    // Create and download CSV file
    var csvContent = csvRows.join("\n");
    var blob = new Blob([csvContent], { type: "text/csv;charset=utf-8;" });
    var link = document.createElement("a");
    var url = URL.createObjectURL(blob);
    link.setAttribute("href", url);
    link.setAttribute("download", "channels_export_" + new Date().toISOString().split('T')[0] + ".csv");
    link.style.visibility = "hidden";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

function importChannelsFromCSV(event) {
    var file = event.target.files[0];
    if (!file) {
        return;
    }
    
    var reader = new FileReader();
    reader.onload = function(e) {
        try {
            var csv = e.target.result;
            var lines = csv.split("\n");
            
            if (lines.length < 2) {
                showAlert("CSV file must contain at least a header row and one data row", "Import Error");
                return;
            }
            
            // Helper function to parse CSV line (handles quoted fields)
            var parseCSVLine = function(line) {
                var fields = [];
                var currentField = "";
                var inQuotes = false;
                
                for (var j = 0; j < line.length; j++) {
                    var char = line[j];
                    if (char === '"') {
                        if (inQuotes && line[j + 1] === '"') {
                            currentField += '"';
                            j++;
                        } else {
                            inQuotes = !inQuotes;
                        }
                    } else if (char === ',' && !inQuotes) {
                        fields.push(currentField.trim());
                        currentField = "";
                    } else {
                        currentField += char;
                    }
                }
                fields.push(currentField.trim());
                return fields;
            };
            
            // Parse header to find column indices
            var header = parseCSVLine(lines[0]);
            var colIndices = {};
            
            for (var i = 0; i < header.length; i++) {
                var col = header[i].trim().toLowerCase();
                if (col.indexOf("label") !== -1) colIndices.label = i;
                else if (col.indexOf("frequency") !== -1) colIndices.freq = i;
                else if (col.indexOf("modulation") !== -1) colIndices.modulation = i;
                else if (col.indexOf("enabled") !== -1 && !colIndices.enabled) colIndices.enabled = i;
                else if (col.indexOf("bandwidth") !== -1) colIndices.bandwidth = i;
                else if (col.indexOf("highpass") !== -1) colIndices.highpass = i;
                else if (col.indexOf("lowpass") !== -1) colIndices.lowpass = i;
                else if (col.indexOf("ampfactor") !== -1 || col.indexOf("amp factor") !== -1) colIndices.ampfactor = i;
                else if (col.indexOf("squelch threshold") !== -1 && col.indexOf("snr") === -1) colIndices.squelch_threshold = i;
                else if (col.indexOf("squelch snr") !== -1) colIndices.squelch_snr_threshold = i;
                else if (col.indexOf("afc") !== -1) colIndices.afc = i;
                else if (col.indexOf("notch") !== -1 && col.indexOf("q") === -1) colIndices.notch = i;
                else if (col.indexOf("notch q") !== -1) colIndices.notch_q = i;
                else if (col.indexOf("ctcss") !== -1) colIndices.ctcss = i;
                // File output columns
                else if (col.indexOf("file output enabled") !== -1) colIndices.file_enabled = i;
                else if (col.indexOf("file directory") !== -1) colIndices.file_directory = i;
                else if (col.indexOf("file template") !== -1) colIndices.file_template = i;
                else if (col.indexOf("file continuous") !== -1) colIndices.file_continuous = i;
                else if (col.indexOf("file split") !== -1) colIndices.file_split = i;
                else if (col.indexOf("file include freq") !== -1) colIndices.file_include_freq = i;
                else if (col.indexOf("file append") !== -1) colIndices.file_append = i;
                else if (col.indexOf("file dated") !== -1) colIndices.file_dated_subdirs = i;
                // UDP output columns
                else if (col.indexOf("udp output enabled") !== -1) colIndices.udp_enabled = i;
                else if (col.indexOf("udp address") !== -1) colIndices.udp_address = i;
                else if (col.indexOf("udp port") !== -1) colIndices.udp_port = i;
                else if (col.indexOf("udp continuous") !== -1) colIndices.udp_continuous = i;
                else if (col.indexOf("udp headers") !== -1) colIndices.udp_headers = i;
                else if (col.indexOf("udp chunking") !== -1) colIndices.udp_chunking = i;
                // Icecast output columns
                else if (col.indexOf("icecast output enabled") !== -1) colIndices.icecast_enabled = i;
                else if (col.indexOf("icecast server") !== -1) colIndices.icecast_server = i;
                else if (col.indexOf("icecast port") !== -1) colIndices.icecast_port = i;
                else if (col.indexOf("icecast mountpoint") !== -1) colIndices.icecast_mountpoint = i;
                else if (col.indexOf("icecast username") !== -1) colIndices.icecast_username = i;
                else if (col.indexOf("icecast password") !== -1) colIndices.icecast_password = i;
                else if (col.indexOf("icecast name") !== -1) colIndices.icecast_name = i;
                // Boondock API columns
                else if (col.indexOf("boondock api enabled") !== -1) colIndices.boondock_enabled = i;
                else if (col.indexOf("boondock api url") !== -1) colIndices.boondock_url = i;
                else if (col.indexOf("boondock api key") !== -1) colIndices.boondock_key = i;
                // Redis columns
                else if (col.indexOf("redis output enabled") !== -1) colIndices.redis_enabled = i;
                else if (col.indexOf("redis address") !== -1) colIndices.redis_address = i;
                else if (col.indexOf("redis port") !== -1) colIndices.redis_port = i;
                else if (col.indexOf("redis password") !== -1) colIndices.redis_password = i;
                else if (col.indexOf("redis database") !== -1) colIndices.redis_database = i;
            }
            
            if (colIndices.label === undefined || colIndices.freq === undefined) {
                showAlert("CSV must contain 'Label' and 'Frequency (MHz)' columns", "Import Error");
                return;
            }
            
            // Get existing channels to check for duplicates
            var existingFreqs = [];
            if (channelsData && channelsData.devices) {
                channelsData.devices.forEach(function(dev) {
                    if (dev.channels) {
                        dev.channels.forEach(function(ch) {
                            if (ch.freq) {
                                existingFreqs.push(parseFloat(ch.freq));
                            }
                        });
                    }
                });
            }
            
            // Helper to check if frequency is duplicate (within 0.001 MHz tolerance)
            var isDuplicate = function(freq) {
                for (var i = 0; i < existingFreqs.length; i++) {
                    if (Math.abs(existingFreqs[i] - freq) < 0.001) {
                        return true;
                    }
                }
                return false;
            };
            
            // Parse CSV rows (skip header)
            var channelsToAdd = [];
            var duplicateCount = 0;
            var invalidCount = 0;
            
            for (var i = 1; i < lines.length; i++) {
                var line = lines[i].trim();
                if (!line) continue;
                
                var fields = parseCSVLine(line);
                
                if (fields.length <= Math.max(colIndices.label, colIndices.freq)) {
                    invalidCount++;
                    continue; // Skip invalid rows
                }
                
                var label = fields[colIndices.label] || "";
                var freqStr = fields[colIndices.freq] || "";
                var freq = parseFloat(freqStr);
                
                if (!label || isNaN(freq) || freq <= 0) {
                    invalidCount++;
                    continue; // Skip invalid rows
                }
                
                // Check for duplicates
                if (isDuplicate(freq)) {
                    duplicateCount++;
                    continue;
                }
                
                // Build channel data with all parameters
                var channelData = {
                    label: label,
                    freq: freq,
                    modulation: (colIndices.modulation !== undefined && fields[colIndices.modulation]) ? fields[colIndices.modulation].toLowerCase() : "am",
                    enabled: (colIndices.enabled !== undefined) ? (fields[colIndices.enabled].toLowerCase() === "yes" || fields[colIndices.enabled] === "1") : true,
                    outputs: []
                };
                
                // Parse optional parameters
                if (colIndices.bandwidth !== undefined && fields[colIndices.bandwidth]) {
                    var bw = parseInt(fields[colIndices.bandwidth]);
                    if (!isNaN(bw) && bw > 0) channelData.bandwidth = bw;
                }
                if (colIndices.highpass !== undefined && fields[colIndices.highpass]) {
                    var hp = parseInt(fields[colIndices.highpass]);
                    if (!isNaN(hp) && hp > 0) channelData.highpass = hp;
                }
                if (colIndices.lowpass !== undefined && fields[colIndices.lowpass]) {
                    var lp = parseInt(fields[colIndices.lowpass]);
                    if (!isNaN(lp) && lp > 0) channelData.lowpass = lp;
                }
                if (colIndices.ampfactor !== undefined && fields[colIndices.ampfactor]) {
                    var amp = parseFloat(fields[colIndices.ampfactor]);
                    if (!isNaN(amp) && amp !== 1.0) channelData.ampfactor = amp;
                }
                if (colIndices.squelch_threshold !== undefined && fields[colIndices.squelch_threshold]) {
                    var sq = parseFloat(fields[colIndices.squelch_threshold]);
                    if (!isNaN(sq) && sq !== 0) channelData.squelch_threshold = sq;
                }
                if (colIndices.squelch_snr_threshold !== undefined && fields[colIndices.squelch_snr_threshold]) {
                    var sqSnr = parseFloat(fields[colIndices.squelch_snr_threshold]);
                    if (!isNaN(sqSnr) && sqSnr !== 0) channelData.squelch_snr_threshold = sqSnr;
                }
                if (colIndices.afc !== undefined && fields[colIndices.afc]) {
                    var afc = parseInt(fields[colIndices.afc]);
                    if (!isNaN(afc) && afc > 0) channelData.afc = afc;
                }
                if (colIndices.notch !== undefined && fields[colIndices.notch]) {
                    var notch = parseFloat(fields[colIndices.notch]);
                    if (!isNaN(notch) && notch > 0) channelData.notch = notch;
                }
                if (colIndices.notch_q !== undefined && fields[colIndices.notch_q]) {
                    var notchQ = parseFloat(fields[colIndices.notch_q]);
                    if (!isNaN(notchQ) && notchQ !== 10.0) channelData.notch_q = notchQ;
                }
                if (colIndices.ctcss !== undefined && fields[colIndices.ctcss]) {
                    var ctcss = parseFloat(fields[colIndices.ctcss]);
                    if (!isNaN(ctcss) && ctcss > 0) channelData.ctcss = ctcss;
                }
                
                // Parse file output
                if (colIndices.file_enabled !== undefined && fields[colIndices.file_enabled] && (fields[colIndices.file_enabled].toLowerCase() === "yes" || fields[colIndices.file_enabled] === "1")) {
                    var globalDir = document.getElementById("output-settings-global-recording-dir");
                    var globalDirValue = (globalDir && globalDir.value) ? globalDir.value : "recordings";
                    var fileDir = (colIndices.file_directory !== undefined && fields[colIndices.file_directory]) ? fields[colIndices.file_directory] : (globalDirValue + "/" + label);
                    
                    channelData.outputs.push({
                        type: "file",
                        enabled: true,
                        directory: fileDir,
                        filename_template: (colIndices.file_template !== undefined && fields[colIndices.file_template]) ? fields[colIndices.file_template] : label,
                        continuous: (colIndices.file_continuous !== undefined && fields[colIndices.file_continuous] && fields[colIndices.file_continuous].toLowerCase() === "yes"),
                        split_on_transmission: (colIndices.file_split !== undefined && fields[colIndices.file_split] && fields[colIndices.file_split].toLowerCase() === "yes"),
                        include_freq: (colIndices.file_include_freq !== undefined && fields[colIndices.file_include_freq] && fields[colIndices.file_include_freq].toLowerCase() === "yes"),
                        append: (colIndices.file_append !== undefined && fields[colIndices.file_append] && fields[colIndices.file_append].toLowerCase() !== "no"),
                        dated_subdirectories: (colIndices.file_dated_subdirs !== undefined && fields[colIndices.file_dated_subdirs] && fields[colIndices.file_dated_subdirs].toLowerCase() === "yes")
                    });
                }
                
                // Parse UDP output
                if (colIndices.udp_enabled !== undefined && fields[colIndices.udp_enabled] && (fields[colIndices.udp_enabled].toLowerCase() === "yes" || fields[colIndices.udp_enabled] === "1")) {
                    var udpOut = {
                        type: "udp_stream",
                        enabled: true,
                        dest_address: (colIndices.udp_address !== undefined && fields[colIndices.udp_address]) ? fields[colIndices.udp_address] : "127.0.0.1",
                        dest_port: (colIndices.udp_port !== undefined && fields[colIndices.udp_port]) ? parseInt(fields[colIndices.udp_port]) : 6001,
                        continuous: (colIndices.udp_continuous !== undefined && fields[colIndices.udp_continuous] && fields[colIndices.udp_continuous].toLowerCase() === "yes"),
                        udp_headers: (colIndices.udp_headers !== undefined && fields[colIndices.udp_headers] && fields[colIndices.udp_headers].toLowerCase() === "yes"),
                        udp_chunking: (colIndices.udp_chunking === undefined || fields[colIndices.udp_chunking].toLowerCase() !== "no")
                    };
                    channelData.outputs.push(udpOut);
                }
                
                // Parse Icecast output
                if (colIndices.icecast_enabled !== undefined && fields[colIndices.icecast_enabled] && (fields[colIndices.icecast_enabled].toLowerCase() === "yes" || fields[colIndices.icecast_enabled] === "1")) {
                    var icecastOut = {
                        type: "icecast",
                        enabled: true,
                        server: (colIndices.icecast_server !== undefined && fields[colIndices.icecast_server]) ? fields[colIndices.icecast_server] : "",
                        port: (colIndices.icecast_port !== undefined && fields[colIndices.icecast_port]) ? parseInt(fields[colIndices.icecast_port]) : 8000,
                        mountpoint: (colIndices.icecast_mountpoint !== undefined && fields[colIndices.icecast_mountpoint]) ? fields[colIndices.icecast_mountpoint] : "",
                        username: (colIndices.icecast_username !== undefined && fields[colIndices.icecast_username]) ? fields[colIndices.icecast_username] : "",
                        password: (colIndices.icecast_password !== undefined && fields[colIndices.icecast_password]) ? fields[colIndices.icecast_password] : "",
                        name: (colIndices.icecast_name !== undefined && fields[colIndices.icecast_name]) ? fields[colIndices.icecast_name] : ""
                    };
                    channelData.outputs.push(icecastOut);
                }
                
                // Parse Boondock API output
                if (colIndices.boondock_enabled !== undefined && fields[colIndices.boondock_enabled] && (fields[colIndices.boondock_enabled].toLowerCase() === "yes" || fields[colIndices.boondock_enabled] === "1")) {
                    channelData.outputs.push({
                        type: "boondock_api",
                        enabled: true,
                        api_url: (colIndices.boondock_url !== undefined && fields[colIndices.boondock_url]) ? fields[colIndices.boondock_url] : "",
                        api_key: (colIndices.boondock_key !== undefined && fields[colIndices.boondock_key]) ? fields[colIndices.boondock_key] : ""
                    });
                }
                
                // Parse Redis output
                if (colIndices.redis_enabled !== undefined && fields[colIndices.redis_enabled] && (fields[colIndices.redis_enabled].toLowerCase() === "yes" || fields[colIndices.redis_enabled] === "1")) {
                    channelData.outputs.push({
                        type: "redis",
                        enabled: true,
                        address: (colIndices.redis_address !== undefined && fields[colIndices.redis_address]) ? fields[colIndices.redis_address] : "127.0.0.1",
                        port: (colIndices.redis_port !== undefined && fields[colIndices.redis_port]) ? parseInt(fields[colIndices.redis_port]) : 6379,
                        password: (colIndices.redis_password !== undefined && fields[colIndices.redis_password]) ? fields[colIndices.redis_password] : "",
                        database: (colIndices.redis_database !== undefined && fields[colIndices.redis_database]) ? parseInt(fields[colIndices.redis_database]) : 0
                    });
                }
                
                // If no outputs specified, add default file output
                if (channelData.outputs.length === 0) {
                    var globalDir = document.getElementById("output-settings-global-recording-dir");
                    var globalDirValue = (globalDir && globalDir.value) ? globalDir.value : "recordings";
                    channelData.outputs.push({
                        type: "file",
                        enabled: true,
                        directory: globalDirValue + "/" + label,
                        filename_template: label,
                        continuous: true,
                        include_freq: true,
                        dated_subdirectories: true
                    });
                }
                
                channelsToAdd.push(channelData);
                existingFreqs.push(freq); // Add to existing list to prevent duplicates within the import
            }
            
            if (channelsToAdd.length === 0) {
                var msg = "No valid channels to import.";
                if (duplicateCount > 0) msg += "\n" + duplicateCount + " duplicate(s) skipped.";
                if (invalidCount > 0) msg += "\n" + invalidCount + " invalid row(s) skipped.";
                showAlert(msg, "Import Error");
                return;
            }
            
            // Confirm import
            var confirmMsg = "Import " + channelsToAdd.length + " channel(s)?";
            if (duplicateCount > 0) confirmMsg += "\n" + duplicateCount + " duplicate(s) will be skipped.";
            if (invalidCount > 0) confirmMsg += "\n" + invalidCount + " invalid row(s) will be skipped.";
            
            showConfirm(confirmMsg, "Import Channels").then(function(confirmed) {
                if (!confirmed) return;
                
                // Add channels via API
                var deviceIdx = 0; // Use first device
                var addedCount = 0;
                var errorCount = 0;
                var errorMessages = [];
                
                var addNextChannel = function(index) {
                    if (index >= channelsToAdd.length) {
                        // Show detailed summary
                        var summary = "Import Complete!\n\n";
                        summary += "✓ Successfully added: " + addedCount + " channel(s)\n";
                        if (duplicateCount > 0) summary += "⊘ Skipped duplicates: " + duplicateCount + " channel(s)\n";
                        if (invalidCount > 0) summary += "⊘ Skipped invalid rows: " + invalidCount + " row(s)\n";
                        if (errorCount > 0) {
                            summary += "✗ Errors: " + errorCount + " channel(s)\n";
                            summary += "\nError details:\n" + errorMessages.join("\n");
                        }
                        showAlert(summary, "Import Complete");
                        loadChannels(); // Reload to show new channels
                        return;
                    }
                    
                    var ch = channelsToAdd[index];
                    var channelPayload = {
                        device_index: deviceIdx,
                        label: ch.label,
                        freq: ch.freq,
                        modulation: ch.modulation,
                        enabled: ch.enabled !== undefined ? ch.enabled : true,
                        bandwidth: ch.bandwidth || null,
                        highpass: ch.highpass || null,
                        lowpass: ch.lowpass || null,
                        ampfactor: ch.ampfactor || null,
                        squelch_threshold: ch.squelch_threshold || null,
                        squelch_snr_threshold: ch.squelch_snr_threshold || null,
                        afc: ch.afc || null,
                        notch: ch.notch || null,
                        notch_q: ch.notch_q || null,
                        ctcss: ch.ctcss || null,
                        outputs: ch.outputs || []
                    };
                    
                    // Add a small delay between requests to avoid overwhelming the server
                    setTimeout(function() {
                        fetch("/api/channels", {
                            method: "POST",
                            headers: { "Content-Type": "application/json" },
                            body: JSON.stringify(channelPayload)
                        })
                        .then(function(r) { 
                            if (!r.ok) {
                                return r.text().then(function(text) {
                                    var errorText = text;
                                    try {
                                        var errorJson = JSON.parse(text);
                                        errorText = errorJson.message || errorJson.error || text;
                                    } catch(e) {
                                        // Not JSON, use text as-is
                                    }
                                    throw new Error("HTTP " + r.status + ": " + errorText);
                                });
                            }
                            return r.json(); 
                        })
                        .then(function(data) {
                            if (data.status === "success") {
                                addedCount++;
                            } else {
                                errorCount++;
                                var errorMsg = ch.label + " (" + ch.freq + " MHz): " + (data.error || data.message || "Unknown error");
                                errorMessages.push(errorMsg);
                                console.error("Error adding channel:", errorMsg);
                            }
                            addNextChannel(index + 1);
                        })
                        .catch(function(err) {
                            errorCount++;
                            var errorMsg = ch.label + " (" + ch.freq + " MHz): " + err.message;
                            errorMessages.push(errorMsg);
                            console.error("Error adding channel:", err);
                            addNextChannel(index + 1);
                        });
                    }, index * 100); // 100ms delay between requests
                };
                
                addNextChannel(0);
            });
            
        } catch (err) {
            console.error("CSV import error:", err);
            showAlert("Error importing CSV: " + err.message, "Import Error");
        }
    };
    
    reader.readAsText(file);
    
    // Reset file input so same file can be imported again
    event.target.value = "";
}

var frequencyBandResizeHandler = null;

function updateChannelFiltering() {
    currentChannelsPage = 1; // Reset to first page when filtering changes
    renderChannelsTable();
    
    // Set up resize handler for frequency band (only once)
    if (!frequencyBandResizeHandler) {
        frequencyBandResizeHandler = function() {
            var centerFreqInput = document.getElementById("channels-center-freq");
            if (centerFreqInput && centerFreqInput.value) {
                var centerFreqMhz = parseFloat(centerFreqInput.value) || 0;
                var centerFreqHz = centerFreqMhz * 1000000;
                
                // Get selected range (2.5 MHz or 10 MHz on each side)
                var rangeInput = document.querySelector('input[name="frequency-range"]:checked');
                var rangeMhz = rangeInput ? parseFloat(rangeInput.value) : 10; // Default to 10 MHz
                var bandwidthHz = rangeMhz * 1000000; // Convert to Hz
                var freqMin = centerFreqHz - bandwidthHz;
                var freqMax = centerFreqHz + bandwidthHz;
                
                drawFrequencyBand(centerFreqHz, freqMin, freqMax);
            }
        };
        window.addEventListener('resize', frequencyBandResizeHandler);
    }
}

function drawFrequencyBand(centerFreqHz, freqMin, freqMax) {
    var container = document.getElementById("frequency-band-visualization");
    if (!container || !channelsData) return;
    
    var canvas = document.getElementById("frequency-band-canvas");
    if (!canvas) return;
    
    var width = container.offsetWidth || 800;
    var height = 60;
    canvas.width = width;
    canvas.height = height;
    var ctx = canvas.getContext("2d");
    
    // Clear canvas
    ctx.fillStyle = "#0a0a0a";
    ctx.fillRect(0, 0, width, height);
    
    // Draw frequency range
    var freqRange = freqMax - freqMin;
    
    // Collect all channels - use current checkbox state for enabled status
    var allChannels = [];
    channelsData.devices.forEach(function(dev) {
        if (dev.channels && dev.channels.length > 0) {
            dev.channels.forEach(function(ch) {
                if (ch.freq) {
                    var chFreqHz = ch.freq * 1000000;
                    var isInRange = (chFreqHz >= freqMin && chFreqHz <= freqMax);
                    
                    // Check actual checkbox state (more reliable than ch.enabled)
                    var checkbox = document.querySelector("input.channel-enable-checkbox[data-device='" + dev.device + "'][data-channel='" + ch.channel_index + "']");
                    var isEnabled = checkbox ? checkbox.checked : (ch.enabled !== false);
                    
                    allChannels.push({
                        freq: chFreqHz,
                        label: ch.label || ("Ch " + ch.channel_index),
                        enabled: isEnabled,
                        inRange: isInRange
                    });
                }
            });
        }
    });
    
    // Draw center frequency line
    ctx.strokeStyle = "#4a9eff";
    ctx.lineWidth = 2;
    var centerX = width / 2;
    ctx.beginPath();
    ctx.moveTo(centerX, 0);
    ctx.lineTo(centerX, height);
    ctx.stroke();
    
    // Draw channel markers - only show enabled channels
    allChannels.forEach(function(ch) {
        // Only draw enabled channels
        if (!ch.enabled) return;
        
        var x = ((ch.freq - freqMin) / freqRange) * width;
        if (x >= 0 && x <= width) {
            ctx.fillStyle = ch.inRange ? "#4caf50" : "#666";
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x - 3, height);
            ctx.lineTo(x + 3, height);
            ctx.closePath();
            ctx.fill();
            
            // Draw label
            if (ch.inRange) {
                ctx.fillStyle = "#fff";
                ctx.font = "9px monospace";
                ctx.textAlign = "center";
                ctx.textBaseline = "top";
                var labelY = height - 15;
                ctx.fillText(ch.label, x, labelY);
            }
        }
    });
    
    // Update frequency labels
    document.getElementById("band-start-freq").textContent = (freqMin / 1000000).toFixed(4) + " MHz";
    document.getElementById("band-center-freq").textContent = (centerFreqHz / 1000000).toFixed(4) + " MHz";
    document.getElementById("band-end-freq").textContent = (freqMax / 1000000).toFixed(4) + " MHz";
}

function saveChannelsConfig() {
    if (!channelsData || !channelsData.devices) {
        showAlert("No channel data available", "Error");
        return;
    }
    
    // Collect enabled channels
    var enabledChannels = [];
    channelsData.devices.forEach(function(dev) {
        if (dev.channels && dev.channels.length > 0) {
            dev.channels.forEach(function(ch) {
                // Check if checkbox is checked using data attributes
                var checkbox = document.querySelector("input.channel-enable-checkbox[data-device='" + dev.device + "'][data-channel='" + ch.channel_index + "']");
                if (checkbox && checkbox.checked) {
                    enabledChannels.push({
                        device: dev.device,
                        channel: ch
                    });
                }
            });
        }
    });
    
    if (enabledChannels.length === 0) {
        showAlert("No channels are enabled. Please enable at least one channel.", "Save Configuration");
        return;
    }
    
    // Check if more than 8 channels are enabled
    if (enabledChannels.length > MAX_ACTIVE_CHANNELS) {
        showAlert("Only " + MAX_ACTIVE_CHANNELS + " channels can be active at a time. Please uncheck " + (enabledChannels.length - MAX_ACTIVE_CHANNELS) + " channel(s) before saving.", "Too Many Channels");
        return;
    }
    
    // Get center frequency
    var centerFreqInput = document.getElementById("channels-center-freq");
    var centerFreqMhz = parseFloat(centerFreqInput.value) || 0;
    var centerFreqHz = centerFreqMhz * 1000000;
    
    // Update channelsData with enabled status and center frequency
    var updatedData = JSON.parse(JSON.stringify(channelsData));
    
    // Get frequency range for filtering
    var rangeInput = document.querySelector('input[name="frequency-range"]:checked');
    var rangeMhz = rangeInput ? parseFloat(rangeInput.value) : 10;
    var bandwidthHz = rangeMhz * 1000000;
    var freqMin = centerFreqHz - bandwidthHz;
    var freqMax = centerFreqHz + bandwidthHz;
    
    updatedData.devices.forEach(function(dev) {
        // Update center frequency
        dev.centerfreq = centerFreqHz;
        
        if (dev.channels) {
            dev.channels.forEach(function(ch) {
                // Find checkbox using data attributes
                var checkbox = document.querySelector("input.channel-enable-checkbox[data-device='" + dev.device + "'][data-channel='" + ch.channel_index + "']");
                var isChecked = checkbox && checkbox.checked;
                
                // Check if channel is in range
                var channelFreqHz = ch.freq ? ch.freq * 1000000 : 0;
                var isInRange = (channelFreqHz >= freqMin && channelFreqHz <= freqMax);
                
                // Only enable if checked AND in range
                ch.enabled = isChecked && isInRange;
            });
        }
    });
    
    // Attach enabled channel list so backend can apply disable flags reliably
    updatedData.enabled_channels = enabledChannels.map(function(entry) {
        return {
            device: entry.device,
            channel_index: entry.channel.channel_index
        };
    });
    
    // Send updated config to server
    fetch("/api/channels/config", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(updatedData)
    })
    .then(function(r) {
        if (!r.ok) {
            return r.text().then(function(text) {
                throw new Error("HTTP " + r.status + ": " + text);
            });
        }
        return r.json();
    })
    .then(function(data) {
        if (data.status === "success") {
            showAlert("Configuration saved successfully! " + enabledChannels.length + " channel(s) enabled.", "Success");
            loadChannels(); // Reload to reflect changes
        } else {
            showAlert("Failed to save configuration: " + (data.error || data.message || "Unknown error"), "Error");
        }
    })
    .catch(function(err) {
        console.error("Save config error:", err);
        showAlert("Error saving configuration: " + err.message, "Error");
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
    showAlert("Device configuration saved. Click 'Start Capture' to apply the configuration.", "Success");
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
    showYesNo("Reset channel to default settings? This will clear all advanced features and keep only basic settings.", "Reset Channel").then(function(confirmed) {
        if (!confirmed) return;
        proceedWithReset();
    });
    return;
    
    function proceedWithReset() {
    
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
    updateChannelRecordingDirectory();
    document.getElementById("output-file-filename").value = "";
    document.getElementById("output-file-continuous").checked = false;
    document.getElementById("output-file-split").checked = false;
    document.getElementById("output-file-include-freq").checked = true;
    document.getElementById("output-file-dated-subdirs").checked = true;
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
    
    // Update output visibility based on enabled methods
    updateChannelOutputVisibility();
    
    // Load default values from output settings
    loadChannelOutputDefaults();
    }
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
    updateChannelRecordingDirectory();
    
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
            // Update directory when label is set
            updateChannelRecordingDirectory();
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
                        // Directory is auto-generated, but we'll update it based on current channel name
                        updateChannelRecordingDirectory();
                        document.getElementById("output-file-filename").value = out.filename_template || "";
                        document.getElementById("output-file-continuous").checked = out.continuous || false;
                        document.getElementById("output-file-split").checked = out.split_on_transmission || false;
                        document.getElementById("output-file-include-freq").checked = out.include_freq !== false; // Default true
                        document.getElementById("output-file-append").checked = out.append !== false; // Default true
                        document.getElementById("output-file-dated-subdirs").checked = out.dated_subdirectories !== false; // Default true
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
                        // Only load stream name - server settings come from Output Settings
                        document.getElementById("output-icecast-name").value = out.name || "";
                        toggleOutputSection("icecast");
                    } else if (out.type === "boondock_api") {
                        document.getElementById("output-boondock-api-enabled").checked = out.enabled;
                        document.getElementById("output-boondock-api-url").value = out.api_url || "";
                        document.getElementById("output-boondock-api-key").value = out.api_key || "";
                        toggleOutputSection("boondock-api");
                    } else if (out.type === "redis") {
                        document.getElementById("output-redis-enabled").checked = out.enabled;
                        document.getElementById("output-redis-address").value = out.address || "";
                        document.getElementById("output-redis-port").value = out.port || "";
                        document.getElementById("output-redis-password").value = out.password || "";
                        document.getElementById("output-redis-database").value = out.database !== undefined ? out.database : 0;
                        toggleOutputSection("redis");
                    }
                });
            }
        }
    }
    
    // Show General tab
    showChannelTab("general");
    
    // Update output visibility based on enabled methods
    updateChannelOutputVisibility();
    
    // Load default values from output settings if not already set
    loadChannelOutputDefaults();
    
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

var captureRunning = true;  // Track capture state

function updateCaptureButton() {
    var btn = document.getElementById("capture-control-btn");
    if (btn) {
        if (captureRunning) {
            btn.textContent = "Stop Capture";
            btn.style.background = "#dc3545";
        } else {
            btn.textContent = "Start Capture";
            btn.style.background = "#28a745";
        }
    }
}

function toggleCapture() {
    var action = captureRunning ? "stop" : "start";
    var btn = document.getElementById("capture-control-btn");
    
    if (captureRunning) {
        // Show stopping message
        showStoppingMessage();
    } else {
        // Show starting message when starting
        showStartingMessage();
    }
    
    btn.disabled = true;
    
    fetch("/api/capture/" + action, {
        method: "POST",
        headers: { "Content-Type": "application/json" }
    })
    .then(function(r) { 
        if (!r.ok) {
            return r.text().then(function(text) {
                throw new Error("HTTP " + r.status + ": " + text);
            });
        }
        return r.json(); 
    })
    .then(function(data) {
        if (data.status === "success") {
            captureRunning = !captureRunning;
            updateCaptureButton();
            
            if (!captureRunning) {
                // Show confirmation that capture is stopped
                hideStoppingMessage();
                alert("Capture process stopped successfully. You can now modify the configuration.");
            } else {
                // Show confirmation that capture started
                hideStartingMessage();
                showAlert("Capture process started successfully with current settings.", "Capture Started");
            }
        } else {
            showAlert("Error: " + (data.error || data.message || "Failed to " + action + " capture"), "Error");
            if (!captureRunning) {
                hideStartingMessage();
            } else {
                hideStoppingMessage();
            }
        }
        btn.disabled = false;
    })
    .catch(function(err) {
        console.error("Toggle capture error:", err);
        showAlert("Error " + (captureRunning ? "stopping" : "starting") + " capture: " + err.message, "Error");
        if (!captureRunning) {
            hideStartingMessage();
        } else {
            hideStoppingMessage();
        }
        btn.disabled = false;
    });
}

function showStartingMessage() {
    // Create or show starting overlay
    var overlay = document.getElementById("starting-overlay");
    if (!overlay) {
        overlay = document.createElement("div");
        overlay.id = "starting-overlay";
        overlay.style.cssText = "position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 10000; display: flex; align-items: center; justify-content: center; flex-direction: column; color: white; font-size: 24px;";
        overlay.innerHTML = '<div style="text-align: center;"><div style="margin-bottom: 20px; font-size: 32px;">▶</div><div>Saving configuration and starting capture...</div><div style="margin-top: 10px; font-size: 14px; color: #aaa;">Please wait</div></div>';
        document.body.appendChild(overlay);
    } else {
        overlay.style.display = "flex";
    }
}

function hideStartingMessage() {
    var overlay = document.getElementById("starting-overlay");
    if (overlay) {
        overlay.style.display = "none";
    }
}

function showStoppingMessage() {
    // Create or show stopping overlay
    var overlay = document.getElementById("stopping-overlay");
    if (!overlay) {
        overlay = document.createElement("div");
        overlay.id = "stopping-overlay";
        overlay.style.cssText = "position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 10000; display: flex; align-items: center; justify-content: center; flex-direction: column; color: white; font-size: 24px;";
        overlay.innerHTML = '<div style="text-align: center;"><div style="margin-bottom: 20px; font-size: 32px;">⏸</div><div>Stopping capture process...</div><div style="margin-top: 10px; font-size: 14px; color: #aaa;">Please wait</div></div>';
        document.body.appendChild(overlay);
    } else {
        overlay.style.display = "flex";
    }
}

function hideStoppingMessage() {
    var overlay = document.getElementById("stopping-overlay");
    if (overlay) {
        overlay.style.display = "none";
    }
}

function checkCaptureStatus() {
    fetch("/api/capture/status")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.status === "success") {
                captureRunning = data.capture_enabled === 1;
                updateCaptureButton();
            }
        })
        .catch(function(err) {
            console.error("Error checking capture status:", err);
        });
}

// applyChanges function removed - changes are now applied automatically when starting capture

function saveChannel(event) {
    console.log("saveChannel called", event);
    
    if (event) {
    event.preventDefault();
        event.stopPropagation();
    }
    
    var form = event ? event.target : document.getElementById("channel-form");
    if (!form) {
        console.error("Channel form not found");
        showAlert("Error: Channel form not found", "Error");
        return;
    }
    
    // Validate required fields
    var labelInput = form.querySelector("#channel-label");
    var freqInput = form.querySelector("#channel-freq");
    
    if (!labelInput || !freqInput) {
        console.error("Required form fields not found");
        showAlert("Error: Form fields not found", "Error");
        return;
    }
    
    var label = labelInput.value.trim();
    var freq = freqInput.value.trim();
    
    if (!label || !freq) {
        showAlert("Please fill in all required fields (Label and Frequency)", "Validation Error");
        if (!label) labelInput.focus();
        else if (!freq) freqInput.focus();
        return;
    }
    
    if (isNaN(parseFloat(freq)) || parseFloat(freq) <= 0) {
        showAlert("Please enter a valid frequency (must be a positive number)", "Validation Error");
        freqInput.focus();
        return;
    }
    
    console.log("Form validation passed, building channel data...");
    var formData = new FormData(form);
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
        // Use global recording directory + channel name
        var globalDir = document.getElementById("output-settings-global-recording-dir");
        var globalDirValue = (globalDir && globalDir.value) ? globalDir.value : "recordings";
        var channelName = formData.get("label") || "Channel_Name";
        var fullDirectory = globalDirValue + "/" + channelName;
        
        channelData.outputs.push({
            type: "file",
            enabled: true,
            directory: fullDirectory,
            filename_template: formData.get("output_file_filename"),
            continuous: formData.get("output_file_continuous") === "on",
            split_on_transmission: formData.get("output_file_split") === "on",
            include_freq: formData.get("output_file_include_freq") === "on",
            append: formData.get("output_file_append") === "on",
            dated_subdirectories: formData.get("output_file_dated_subdirs") === "on"
        });
    }
    
    // Add UDP output
    if (formData.get("output_udp_enabled") === "on") {
        var udpAddress = formData.get("output_udp_address");
        // Use default from output settings if not specified
        if (!udpAddress) {
            var defaultUdpAddress = document.getElementById("output-settings-udp-default-address");
            if (defaultUdpAddress && defaultUdpAddress.value) {
                udpAddress = defaultUdpAddress.value;
            }
        }
        channelData.outputs.push({
            type: "udp_stream",
            enabled: true,
            dest_address: udpAddress || "127.0.0.1",
            dest_port: parseInt(formData.get("output_udp_port")) || 6001,
            continuous: formData.get("output_udp_continuous") === "on",
            udp_headers: formData.get("output_udp_headers") === "on",
            udp_chunking: formData.get("output_udp_chunking") !== "off" // Default true if not explicitly off
        });
    }
    
    // Add Icecast output
    if (formData.get("output_icecast_enabled") === "on") {
        // Use defaults from output settings
        var icecastServer = document.getElementById("output-settings-icecast-server").value || "";
        var icecastPort = parseInt(document.getElementById("output-settings-icecast-port").value) || 8000;
        var icecastMountpoint = document.getElementById("output-settings-icecast-mountpoint").value || "";
        var icecastUsername = document.getElementById("output-settings-icecast-username").value || "";
        var icecastPassword = document.getElementById("output-settings-icecast-password").value || "";
        var icecastName = formData.get("output_icecast_name") || "";
        
        channelData.outputs.push({
            type: "icecast",
            enabled: true,
            server: icecastServer,
            port: icecastPort,
            mountpoint: icecastMountpoint,
            username: icecastUsername,
            password: icecastPassword,
            name: icecastName
        });
    }
    
    // Add Boondock API output
    if (formData.get("output_boondock_api_enabled") === "on") {
        var apiUrl = formData.get("output_boondock_api_url");
        var apiKey = formData.get("output_boondock_api_key");
        // Use defaults from output settings if not specified
        if (!apiUrl) {
            var defaultApiUrl = document.getElementById("output-settings-boondock-api-url");
            if (defaultApiUrl && defaultApiUrl.value) {
                apiUrl = defaultApiUrl.value;
            }
        }
        if (!apiKey) {
            var defaultApiKey = document.getElementById("output-settings-boondock-api-key");
            if (defaultApiKey && defaultApiKey.value) {
                apiKey = defaultApiKey.value;
            }
        }
        channelData.outputs.push({
            type: "boondock_api",
            enabled: true,
            api_url: apiUrl || "",
            api_key: apiKey || ""
        });
    }
    
    // Add Redis output
    if (formData.get("output_redis_enabled") === "on") {
        var redisAddress = formData.get("output_redis_address");
        var redisPort = formData.get("output_redis_port");
        var redisPassword = formData.get("output_redis_password");
        var redisDatabase = formData.get("output_redis_database");
        // Use defaults from output settings if not specified
        if (!redisAddress) {
            var defaultRedisAddress = document.getElementById("output-settings-redis-address");
            if (defaultRedisAddress && defaultRedisAddress.value) {
                redisAddress = defaultRedisAddress.value;
            }
        }
        if (!redisPort) {
            var defaultRedisPort = document.getElementById("output-settings-redis-port");
            if (defaultRedisPort && defaultRedisPort.value) {
                redisPort = defaultRedisPort.value;
            }
        }
        if (!redisPassword) {
            var defaultRedisPassword = document.getElementById("output-settings-redis-password");
            if (defaultRedisPassword && defaultRedisPassword.value) {
                redisPassword = defaultRedisPassword.value;
            }
        }
        if (!redisDatabase && redisDatabase !== "0") {
            var defaultRedisDatabase = document.getElementById("output-settings-redis-database");
            if (defaultRedisDatabase && defaultRedisDatabase.value) {
                redisDatabase = defaultRedisDatabase.value;
            }
        }
        channelData.outputs.push({
            type: "redis",
            enabled: true,
            address: redisAddress || "127.0.0.1",
            port: parseInt(redisPort) || 6379,
            password: redisPassword || "",
            database: parseInt(redisDatabase) || 0
        });
    }
    
    var url = "/api/channels";
    var method = "POST";
    if (formData.get("edit_mode") === "true") {
        url = "/api/channels/" + channelData.device_index + "/" + channelData.channel_index;
        method = "PUT";
    }
    
    console.log("Sending request to:", url, "Method:", method);
    console.log("Channel data:", JSON.stringify(channelData, null, 2));
    
    fetch(url, {
        method: method,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(channelData)
    })
    .then(function(r) {
        console.log("Response status:", r.status);
        if (!r.ok) {
            return r.text().then(function(text) {
                throw new Error("HTTP " + r.status + ": " + text);
            });
        }
        return r.json();
    })
    .then(function(data) {
        console.log("Response data:", data);
        if (data.status === "success") {
            markChange("channels");
            showAlert("Channel saved. Click 'Start Capture' to apply the configuration.", "Success");
            closeChannelModal();
            loadChannels();
        } else {
            showAlert("Error: " + (data.error || data.message || "Failed to save channel"), "Error");
        }
    })
    .catch(function(err) {
        console.error("Save channel error:", err);
        showAlert("Error saving channel: " + err.message + ". Please check the console for details.", "Error");
    });
}

function toggleChannel(deviceIdx, channelIdx, enabled) {
    // Find the channel in allChannelsList to check if it's in range
    var channel = null;
    if (allChannelsList) {
        for (var i = 0; i < allChannelsList.length; i++) {
            if (allChannelsList[i].device === deviceIdx && allChannelsList[i].channel_index === channelIdx) {
                channel = allChannelsList[i];
                break;
            }
        }
    }
    
    // Check if channel is in range (only for enabling)
    if (enabled) {
        if (channel && !channel.isInRange) {
            showAlert("This channel is outside the frequency range and cannot be enabled.", "Out of Range");
            // Uncheck the checkbox
            var checkbox = document.querySelector("input.channel-enable-checkbox[data-device='" + deviceIdx + "'][data-channel='" + channelIdx + "']");
            if (checkbox) {
                checkbox.checked = false;
            }
            return;
        }
        
        // Count currently enabled AND in-range channels
        var enabledCount = 0;
        var checkboxes = document.querySelectorAll("input.channel-enable-checkbox:checked");
        checkboxes.forEach(function(cb) {
            var cbDevice = parseInt(cb.getAttribute("data-device"));
            var cbChannel = parseInt(cb.getAttribute("data-channel"));
            
            // Find this channel in allChannelsList to check if it's in range
            var ch = null;
            if (allChannelsList) {
                for (var j = 0; j < allChannelsList.length; j++) {
                    if (allChannelsList[j].device === cbDevice && allChannelsList[j].channel_index === cbChannel) {
                        ch = allChannelsList[j];
                        break;
                    }
                }
            }
            
            // Only count if in range
            if (ch && ch.isInRange) {
                enabledCount++;
            }
        });
        
        // Check if this checkbox is already checked (to avoid counting it twice)
        var currentCheckbox = document.querySelector("input.channel-enable-checkbox[data-device='" + deviceIdx + "'][data-channel='" + channelIdx + "']");
        if (currentCheckbox && currentCheckbox.checked && channel && channel.isInRange) {
            enabledCount--; // Don't count the one being unchecked
        }
        
        if (enabledCount >= MAX_ACTIVE_CHANNELS) {
            showAlert("Maximum " + MAX_ACTIVE_CHANNELS + " channels can be active at a time. Please uncheck another channel first.", "Channel Limit");
            // Uncheck the checkbox
            if (currentCheckbox) {
                currentCheckbox.checked = false;
            }
            return;
        }
    }
    
    // Update the channel data immediately for UI responsiveness
    if (channelsData && channelsData.devices) {
        channelsData.devices.forEach(function(dev) {
            if (dev.device === deviceIdx && dev.channels) {
                dev.channels.forEach(function(ch) {
                    if (ch.channel_index === channelIdx) {
                        ch.enabled = enabled;
                    }
                });
            }
        });
    }
    
    // Re-render table to show active channels first
    renderChannelsTable();
    
    // Update frequency band visualization
    var centerFreqInput = document.getElementById("channels-center-freq");
    if (centerFreqInput && centerFreqInput.value) {
        var centerFreqMhz = parseFloat(centerFreqInput.value) || 0;
        var centerFreqHz = centerFreqMhz * 1000000;
        var rangeInput = document.querySelector('input[name="frequency-range"]:checked');
        var rangeMhz = rangeInput ? parseFloat(rangeInput.value) : 10;
        var bandwidthHz = rangeMhz * 1000000;
        var freqMin = centerFreqHz - bandwidthHz;
        var freqMax = centerFreqHz + bandwidthHz;
        drawFrequencyBand(centerFreqHz, freqMin, freqMax);
    }
    
    // Note: We don't call the API here - changes are only saved when "Save Config" is clicked
    markChange("channels");
}

function deleteChannel(deviceIdx, channelIdx) {
    showYesNo("Are you sure you want to delete this channel?", "Delete Channel").then(function(confirmed) {
        if (!confirmed) return;
        proceedWithDelete();
    });
    return;
    
    function proceedWithDelete() {
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
}

function updateChannelRecordingDirectory() {
    var globalDir = document.getElementById("output-settings-global-recording-dir");
    var channelLabel = document.getElementById("channel-label");
    var directoryInput = document.getElementById("output-file-directory");
    var directoryPreview = document.getElementById("output-file-directory-preview");
    
    if (globalDir && channelLabel && directoryInput && directoryPreview) {
        var globalDirValue = globalDir.value || "recordings";
        var channelName = channelLabel.value || "Channel_Name";
        var fullPath = globalDirValue + "/" + channelName;
        
        directoryInput.value = fullPath;
        directoryPreview.textContent = fullPath;
    }
}

function toggleOutputMethod(method) {
    var checkbox = document.getElementById("output-method-" + method + "-enabled");
    var section = document.getElementById("output-method-" + method + "-section");
    if (section) {
        section.style.display = checkbox.checked ? "block" : "none";
    }
    // Update channel output visibility
    updateChannelOutputVisibility();
}

function toggleUdpServerPorts() {
    var checkbox = document.getElementById("output-method-udp-server-enabled");
    var section = document.getElementById("output-method-udp-server-section");
    if (section) {
        section.style.display = checkbox.checked ? "block" : "none";
    }
}

function updateChannelOutputVisibility() {
    // Get enabled output methods from output settings
    var fileEnabled = true;  // Default enabled
    var udpEnabled = false;
    var udpServerEnabled = false;
    var boondockApiEnabled = false;
    var redisEnabled = false;
    var icecastEnabled = false;
    
    var fileCheckbox = document.getElementById("output-method-file-enabled");
    var udpCheckbox = document.getElementById("output-method-udp-enabled");
    var udpServerCheckbox = document.getElementById("output-method-udp-server-enabled");
    var boondockApiCheckbox = document.getElementById("output-method-boondock-api-enabled");
    var redisCheckbox = document.getElementById("output-method-redis-enabled");
    var icecastCheckbox = document.getElementById("output-method-icecast-enabled");
    
    if (fileCheckbox) fileEnabled = fileCheckbox.checked;
    if (udpCheckbox) udpEnabled = udpCheckbox.checked;
    if (udpServerCheckbox) udpServerEnabled = udpServerCheckbox.checked;
    if (boondockApiCheckbox) boondockApiEnabled = boondockApiCheckbox.checked;
    if (redisCheckbox) redisEnabled = redisCheckbox.checked;
    if (icecastCheckbox) icecastEnabled = icecastCheckbox.checked;
    
    // Show/hide output sections in channel modal
    var fileSection = document.getElementById("output-file-enabled");
    if (fileSection) {
        var fileContainer = fileSection.closest("div[style*='background: #252525']");
        if (fileContainer) {
            fileContainer.style.display = fileEnabled ? "block" : "none";
        }
    }
    
    var udpSection = document.getElementById("output-udp-enabled");
    if (udpSection) {
        var udpContainer = udpSection.closest("div[style*='background: #252525']");
        if (udpContainer) {
            udpContainer.style.display = (udpEnabled || udpServerEnabled) ? "block" : "none";
        }
    }
    
    // Show/hide Boondock API section
    var boondockApiContainer = document.getElementById("output-boondock-api-container");
    if (boondockApiContainer) {
        boondockApiContainer.style.display = boondockApiEnabled ? "block" : "none";
    }
    
    // Show/hide Redis section
    var redisContainer = document.getElementById("output-redis-container");
    if (redisContainer) {
        redisContainer.style.display = redisEnabled ? "block" : "none";
    }
    
    // Show/hide Icecast section
    var icecastContainer = document.getElementById("output-icecast-container");
    if (icecastContainer) {
        icecastContainer.style.display = icecastEnabled ? "block" : "none";
    }
}

function loadChannelOutputDefaults() {
    // Load default UDP address if UDP is enabled and address is empty
    var udpAddress = document.getElementById("output-udp-address");
    var defaultUdpAddress = document.getElementById("output-settings-udp-default-address");
    if (udpAddress && defaultUdpAddress && !udpAddress.value && defaultUdpAddress.value) {
        udpAddress.value = defaultUdpAddress.value;
    }
    
    // TODO: Load default port from UDP server port range if UDP server is enabled
    // This would require tracking which ports are in use and assigning the next available one
    
    // Load defaults for boondock-api if not specified
    var boondockApiUrl = document.getElementById("output-boondock-api-url");
    var defaultBoondockApiUrl = document.getElementById("output-settings-boondock-api-url");
    if (boondockApiUrl && defaultBoondockApiUrl && !boondockApiUrl.value && defaultBoondockApiUrl.value) {
        boondockApiUrl.value = defaultBoondockApiUrl.value;
    }
    
    var boondockApiKey = document.getElementById("output-boondock-api-key");
    var defaultBoondockApiKey = document.getElementById("output-settings-boondock-api-key");
    if (boondockApiKey && defaultBoondockApiKey && !boondockApiKey.value && defaultBoondockApiKey.value) {
        boondockApiKey.value = defaultBoondockApiKey.value;
    }
    
    // Load defaults for redis if not specified
    var redisAddress = document.getElementById("output-redis-address");
    var defaultRedisAddress = document.getElementById("output-settings-redis-address");
    if (redisAddress && defaultRedisAddress && !redisAddress.value && defaultRedisAddress.value) {
        redisAddress.value = defaultRedisAddress.value;
    }
    
    var redisPort = document.getElementById("output-redis-port");
    var defaultRedisPort = document.getElementById("output-settings-redis-port");
    if (redisPort && defaultRedisPort && !redisPort.value && defaultRedisPort.value) {
        redisPort.value = defaultRedisPort.value;
    }
    
    var redisPassword = document.getElementById("output-redis-password");
    var defaultRedisPassword = document.getElementById("output-settings-redis-password");
    if (redisPassword && defaultRedisPassword && !redisPassword.value && defaultRedisPassword.value) {
        redisPassword.value = defaultRedisPassword.value;
    }
    
    var redisDatabase = document.getElementById("output-redis-database");
    var defaultRedisDatabase = document.getElementById("output-settings-redis-database");
    if (redisDatabase && defaultRedisDatabase && !redisDatabase.value && defaultRedisDatabase.value) {
        redisDatabase.value = defaultRedisDatabase.value;
    }
}

function loadOutputSettings() {
    fetch("/api/outputs/settings")
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.file_chunk_duration_minutes) {
                document.getElementById("file-chunk-duration").value = data.file_chunk_duration_minutes;
                document.getElementById("chunk-duration-value").textContent = data.file_chunk_duration_minutes;
            }
            
            // Load output method settings
            if (data.output_methods) {
                if (data.output_methods.file !== undefined) {
                    document.getElementById("output-method-file-enabled").checked = data.output_methods.file.enabled || false;
                    if (data.output_methods.file.global_recording_directory) {
                        document.getElementById("output-settings-global-recording-dir").value = data.output_methods.file.global_recording_directory;
                    }
                    toggleOutputMethod("file");
                    // Update channel directory if modal is open
                    updateChannelRecordingDirectory();
                }
                if (data.output_methods.udp !== undefined) {
                    document.getElementById("output-method-udp-enabled").checked = data.output_methods.udp.enabled || false;
                    if (data.output_methods.udp.default_address) {
                        document.getElementById("output-settings-udp-default-address").value = data.output_methods.udp.default_address;
                    }
                    toggleOutputMethod("udp");
                }
                if (data.output_methods.udp_server !== undefined) {
                    document.getElementById("output-method-udp-server-enabled").checked = data.output_methods.udp_server.enabled || false;
                    if (data.output_methods.udp_server.port_start) {
                        document.getElementById("output-udp-port-start").value = data.output_methods.udp_server.port_start;
                    }
                    if (data.output_methods.udp_server.port_end) {
                        document.getElementById("output-udp-port-end").value = data.output_methods.udp_server.port_end;
                    }
                    toggleOutputMethod("udp-server");
                }
                if (data.output_methods.boondock_api !== undefined) {
                    document.getElementById("output-method-boondock-api-enabled").checked = data.output_methods.boondock_api.enabled || false;
                    if (data.output_methods.boondock_api.api_url) {
                        document.getElementById("output-settings-boondock-api-url").value = data.output_methods.boondock_api.api_url;
                    }
                    if (data.output_methods.boondock_api.api_key) {
                        document.getElementById("output-settings-boondock-api-key").value = data.output_methods.boondock_api.api_key;
                    }
                    toggleOutputMethod("boondock-api");
                }
                if (data.output_methods.redis !== undefined) {
                    document.getElementById("output-method-redis-enabled").checked = data.output_methods.redis.enabled || false;
                    if (data.output_methods.redis.address) {
                        document.getElementById("output-settings-redis-address").value = data.output_methods.redis.address;
                    }
                    if (data.output_methods.redis.port) {
                        document.getElementById("output-settings-redis-port").value = data.output_methods.redis.port;
                    }
                    if (data.output_methods.redis.password) {
                        document.getElementById("output-settings-redis-password").value = data.output_methods.redis.password;
                    }
                    if (data.output_methods.redis.database !== undefined) {
                        document.getElementById("output-settings-redis-database").value = data.output_methods.redis.database;
                    }
                    toggleOutputMethod("redis");
                }
                if (data.output_methods.icecast !== undefined) {
                    document.getElementById("output-method-icecast-enabled").checked = data.output_methods.icecast.enabled || false;
                    if (data.output_methods.icecast.server) {
                        document.getElementById("output-settings-icecast-server").value = data.output_methods.icecast.server;
                    }
                    if (data.output_methods.icecast.port) {
                        document.getElementById("output-settings-icecast-port").value = data.output_methods.icecast.port;
                    }
                    if (data.output_methods.icecast.mountpoint) {
                        document.getElementById("output-settings-icecast-mountpoint").value = data.output_methods.icecast.mountpoint;
                    }
                    if (data.output_methods.icecast.username) {
                        document.getElementById("output-settings-icecast-username").value = data.output_methods.icecast.username;
                    }
                    if (data.output_methods.icecast.password) {
                        document.getElementById("output-settings-icecast-password").value = data.output_methods.icecast.password;
                    }
                    toggleOutputMethod("icecast");
                }
            }
            
            // Update channel output visibility after loading
            updateChannelOutputVisibility();
        })
        .catch(function(err) { console.error("Load output settings error:", err); });
}

function saveOutputSettings() {
    var chunkDuration = parseInt(document.getElementById("file-chunk-duration").value);
    
    var outputSettings = {
        file_chunk_duration_minutes: chunkDuration,
        output_methods: {
            file: {
                enabled: document.getElementById("output-method-file-enabled").checked,
                global_recording_directory: document.getElementById("output-settings-global-recording-dir").value || "recordings"
            },
            udp: {
                enabled: document.getElementById("output-method-udp-enabled").checked,
                default_address: document.getElementById("output-settings-udp-default-address").value || "127.0.0.1"
            },
            udp_server: {
                enabled: document.getElementById("output-method-udp-server-enabled").checked,
                port_start: parseInt(document.getElementById("output-udp-port-start").value) || 6001,
                port_end: parseInt(document.getElementById("output-udp-port-end").value) || 6100
            },
            boondock_api: {
                enabled: document.getElementById("output-method-boondock-api-enabled").checked,
                api_url: document.getElementById("output-settings-boondock-api-url").value || "",
                api_key: document.getElementById("output-settings-boondock-api-key").value || ""
            },
            redis: {
                enabled: document.getElementById("output-method-redis-enabled").checked,
                address: document.getElementById("output-settings-redis-address").value || "127.0.0.1",
                port: parseInt(document.getElementById("output-settings-redis-port").value) || 6379,
                password: document.getElementById("output-settings-redis-password").value || "",
                database: parseInt(document.getElementById("output-settings-redis-database").value) || 0
            },
            icecast: {
                enabled: document.getElementById("output-method-icecast-enabled").checked,
                server: document.getElementById("output-settings-icecast-server").value || "",
                port: parseInt(document.getElementById("output-settings-icecast-port").value) || 8000,
                mountpoint: document.getElementById("output-settings-icecast-mountpoint").value || "",
                username: document.getElementById("output-settings-icecast-username").value || "",
                password: document.getElementById("output-settings-icecast-password").value || ""
            }
        }
    };
    
    fetch("/api/outputs/settings", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(outputSettings)
    })
    .then(function(r) { return r.json(); })
    .then(function(data) {
        if (data.status === "success") {
            markChange("outputs");
            alert("Output settings saved. Click 'Start Capture' to apply the configuration.");
        } else {
            showAlert("Error: " + (data.error || "Failed to save output settings"), "Error");
        }
    })
    .catch(function(err) {
        console.error("Save output settings error:", err);
        showAlert("Error saving output settings. Please check the console for details.", "Error");
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
        showAlert("Please enter a configuration file path", "Validation Error");
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
                showAlert("Configuration path saved. Click 'Start Capture' to apply.", "Success");
            } else {
                showAlert("Failed to save configuration path: " + (data.error || "Unknown error"), "Error");
            }
        })
        .catch(function(err) { 
            console.error("Save config path error:", err);
            showAlert("Error saving configuration path", "Error");
        });
}

function downloadConfig() {
    window.location.href = "/api/config/download";
}

function uploadConfig() {
    var fileInput = document.getElementById("config-upload");
    var file = fileInput.files[0];
    if (!file) {
        showAlert("Please select a configuration file to upload", "Validation Error");
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
                    statusDiv.textContent = "Configuration uploaded successfully! Click 'Start Capture' to apply.";
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

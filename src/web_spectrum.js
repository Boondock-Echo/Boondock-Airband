// Spectrum Analyzer
var spectrumDevice = -1;
var spectrumUpdateInterval = null;
var spectrumCanvas = null;
var waterfallCanvas = null;
var spectrumCtx = null;
var waterfallCtx = null;
var waterfallData = [];  // Array of spectrum lines for waterfall
var maxWaterfallLines = 200;  // Maximum number of lines in waterfall
var channelsData = null;  // Store channels data for markers

// Control parameters
var spectrumGain = -40;  // dB gain adjustment
var spectrumVerticalScale = 70;  // dB range (e.g., 70 = -95 to -25)
var spectrumVerticalOffset = 35;  // dB vertical offset
var spectrumAveraging = 10;  // Number of samples to average
var spectrumAveragingBuffer = [];  // Buffer for averaging
var waterfallIntensity = 50;  // Intensity percentage (10-100)
var waterfallSpeed = 1;  // Speed multiplier (1-10)
var waterfallSpeedCounter = 0;  // Counter for speed control
var currentSpectrumData = null;  // Store current data for redraw

// Zoom and pan parameters
var spectrumZoom = 1.0;  // Zoom level (1.0 = no zoom, >1.0 = zoomed in)
var spectrumPan = 0.0;  // Pan offset in frequency (Hz)
var isPanning = false;  // Whether user is currently panning
var panStartX = 0;  // Starting X position for pan
var panStartOffset = 0;  // Starting pan offset when drag begins
var mouseX = -1;  // Current mouse X position (-1 = not over canvas)
var mouseFreq = 0;  // Frequency at mouse position

function initSpectrum() {
    spectrumCanvas = document.getElementById("spectrum-canvas");
    waterfallCanvas = document.getElementById("waterfall-canvas");
    if (!spectrumCanvas || !waterfallCanvas) return;
    
    spectrumCtx = spectrumCanvas.getContext("2d");
    waterfallCtx = waterfallCanvas.getContext("2d", { willReadFrequently: true });
    
    // Set canvas size based on full browser width
    function updateCanvasSize() {
        var width = window.innerWidth;
        spectrumCanvas.width = width;
        waterfallCanvas.width = width;
        // Fixed heights: spectrum = 200px, waterfall = 300px
        spectrumCanvas.height = 200;
        spectrumCanvas.style.height = "200px";
        waterfallCanvas.height = 300;
        waterfallCanvas.style.height = "300px";
        maxWaterfallLines = Math.floor(300 / 2);
        
        // Redraw if we have data
        if (currentSpectrumData) {
            drawSpectrum(currentSpectrumData);
            if (waterfallData.length > 0) {
                redrawWaterfall();
            }
        }
    }
    
    // Set initial size
    updateCanvasSize();
    
    // Update on window resize
    window.addEventListener('resize', updateCanvasSize);
    
    // Load device list
    loadSpectrumDevices();
    
    // Load channels data for markers
    loadChannelsForSpectrum();
    
    // Setup device selector
    document.getElementById("spectrum-device-select").addEventListener("change", function() {
        var val = this.value;
        spectrumDevice = val === "" ? -1 : parseInt(val);
        // Reload channels when device changes
        loadChannelsForSpectrum();
        if (spectrumDevice >= 0) {
            startSpectrumUpdate();
        } else {
            stopSpectrumUpdate();
        }
    });
    
    // Setup auto-update checkbox
    document.getElementById("spectrum-auto-update").addEventListener("change", function() {
        if (this.checked && spectrumDevice >= 0) {
            startSpectrumUpdate();
        } else {
            stopSpectrumUpdate();
        }
    });
    
    // Setup zoom and pan controls
    setupSpectrumZoomPan();
    
    // Initialize control values
    updateSpectrumGain(document.getElementById("spectrum-gain").value);
    updateSpectrumVerticalScale(document.getElementById("spectrum-vertical-scale").value);
    updateSpectrumVerticalOffset(document.getElementById("spectrum-vertical-offset").value);
    updateSpectrumAveraging(document.getElementById("spectrum-averaging").value);
    updateWaterfallIntensity(document.getElementById("waterfall-intensity").value);
    updateWaterfallSpeed(document.getElementById("waterfall-speed").value);
    
    // Start if page is visible
    if (document.getElementById("page-spectrum").classList.contains("hidden") === false) {
        if (spectrumDevice >= 0) {
            startSpectrumUpdate();
        }
    }
}

function loadSpectrumDevices() {
    var select = document.getElementById("spectrum-device-select");
    if (!select) {
        console.error("spectrum-device-select element not found");
        return;
    }
    
    select.innerHTML = "<option value=\"\">Loading...</option>";
    
    fetch("/api/spectrum")
        .then(function(r) {
            if (!r.ok) {
                throw new Error("HTTP error! status: " + r.status);
            }
            return r.json();
        })
        .then(function(data) {
            select.innerHTML = "<option value=\"\">Select Device...</option>";
            if (data.devices && data.devices.length > 0) {
                data.devices.forEach(function(dev) {
                    var opt = document.createElement("option");
                    opt.value = dev.device;
                    opt.textContent = "Device " + dev.device + " (" + (dev.center_freq / 1000000).toFixed(3) + " MHz, " + (dev.sample_rate / 1000).toFixed(0) + " kHz)";
                    select.appendChild(opt);
                });
                // Auto-select if only one device
                if (data.devices.length === 1) {
                    select.value = data.devices[0].device;
                    spectrumDevice = parseInt(data.devices[0].device);
                    if (document.getElementById("spectrum-auto-update") && document.getElementById("spectrum-auto-update").checked) {
                        startSpectrumUpdate();
                    }
                }
            } else {
                select.innerHTML = "<option value=\"\">No devices available</option>";
            }
        })
        .catch(function(err) {
            console.error("Error loading spectrum devices:", err);
            select.innerHTML = "<option value=\"\">Error loading devices</option>";
        });
}

function loadChannelsForSpectrum() {
    fetch("/api/channels")
        .then(function(r) {
            if (!r.ok) {
                throw new Error("HTTP error! status: " + r.status);
            }
            return r.json();
        })
        .then(function(data) {
            channelsData = data;
        })
        .catch(function(err) {
            console.error("Error loading channels for spectrum:", err);
            channelsData = null;
        });
}

function toggleDisplayControls() {
    var content = document.getElementById("display-controls-content");
    var arrow = document.getElementById("display-controls-arrow");
    if (content.style.display === "none") {
        content.style.display = "block";
        arrow.textContent = "â–²";
    } else {
        content.style.display = "none";
        arrow.textContent = "â–¼";
    }
}

function startSpectrumUpdate() {
    if (spectrumUpdateInterval) return;
    updateSpectrum();
    spectrumUpdateInterval = setInterval(updateSpectrum, 100);  // Update every 100ms
}

function stopSpectrumUpdate() {
    if (spectrumUpdateInterval) {
        clearInterval(spectrumUpdateInterval);
        spectrumUpdateInterval = null;
    }
}

function updateSpectrum() {
    if (spectrumDevice < 0 || !spectrumCanvas || !waterfallCanvas) return;
    
    fetch("/api/spectrum/" + spectrumDevice)
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.data && data.data.length > 0) {
                drawSpectrum(data);
                // Update waterfall with zoom/pan applied
                updateWaterfall(data.data);
                // Update waterfall status labels
                document.getElementById("waterfall-center-freq").textContent = 
                    "Center: " + (data.center_freq / 1000000).toFixed(3) + " MHz";
                document.getElementById("waterfall-sample-rate").textContent = 
                    "Sample Rate: " + (data.sample_rate / 1000).toFixed(0) + " kHz";
                document.getElementById("waterfall-updated").textContent = 
                    "Updated: " + new Date(data.last_update * 1000).toLocaleTimeString();
            }
        })
        .catch(function(err) {
            console.error("Error updating spectrum:", err);
            document.getElementById("waterfall-updated").textContent = "Error: " + (err.message || "Unknown error");
        });
}

function drawSpectrum(data) {
    if (!data || !data.data || data.data.length === 0) return;
    
    // Store current data for redraw on control changes
    currentSpectrumData = data;
    
    var ctx = spectrumCtx;
    var width = spectrumCanvas.width;
    var height = spectrumCanvas.height;
    var dataPoints = data.data;
    var centerFreq = data.center_freq;
    var sampleRate = data.sample_rate;
    
    // Apply averaging
    if (spectrumAveraging > 1) {
        spectrumAveragingBuffer.push(dataPoints.slice());
        if (spectrumAveragingBuffer.length > spectrumAveraging) {
            spectrumAveragingBuffer.shift();
        }
        
        // Average the buffer
        if (spectrumAveragingBuffer.length > 0) {
            var averaged = new Array(dataPoints.length);
            for (var i = 0; i < dataPoints.length; i++) {
                var sum = 0;
                for (var j = 0; j < spectrumAveragingBuffer.length; j++) {
                    sum += spectrumAveragingBuffer[j][i];
                }
                averaged[i] = sum / spectrumAveragingBuffer.length;
            }
            dataPoints = averaged;
        }
    } else {
        spectrumAveragingBuffer = [];
    }
    
    // Calculate dB range based on vertical scale
    // Offset shifts the center of the range, not the minimum
    var centerDb = -60 + spectrumVerticalOffset;  // Center around -60 dB (typical noise floor)
    var dbMin = centerDb - (spectrumVerticalScale / 2);
    var dbMax = centerDb + (spectrumVerticalScale / 2);
    
    // Ensure we don't go below -150 dB (hardware limit) or above 0 dB (saturation)
    if (dbMin < -150) {
        var diff = -150 - dbMin;
        dbMin = -150;
        dbMax += diff;
    }
    if (dbMax > 0) {
        var diff = dbMax - 0;
        dbMax = 0;
        dbMin -= diff;
    }
    
    // Clear canvas
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, width, height);
    
    // Draw grid
    ctx.strokeStyle = "#333";
    ctx.lineWidth = 1;
    for (var i = 0; i <= 10; i++) {
        var y = (height / 10) * i;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
    }
    
    // Calculate visible frequency range with zoom and pan
    var fullFreqRange = sampleRate / 2;
    var fullStartFreq = centerFreq - fullFreqRange;
    var fullEndFreq = centerFreq + fullFreqRange;
    var fullBandwidth = fullEndFreq - fullStartFreq;
    
    // Apply zoom: visible bandwidth = full bandwidth / zoom
    visibleBandwidth = fullBandwidth / spectrumZoom;
    visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
    var visibleEndFreq = visibleStartFreq + visibleBandwidth;
    
    // Clamp to full bandwidth limits
    if (visibleStartFreq < fullStartFreq) {
        visibleStartFreq = fullStartFreq;
        visibleEndFreq = visibleStartFreq + visibleBandwidth;
        spectrumPan = (fullBandwidth - visibleBandwidth) / 2;
    }
    if (visibleEndFreq > fullEndFreq) {
        visibleEndFreq = fullEndFreq;
        visibleStartFreq = visibleEndFreq - visibleBandwidth;
        spectrumPan = -(fullBandwidth - visibleBandwidth) / 2;
    }
    
    // Draw frequency labels
    ctx.fillStyle = "#888";
    ctx.font = "12px monospace";
    ctx.textAlign = "left";
    ctx.textBaseline = "bottom";
    for (var i = 0; i <= 10; i++) {
        var freq = visibleStartFreq + (visibleBandwidth * i / 10);
        var x = (width / 10) * i;
        ctx.fillText((freq / 1000000).toFixed(3) + " MHz", x + 5, height - 2);
    }
    
    // Draw dB scale
    ctx.fillStyle = "#888";
    ctx.textAlign = "right";
    for (var i = 0; i <= 10; i++) {
        var db = dbMin + ((dbMax - dbMin) * i / 10);
        var y = (height / 10) * i;
        ctx.fillText(db.toFixed(0) + " dB", width - 5, y + 4);
    }
    
    // Draw mouse cursor line and frequency label
    if (mouseX >= 0 && mouseX < width) {
        // Draw vertical line at mouse position
        ctx.strokeStyle = "#ff0";
        ctx.lineWidth = 1;
        ctx.setLineDash([5, 5]);
        ctx.beginPath();
        ctx.moveTo(mouseX, 0);
        ctx.lineTo(mouseX, height);
        ctx.stroke();
        ctx.setLineDash([]);
        
        // Draw frequency label at bottom
        ctx.fillStyle = "#ff0";
        ctx.font = "bold 12px monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "top";
        var labelText = (mouseFreq / 1000000).toFixed(4) + " MHz";
        var labelWidth = ctx.measureText(labelText).width;
        var labelX = mouseX;
        var labelY = height - 18;
        
        // Ensure label stays within canvas bounds
        if (labelX - labelWidth/2 < 0) labelX = labelWidth/2;
        if (labelX + labelWidth/2 > width) labelX = width - labelWidth/2;
        
        // Draw background for label
        ctx.fillStyle = "rgba(0, 0, 0, 0.7)";
        ctx.fillRect(labelX - labelWidth/2 - 4, labelY - 2, labelWidth + 8, 16);
        
        // Draw text
        ctx.fillStyle = "#ff0";
        ctx.fillText(labelText, labelX, labelY);
    }
    
    // Draw channel markers
    if (channelsData && channelsData.devices && channelsData.devices.length > 0 && spectrumDevice >= 0) {
        ctx.strokeStyle = "#00ffff";
        ctx.lineWidth = 1.5;
        ctx.font = "11px monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "top";
        
        // Collect channels from the selected device
        var allChannels = [];
        channelsData.devices.forEach(function(dev) {
            // Only show channels for the currently selected device
            if (dev.device === spectrumDevice && dev.channels && dev.channels.length > 0) {
                dev.channels.forEach(function(ch) {
                    if (ch.freq && ch.enabled !== false) {
                        // Convert MHz to Hz
                        var channelFreqHz = ch.freq * 1000000;
                        // Check if channel is within visible range
                        if (channelFreqHz >= visibleStartFreq && channelFreqHz <= visibleEndFreq) {
                            allChannels.push({
                                freq: channelFreqHz,
                                label: ch.label || ("Channel " + (ch.channel_index !== undefined ? ch.channel_index : "")),
                                freqMhz: ch.freq
                            });
                        }
                    }
                });
            }
        });
        
        // Draw each channel marker
        allChannels.forEach(function(ch) {
            // Calculate X position based on frequency
            var x = ((ch.freq - visibleStartFreq) / visibleBandwidth) * width;
            
            // Only draw if within canvas bounds
            if (x >= 0 && x <= width) {
                // Draw vertical line
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, height);
                ctx.stroke();
                
                // Draw label background
                var labelText = ch.label + "\n" + ch.freqMhz.toFixed(3) + " MHz";
                var labelMetrics = ctx.measureText(ch.label);
                var labelWidth = Math.max(labelMetrics.width, ctx.measureText(ch.freqMhz.toFixed(3) + " MHz").width);
                var labelHeight = 32;
                var labelX = x;
                var labelY = 5;
                
                // Ensure label stays within canvas bounds
                if (labelX - labelWidth/2 < 0) labelX = labelWidth/2;
                if (labelX + labelWidth/2 > width) labelX = width - labelWidth/2;
                
                // Draw background rectangle
                ctx.fillStyle = "rgba(0, 0, 0, 0.8)";
                ctx.fillRect(labelX - labelWidth/2 - 4, labelY - 2, labelWidth + 8, labelHeight);
                
                // Draw border
                ctx.strokeStyle = "#00ffff";
                ctx.lineWidth = 1;
                ctx.strokeRect(labelX - labelWidth/2 - 4, labelY - 2, labelWidth + 8, labelHeight);
                
                // Draw channel name
                ctx.fillStyle = "#00ffff";
                ctx.textBaseline = "top";
                ctx.fillText(ch.label, labelX, labelY + 2);
                
                // Draw frequency
                ctx.fillStyle = "#ffffff";
                ctx.font = "10px monospace";
                ctx.fillText(ch.freqMhz.toFixed(3) + " MHz", labelX, labelY + 16);
                
                // Reset font
                ctx.font = "11px monospace";
            }
        });
    }
    
    // Draw spectrum line
    if (dataPoints.length > 0) {
        ctx.strokeStyle = "#0f0";
        ctx.lineWidth = 2;
        ctx.beginPath();
        
        // Map pixel X to frequency, then to data point index
        var fullFreqRange = sampleRate / 2;
        var fullStartFreq = centerFreq - fullFreqRange;
        var fullBandwidth = fullFreqRange * 2;
        var visibleBandwidth = fullBandwidth / spectrumZoom;
        var visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
        
        // Clamp visible range
        if (visibleStartFreq < fullStartFreq) {
            visibleStartFreq = fullStartFreq;
        }
        var visibleEndFreq = visibleStartFreq + visibleBandwidth;
        if (visibleEndFreq > fullStartFreq + fullBandwidth) {
            visibleEndFreq = fullStartFreq + fullBandwidth;
            visibleStartFreq = visibleEndFreq - visibleBandwidth;
        }
        
        var freqPerBin = fullBandwidth / dataPoints.length;
        var hasStarted = false;
        
        for (var x = 0; x < width; x++) {
            // Convert pixel X to frequency
            var freq = visibleStartFreq + (visibleBandwidth * x / width);
            // Convert frequency to data point index
            var idx = Math.floor((freq - fullStartFreq) / freqPerBin);
            if (idx < 0) idx = 0;
            if (idx >= dataPoints.length) idx = dataPoints.length - 1;
            
            var db = dataPoints[idx] + spectrumGain;  // Apply gain
            
            // Calculate Y position (allow values outside range to extend beyond canvas)
            var y = height - ((db - dbMin) / (dbMax - dbMin)) * height;
            
            // Only draw if within reasonable bounds (allow slight overflow for visibility)
            if (y >= -10 && y <= height + 10) {
                if (!hasStarted) {
                    ctx.moveTo(x, Math.max(0, Math.min(height, y)));
                    hasStarted = true;
                } else {
                    ctx.lineTo(x, Math.max(0, Math.min(height, y)));
                }
            } else if (hasStarted && (y < -10 || y > height + 10)) {
                // If we go off-screen, close the path and start a new one
                ctx.stroke();
                ctx.beginPath();
                hasStarted = false;
            }
        }
        
        if (hasStarted) {
            ctx.stroke();
        }
        
        // Fill area under curve (only if we have a valid path)
        if (hasStarted) {
            ctx.beginPath();
            var fullFreqRange = sampleRate / 2;
            var fullStartFreq = centerFreq - fullFreqRange;
            var fullBandwidth = fullFreqRange * 2;
            var visibleBandwidth = fullBandwidth / spectrumZoom;
            var visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
            
            if (visibleStartFreq < fullStartFreq) {
                visibleStartFreq = fullStartFreq;
            }
            var visibleEndFreq = visibleStartFreq + visibleBandwidth;
            if (visibleEndFreq > fullStartFreq + fullBandwidth) {
                visibleEndFreq = fullStartFreq + fullBandwidth;
                visibleStartFreq = visibleEndFreq - visibleBandwidth;
            }
            
            var freqPerBin = fullBandwidth / dataPoints.length;
            
            for (var x = 0; x < width; x++) {
                var freq = visibleStartFreq + (visibleBandwidth * x / width);
                var idx = Math.floor((freq - fullStartFreq) / freqPerBin);
                if (idx < 0) idx = 0;
                if (idx >= dataPoints.length) idx = dataPoints.length - 1;
                
                var db = dataPoints[idx] + spectrumGain;
                var y = height - ((db - dbMin) / (dbMax - dbMin)) * height;
                y = Math.max(0, Math.min(height, y));
                
                if (x === 0) {
                    ctx.moveTo(x, height);
                    ctx.lineTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
            }
            ctx.lineTo(width, height);
            ctx.closePath();
            var gradient = ctx.createLinearGradient(0, 0, 0, height);
            gradient.addColorStop(0, "rgba(0, 255, 0, 0.3)");
            gradient.addColorStop(1, "rgba(0, 255, 0, 0.0)");
            ctx.fillStyle = gradient;
            ctx.fill();
        }
    }
}

function updateWaterfall(spectrumData) {
    if (!waterfallCtx || !spectrumData || spectrumData.length === 0) return;
    
    // Apply speed control
    waterfallSpeedCounter++;
    if (waterfallSpeedCounter < waterfallSpeed) {
        return;  // Skip this update
    }
    waterfallSpeedCounter = 0;
    
    var width = waterfallCanvas.width;
    var height = waterfallCanvas.height;
    
    // Apply gain to waterfall data
    var adjustedData = spectrumData.map(function(db) {
        return db + spectrumGain;
    });
    
    // Add new line to waterfall data (at the beginning for top-to-bottom)
    waterfallData.unshift(adjustedData.slice());
    if (waterfallData.length > maxWaterfallLines) {
        waterfallData.pop();  // Remove oldest (bottom)
    }
    
    // Shift existing image down
    var imageData = waterfallCtx.getImageData(0, 0, width, height);
    waterfallCtx.putImageData(imageData, 0, 1);
    
    // Draw new line at top
    var y = 0;
    
    // Use same zoom/pan calculation as spectrum
    if (!currentSpectrumData) return;
    var sampleRate = currentSpectrumData.sample_rate;
    var centerFreq = currentSpectrumData.center_freq;
    var fullFreqRange = sampleRate / 2;
    var fullStartFreq = centerFreq - fullFreqRange;
    var fullBandwidth = fullFreqRange * 2;
    var visibleBandwidth = fullBandwidth / spectrumZoom;
    var visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
    
    // Clamp to full bandwidth limits
    if (visibleStartFreq < fullStartFreq) {
        visibleStartFreq = fullStartFreq;
    }
    var visibleEndFreq = visibleStartFreq + visibleBandwidth;
    if (visibleEndFreq > fullStartFreq + fullBandwidth) {
        visibleEndFreq = fullStartFreq + fullBandwidth;
        visibleStartFreq = visibleEndFreq - visibleBandwidth;
    }
    
    var freqPerBin = fullBandwidth / adjustedData.length;
    
    // Use same calculation as spectrum
    var centerDb = -60 + spectrumVerticalOffset;
    var dbMin = centerDb - (spectrumVerticalScale / 2);
    var dbMax = centerDb + (spectrumVerticalScale / 2);
    
    if (dbMin < -150) {
        var diff = -150 - dbMin;
        dbMin = -150;
        dbMax += diff;
    }
    if (dbMax > 0) {
        var diff = dbMax - 0;
        dbMax = 0;
        dbMin -= diff;
    }
    
    for (var x = 0; x < width; x++) {
        // Convert pixel X to frequency (same as spectrum)
        var freq = visibleStartFreq + (visibleBandwidth * x / width);
        // Convert frequency to data point index
        var idx = Math.floor((freq - fullStartFreq) / freqPerBin);
        if (idx < 0) idx = 0;
        if (idx >= adjustedData.length) idx = adjustedData.length - 1;
        
        var db = adjustedData[idx];
        if (db < dbMin) db = dbMin;
        if (db > dbMax) db = dbMax;
        
        // Intensity controls the dB threshold - what signals show vs. what's black
        // Low intensity (10-30%) = only strong signals, High intensity (80-100%) = weak signals too
        // Calculate threshold: intensity 0% = dbMax (only strongest), intensity 100% = dbMin (all signals)
        var intensityFactor = waterfallIntensity / 100;
        var thresholdDb = dbMax - (dbMax - dbMin) * intensityFactor;
        
        var r, g, b;
        
        // If signal is below threshold, make it completely black
        if (db < thresholdDb) {
            r = 0;
            g = 0;
            b = 0;
        } else {
            // Normalize based on range from threshold to max (not min to max)
            // This makes colors more vibrant and responsive
            var normalized = Math.max(0, Math.min(1, (db - thresholdDb) / (dbMax - thresholdDb)));
            
            // Enhanced vibrant SDR-style palette (more saturated colors)
            if (normalized < 0.10) {
                // Dark blue (0-10%)
                var t = normalized / 0.10;
                r = 0;
                g = 0;
                b = Math.floor(20 + t * 100);
            } else if (normalized < 0.25) {
                // Blue to cyan (10-25%)
                var t = (normalized - 0.10) / 0.15;
                r = 0;
                g = Math.floor(t * 200);
                b = 120 + Math.floor(t * 135);
            } else if (normalized < 0.40) {
                // Cyan to green (25-40%)
                var t = (normalized - 0.25) / 0.15;
                r = 0;
                g = 200 + Math.floor(t * 55);
                b = 255 - Math.floor(t * 100);
            } else if (normalized < 0.55) {
                // Green to yellow (40-55%)
                var t = (normalized - 0.40) / 0.15;
                r = Math.floor(t * 255);
                g = 255;
                b = 155 - Math.floor(t * 155);
            } else if (normalized < 0.70) {
                // Yellow to orange (55-70%)
                var t = (normalized - 0.55) / 0.15;
                r = 255;
                g = 255 - Math.floor(t * 100);
                b = 0;
            } else if (normalized < 0.85) {
                // Orange to red-orange (70-85%)
                var t = (normalized - 0.70) / 0.15;
                r = 255;
                g = 155 - Math.floor(t * 80);
                b = 0;
            } else {
                // Bright red (85-100%)
                var t = (normalized - 0.85) / 0.15;
                r = 255;
                g = 75 - Math.floor(t * 75);
                b = Math.floor(t * 50);  // Slight blue for very strong signals
            }
            
            // Apply slight gamma correction for better visual appearance (no intensity dimming)
            var gamma = 1.1;
            r = Math.floor(Math.pow(r / 255.0, 1.0 / gamma) * 255);
            g = Math.floor(Math.pow(g / 255.0, 1.0 / gamma) * 255);
            b = Math.floor(Math.pow(b / 255.0, 1.0 / gamma) * 255);
        }
        
        waterfallCtx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
        waterfallCtx.fillRect(x, y, 1, 1);
    }
}

// Control update functions
function updateSpectrumGain(value) {
    spectrumGain = parseFloat(value);
    document.getElementById("spectrum-gain-value").textContent = spectrumGain;
    if (currentSpectrumData) {
        drawSpectrum(currentSpectrumData);
    }
}

function updateSpectrumVerticalScale(value) {
    spectrumVerticalScale = parseFloat(value);
    document.getElementById("spectrum-vertical-scale-value").textContent = spectrumVerticalScale;
    if (currentSpectrumData) {
        drawSpectrum(currentSpectrumData);
    }
}

function updateSpectrumVerticalOffset(value) {
    spectrumVerticalOffset = parseFloat(value);
    document.getElementById("spectrum-vertical-offset-value").textContent = spectrumVerticalOffset;
    if (currentSpectrumData) {
        drawSpectrum(currentSpectrumData);
    }
}

function updateSpectrumAveraging(value) {
    spectrumAveraging = parseInt(value);
    document.getElementById("spectrum-averaging-value").textContent = spectrumAveraging;
    spectrumAveragingBuffer = [];  // Clear buffer when changing
    if (currentSpectrumData) {
        drawSpectrum(currentSpectrumData);
    }
}

function updateWaterfallIntensity(value) {
    waterfallIntensity = parseInt(value);
    document.getElementById("waterfall-intensity-value").textContent = waterfallIntensity;
    // Redraw waterfall with new intensity
    if (waterfallData.length > 0) {
        redrawWaterfall();
    }
}

function updateWaterfallSpeed(value) {
    waterfallSpeed = parseInt(value);
    document.getElementById("waterfall-speed-value").textContent = waterfallSpeed;
    waterfallSpeedCounter = 0;
}

function redrawWaterfall() {
    if (!waterfallCtx || waterfallData.length === 0 || !currentSpectrumData) return;
    
    var width = waterfallCanvas.width;
    var height = waterfallCanvas.height;
    
    // Clear
    waterfallCtx.fillStyle = "#000";
    waterfallCtx.fillRect(0, 0, width, height);
    
    // Use same zoom/pan calculation as spectrum
    var sampleRate = currentSpectrumData.sample_rate;
    var centerFreq = currentSpectrumData.center_freq;
    var fullFreqRange = sampleRate / 2;
    var fullStartFreq = centerFreq - fullFreqRange;
    var fullBandwidth = fullFreqRange * 2;
    var visibleBandwidth = fullBandwidth / spectrumZoom;
    var visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
    
    // Clamp to full bandwidth limits
    if (visibleStartFreq < fullStartFreq) {
        visibleStartFreq = fullStartFreq;
    }
    var visibleEndFreq = visibleStartFreq + visibleBandwidth;
    if (visibleEndFreq > fullStartFreq + fullBandwidth) {
        visibleEndFreq = fullStartFreq + fullBandwidth;
        visibleStartFreq = visibleEndFreq - visibleBandwidth;
    }
    
    var freqPerBin = fullBandwidth / waterfallData[0].length;
    
    // Use same calculation as spectrum
    var centerDb = -60 + spectrumVerticalOffset;
    var dbMin = centerDb - (spectrumVerticalScale / 2);
    var dbMax = centerDb + (spectrumVerticalScale / 2);
    
    if (dbMin < -150) {
        var diff = -150 - dbMin;
        dbMin = -150;
        dbMax += diff;
    }
    if (dbMax > 0) {
        var diff = dbMax - 0;
        dbMax = 0;
        dbMin -= diff;
    }
    
    for (var lineIdx = 0; lineIdx < waterfallData.length && lineIdx < maxWaterfallLines; lineIdx++) {
        var y = lineIdx;
        if (y >= height) break;
        
        var spectrumData = waterfallData[lineIdx];
        if (!spectrumData) continue;
        
        for (var x = 0; x < width; x++) {
            // Convert pixel X to frequency (same as spectrum)
            var freq = visibleStartFreq + (visibleBandwidth * x / width);
            // Convert frequency to data point index
            var idx = Math.floor((freq - fullStartFreq) / freqPerBin);
            if (idx < 0) idx = 0;
            if (idx >= spectrumData.length) idx = spectrumData.length - 1;
            
            var db = spectrumData[idx];
            if (db < dbMin) db = dbMin;
            if (db > dbMax) db = dbMax;
            
            // Intensity controls the dB threshold - what signals show vs. what's black
            var intensityFactor = waterfallIntensity / 100;
            var thresholdDb = dbMax - (dbMax - dbMin) * intensityFactor;
            
            var r, g, b;
            
            // If signal is below threshold, make it completely black
            if (db < thresholdDb) {
                r = 0;
                g = 0;
                b = 0;
            } else {
                // Normalize based on range from threshold to max
                var normalized = Math.max(0, Math.min(1, (db - thresholdDb) / (dbMax - thresholdDb)));
                
                // Enhanced vibrant SDR-style palette (same as updateWaterfall)
                if (normalized < 0.10) {
                    var t = normalized / 0.10;
                    r = 0;
                    g = 0;
                    b = Math.floor(20 + t * 100);
                } else if (normalized < 0.25) {
                    var t = (normalized - 0.10) / 0.15;
                    r = 0;
                    g = Math.floor(t * 200);
                    b = 120 + Math.floor(t * 135);
                } else if (normalized < 0.40) {
                    var t = (normalized - 0.25) / 0.15;
                    r = 0;
                    g = 200 + Math.floor(t * 55);
                    b = 255 - Math.floor(t * 100);
                } else if (normalized < 0.55) {
                    var t = (normalized - 0.40) / 0.15;
                    r = Math.floor(t * 255);
                    g = 255;
                    b = 155 - Math.floor(t * 155);
                } else if (normalized < 0.70) {
                    var t = (normalized - 0.55) / 0.15;
                    r = 255;
                    g = 255 - Math.floor(t * 100);
                    b = 0;
                } else if (normalized < 0.85) {
                    var t = (normalized - 0.70) / 0.15;
                    r = 255;
                    g = 155 - Math.floor(t * 80);
                    b = 0;
                } else {
                    var t = (normalized - 0.85) / 0.15;
                    r = 255;
                    g = 75 - Math.floor(t * 75);
                    b = Math.floor(t * 50);
                }
                
                // Apply slight gamma correction (no intensity dimming)
                var gamma = 1.1;
                r = Math.floor(Math.pow(r / 255.0, 1.0 / gamma) * 255);
                g = Math.floor(Math.pow(g / 255.0, 1.0 / gamma) * 255);
                b = Math.floor(Math.pow(b / 255.0, 1.0 / gamma) * 255);
            }
            
            waterfallCtx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
            waterfallCtx.fillRect(x, y, 1, 1);
        }
    }
}

function resetSpectrumControls() {
    document.getElementById("spectrum-gain").value = -40;
    document.getElementById("spectrum-vertical-scale").value = 70;
    document.getElementById("spectrum-vertical-offset").value = 35;
    document.getElementById("spectrum-averaging").value = 10;
    document.getElementById("waterfall-intensity").value = 50;
    document.getElementById("waterfall-speed").value = 1;
    
    updateSpectrumGain(-40);
    updateSpectrumVerticalScale(70);
    updateSpectrumVerticalOffset(35);
    updateSpectrumAveraging(10);
    updateWaterfallIntensity(50);
    updateWaterfallSpeed(1);
    
    // Reset zoom and pan
    spectrumZoom = 1.0;
    spectrumPan = 0.0;
    if (currentSpectrumData) {
        drawSpectrum(currentSpectrumData);
        // Also redraw waterfall
        if (waterfallData.length > 0) {
            redrawWaterfall();
        }
    }
}

function setupSpectrumZoomPan() {
    if (!spectrumCanvas) return;
    
    // Zoom with Ctrl + Scroll
    spectrumCanvas.addEventListener("wheel", function(e) {
        if (e.ctrlKey || e.metaKey) {
            e.preventDefault();
            
            if (!currentSpectrumData) return;
            
            var sampleRate = currentSpectrumData.sample_rate;
            var centerFreq = currentSpectrumData.center_freq;
            var fullFreqRange = sampleRate / 2;
            var fullBandwidth = fullFreqRange * 2;
            
            // Get mouse position relative to canvas
            var rect = spectrumCanvas.getBoundingClientRect();
            var mouseX = e.clientX - rect.left;
            var oldVisibleBandwidth = fullBandwidth / spectrumZoom;
            var oldVisibleStartFreq = centerFreq - fullFreqRange + (fullBandwidth - oldVisibleBandwidth) / 2 - spectrumPan;
            var mouseFreq = oldVisibleStartFreq + (oldVisibleBandwidth * mouseX / spectrumCanvas.width);
            
            // Zoom factor
            var zoomFactor = e.deltaY > 0 ? 1.1 : 0.9;
            var newZoom = spectrumZoom * zoomFactor;
            
            // Limit zoom (min 1.0, max 100x)
            newZoom = Math.max(1.0, Math.min(100.0, newZoom));
            
            if (newZoom !== spectrumZoom) {
                // Calculate new pan to keep mouse frequency under cursor
                var newVisibleBandwidth = fullBandwidth / newZoom;
                spectrumZoom = newZoom;
                var newVisibleStartFreq = centerFreq - fullFreqRange + (fullBandwidth - newVisibleBandwidth) / 2 - spectrumPan;
                var newFreqAtMouse = newVisibleStartFreq + (newVisibleBandwidth * mouseX / spectrumCanvas.width);
                spectrumPan += (mouseFreq - newFreqAtMouse);
                
                // Clamp pan
                var maxPan = (fullBandwidth - newVisibleBandwidth) / 2;
                spectrumPan = Math.max(-maxPan, Math.min(maxPan, spectrumPan));
                
                if (currentSpectrumData) {
                    drawSpectrum(currentSpectrumData);
                    // Also redraw waterfall to match zoom/pan
                    if (waterfallData.length > 0) {
                        redrawWaterfall();
                    }
                }
            }
        }
    }, { passive: false });
    
    // Pan with click and drag
    spectrumCanvas.addEventListener("mousedown", function(e) {
        if (spectrumZoom > 1.0) {
            isPanning = true;
            panStartX = e.clientX;
            panStartOffset = spectrumPan;
            spectrumCanvas.style.cursor = "grabbing";
        }
    });
    
    spectrumCanvas.addEventListener("mousemove", function(e) {
        var rect = spectrumCanvas.getBoundingClientRect();
        var x = e.clientX - rect.left;
        var y = e.clientY - rect.top;
        
        // Update mouse position and frequency
        if (x >= 0 && x < spectrumCanvas.width && y >= 0 && y < spectrumCanvas.height) {
            mouseX = x;
            if (currentSpectrumData) {
                var sampleRate = currentSpectrumData.sample_rate;
                var centerFreq = currentSpectrumData.center_freq;
                var fullFreqRange = sampleRate / 2;
                var fullStartFreq = centerFreq - fullFreqRange;
                var fullBandwidth = fullFreqRange * 2;
                var visibleBandwidth = fullBandwidth / spectrumZoom;
                var visibleStartFreq = fullStartFreq + (fullBandwidth - visibleBandwidth) / 2 - spectrumPan;
                
                // Clamp to full bandwidth limits
                if (visibleStartFreq < fullStartFreq) {
                    visibleStartFreq = fullStartFreq;
                }
                var visibleEndFreq = visibleStartFreq + visibleBandwidth;
                if (visibleEndFreq > fullStartFreq + fullBandwidth) {
                    visibleEndFreq = fullStartFreq + fullBandwidth;
                    visibleStartFreq = visibleEndFreq - visibleBandwidth;
                }
                
                mouseFreq = visibleStartFreq + (visibleBandwidth * x / spectrumCanvas.width);
                
                // Redraw to show cursor line
                if (currentSpectrumData) {
                    drawSpectrum(currentSpectrumData);
                }
            }
        } else {
            mouseX = -1;
            if (currentSpectrumData) {
                drawSpectrum(currentSpectrumData);
            }
        }
        
        if (isPanning && currentSpectrumData) {
            var sampleRate = currentSpectrumData.sample_rate;
            var fullFreqRange = sampleRate / 2;
            var fullBandwidth = fullFreqRange * 2;
            var visibleBandwidth = fullBandwidth / spectrumZoom;
            
            var deltaX = e.clientX - panStartX;
            var deltaFreq = (deltaX / spectrumCanvas.width) * visibleBandwidth;
            spectrumPan = panStartOffset + deltaFreq;
            
            // Clamp pan to limits
            var maxPan = (fullBandwidth - visibleBandwidth) / 2;
            spectrumPan = Math.max(-maxPan, Math.min(maxPan, spectrumPan));
            
            if (currentSpectrumData) {
                drawSpectrum(currentSpectrumData);
                // Also redraw waterfall to match zoom/pan
                if (waterfallData.length > 0) {
                    redrawWaterfall();
                }
            }
        } else if (spectrumZoom > 1.0) {
            spectrumCanvas.style.cursor = "grab";
        } else {
            spectrumCanvas.style.cursor = "default";
        }
    });
    
    spectrumCanvas.addEventListener("mouseleave", function(e) {
        mouseX = -1;
        if (currentSpectrumData) {
            drawSpectrum(currentSpectrumData);
        }
        if (isPanning) {
            isPanning = false;
            spectrumCanvas.style.cursor = spectrumZoom > 1.0 ? "grab" : "default";
        }
    });
    
    spectrumCanvas.addEventListener("mouseup", function(e) {
        if (isPanning) {
            isPanning = false;
            spectrumCanvas.style.cursor = spectrumZoom > 1.0 ? "grab" : "default";
        }
    });
}

// Helper variables for zoom calculations (needed in event handlers)
var visibleStartFreq = 0;
var visibleBandwidth = 0;

function clearWaterfall() {
    waterfallData = [];
    if (waterfallCtx) {
        var width = waterfallCanvas.width;
        var height = waterfallCanvas.height;
        waterfallCtx.fillStyle = "#000";
        waterfallCtx.fillRect(0, 0, width, height);
    }
}

// Export function to hook into showPage from main app
// This will be called by web_ui.js to integrate spectrum analyzer
function initSpectrumModule(showPageFunction) {
    // Override showPage to handle spectrum page
    var originalShowPage = showPageFunction;
    window.showPage = function(page) {
        originalShowPage(page);
        if (page === "spectrum") {
            if (!spectrumCanvas) {
                setTimeout(initSpectrum, 100);
            } else {
                loadSpectrumDevices();
                if (spectrumDevice >= 0 && document.getElementById("spectrum-auto-update") && document.getElementById("spectrum-auto-update").checked) {
                    startSpectrumUpdate();
                }
            }
        } else {
            stopSpectrumUpdate();
        }
    };
}
let ws = null;
let reconnectAttempts = 0;
const maxReconnectAttempts = 5;

const userTimezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
const userTimezoneOffset = new Date().getTimezoneOffset() * 60;

let maintenanceStartTime = null;
const maintenanceTimeout = 60 * 60 * 1000;
let maintenanceInterval = null;

let minTemp = parseFloat(localStorage.getItem('minTemp')) || 22.0;
let maxTemp = parseFloat(localStorage.getItem('maxTemp')) || 30.0;

function connect() {
    const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${wsProtocol}//${window.location.hostname}/ws`;
    updateConnectionStatus('connecting');
    ws = new WebSocket(wsUrl);

    ws.onopen = function() {
        reconnectAttempts = 0;
        updateConnectionStatus('connected');
    };

    ws.onmessage = function(event) {
        const data = JSON.parse(event.data);
        updateUI(data);
    };

    ws.onclose = function() {
        updateConnectionStatus('disconnected');
        if (reconnectAttempts < maxReconnectAttempts) {
            setTimeout(connect, 2000);
            reconnectAttempts++;
        }
    };

    ws.onerror = function(error) {
        updateConnectionStatus('error');
    };
}

function updateConnectionStatus(status) {
    const statusDiv = document.getElementById('connection-status');
    const statusDot = statusDiv.querySelector('.dot');
    const statusText = statusDiv.querySelector('.text');
    statusDiv.className = 'status-indicator ' + status;
    switch(status) {
        case 'connected':
            statusText.textContent = 'Connected';
            break;
        case 'disconnected':
            statusText.textContent = 'Disconnected';
            break;
        case 'connecting':
            statusText.textContent = 'Connecting...';
            break;
        case 'error':
            statusText.textContent = 'Connection Error';
            break;
    }
}

function updateUI(data) {
    // Maintenance mode UI
    const maintenanceText = document.getElementById('maintenance-text');
    if (data.maintenance_mode) {
        maintenanceText.textContent = 'Maintenance';
        maintenanceText.className = 'warning-text';
        document.getElementById('pause-btn').disabled = true;
        document.getElementById('resume-btn').disabled = false;
        // Always show timer in maintenance mode
        startMaintenanceCountdown(data.maintenance_remaining || 0);
    } else {
        maintenanceText.textContent = 'Active';
        maintenanceText.className = 'success-text';
        document.getElementById('pause-btn').disabled = false;
        document.getElementById('resume-btn').disabled = true;
        stopMaintenanceCountdown();
        document.getElementById('maintenance-timer').textContent = '';
    }

    // Pump status
    const pumpStatus = document.querySelector('#pump-status .status-value');
    pumpStatus.textContent = data.pumping ? 'Active' : (data.maintenance_mode ? 'Paused' : 'Inactive');
    pumpStatus.className = 'status-value ' + (data.pumping ? 'active' : (data.maintenance_mode ? 'warning' : 'inactive'));

    // Last run
    const lastRun = document.querySelector('#pump-status .last-run');
    if (data.last_pump_run) {
        const date = new Date(data.last_pump_run * 1000);
        lastRun.textContent = 'Last run: ' + date.toLocaleString();
    }

    // Water levels
    const sumpLevel = document.getElementById('sump-level');
    sumpLevel.textContent = data.sump_low ? 'Low' : 'Normal';
    sumpLevel.className = 'value ' + (data.sump_low ? 'warning' : 'normal');

    const rodiLevel = document.getElementById('rodi-level');
    rodiLevel.textContent = data.rodi_low ? 'Low' : 'Normal';
    rodiLevel.className = 'value ' + (data.rodi_low ? 'warning' : 'normal');

    const emergencyLevel = document.getElementById('emergency-level');
    emergencyLevel.textContent = data.emergency_high ? 'HIGH!' : 'Normal';
    emergencyLevel.className = 'value ' + (data.emergency_high ? 'error' : 'normal');

    // Temperature color
    const tempDiv = document.querySelector('#temperature .temp-value');
    tempDiv.innerHTML = `${data.temperature.toFixed(1)}&deg;C`;
    if (data.temperature > maxTemp) {
        tempDiv.className = 'temp-value temp-high';
    } else if (data.temperature < minTemp) {
        tempDiv.className = 'temp-value temp-low';
    } else {
        tempDiv.className = 'temp-value temp-ok';
    }

    // Safe range
    document.getElementById('safe-range').textContent = minTemp + ' - ' + maxTemp + '\u00B0C';

    // Error status
    const errorDiv = document.querySelector('#error-status .error-message');
    const errorCodeDiv = document.querySelector('#error-status .error-code');
    const resetButton = document.getElementById('reset-error-btn');
    if (data.error) {
        let msg = "Unknown Error";
        if (data.error_code === 1) msg = "Emergency High Water Level";
        else if (data.error_code === 3) msg = "Pump Timeout";
        else if (data.error_code === 4) msg = "Temperature Sensor Error";
        errorDiv.textContent = msg;
        errorCodeDiv.textContent = `Error Code: ${data.error_code}`;
        document.getElementById('error-status').className = 'card error';
        resetButton.style.display = data.error_code === 3 ? 'block' : 'none';
    } else {
        errorDiv.textContent = 'No Errors';
        errorCodeDiv.textContent = '';
        document.getElementById('error-status').className = 'card';
        resetButton.style.display = 'none';
    }

    // System info
    if (data.device_name) document.getElementById('device-name').textContent = data.device_name;
    if (data.uptime) document.getElementById('uptime').textContent = formatUptime(data.uptime);
    if (data.wifi_signal) {
        const signal = data.wifi_signal;
        const signalText = signal + ' dBm (' + getSignalStrength(signal) + ')';
        document.getElementById('wifi-signal').textContent = signalText;
    }
    if (data.fw_version) {
        document.getElementById('fw-version').textContent = data.fw_version;
    }
    fetch('/frontend_version.json')
  .then(r => r.json())
  .then(data => {
    document.getElementById('frontend-version').textContent = data.version;
  })
  .catch(() => {
    document.getElementById('frontend-version').textContent = 'unknown';
  });
    // Min/max temp inputs
    if (typeof data.min_temp !== "undefined") {
        minTemp = data.min_temp;
        document.getElementById('min-temp').value = minTemp;
        localStorage.setItem('minTemp', minTemp);
    }
    if (typeof data.max_temp !== "undefined") {
        maxTemp = data.max_temp;
        document.getElementById('max-temp').value = maxTemp;
        localStorage.setItem('maxTemp', maxTemp);
    }

    // Update notice
    const updateNotice = document.getElementById('updateNotice');
    const updateMsg = document.getElementById('updateMsg');
    const goUpdateBtn = document.getElementById('goUpdateBtn');

    if ((data.firmware_update_available || data.frontend_update_available) && updateNotice && updateMsg && goUpdateBtn) {
        let msg = '';
        // Try to get frontend version from API payload, fallback to SPIFFS file if missing
        let frontendVersion = (typeof data.frontend_version === "string" && data.frontend_version && data.frontend_version !== "unknown")
            ? data.frontend_version
            : null;

        // If not present in API, fetch from SPIFFS (async)
        if (!frontendVersion) {
            fetch('/frontend_version.json')
                .then(r => r.json())
                .then(json => {
                    frontendVersion = json.version || "unknown";
                    showUpdateBanner(msg, frontendVersion, data);
                })
                .catch(() => {
                    frontendVersion = "unknown";
                    showUpdateBanner(msg, frontendVersion, data);
                });
        } else {
            showUpdateBanner(msg, frontendVersion, data);
        }
    } else if (updateNotice) {
        updateNotice.style.display = 'none';
    }

    // Helper function to build and show the update banner
    function showUpdateBanner(msg, frontendVersion, data) {
        let frontendUpdateVersion = data.frontend_update_version || "?";
        // Only show Web UI update if versions are different and not unknown
        const showFrontendUpdate = data.frontend_update_available &&
            frontendVersion !== "unknown" &&
            frontendUpdateVersion !== "?" &&
            frontendVersion !== frontendUpdateVersion;

        if (data.firmware_update_available) {
            msg += `New firmware available (v${data.fw_version} -> server v${data.firmware_update_version || "?"})`;
        }
        if (showFrontendUpdate) {
            if (msg) msg += ' & ';
            msg += `New web UI available (v${frontendVersion} -> server v${frontendUpdateVersion})`;
        }
        updateMsg.textContent = msg;
        updateNotice.style.display = msg ? 'flex' : 'none';
        goUpdateBtn.onclick = function() {
            updateNotice.style.display = 'block';
            updateMsg.innerHTML = '<span class="spinner"></span> Updating firmware... Device will reboot when done.';
            goUpdateBtn.disabled = true;

            fetch('/api/auto_update', {method: 'POST'})
                .then(r => r.text())
                .then(msg => {
                    // Keep showing the banner and spinner until the device reboots
                    updateMsg.innerHTML = '<span class="spinner"></span> Updating firmware... Device will reboot when done.';
                    // Optionally, show the server message below the spinner
                    // updateMsg.innerHTML += `<br>${msg}`;
                })
                .catch(() => {
                    updateMsg.textContent = "Update failed!";
                    goUpdateBtn.disabled = false;
                    setTimeout(() => {
                        updateNotice.style.display = 'none';
                    }, 5000);
                });
        };
    }
}

function getSignalStrength(signal) {
    if (signal >= -50) return 'Excellent';
    if (signal >= -60) return 'Good';
    if (signal >= -70) return 'Fair';
    return 'Poor';
}

function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    let uptime = '';
    if (days > 0) uptime += `${days}d `;
    if (hours > 0) uptime += `${hours}h `;
    uptime += `${minutes}m`;
    return uptime;
}

// REMOVE the following block from your script.js:

function toggleMaintenance(enable) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ maintenance: enable }));
    }
}

window.toggleMaintenance = toggleMaintenance;

function resetError() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ reset_error: true }));
    }
}

function resetWiFi() {
    if (confirm("Are you sure you want to reset WiFi settings? This will reboot the device.")) {
        fetch('/reset_wifi', {method: 'POST'})
            .then(r => alert('WiFi will be reset. Rebooting!'));
    }
}

// Temperature Graph Logic
let tempChart = null;

document.addEventListener('DOMContentLoaded', function () {
    const tempValueElement = document.querySelector('.temp-value');
    if (tempValueElement) {
        tempValueElement.addEventListener('click', function () {
            const modal = document.getElementById('tempGraphModal');
            modal.style.display = 'block';
            fetch('/temperature_history')
                .then(response => response.json())
                .then(data => {
                    const chartData = data.map(entry => [
                        entry.timestamp * 1000,
                        entry.temperature
                    ]);
                    if (!tempChart) {
                        tempChart = Highcharts.chart('chart-temperature', {
                            chart: { type: 'line', zoomType: 'x' },
                            title: { text: '' },
                            xAxis: {
                                type: 'datetime',
                                labels: { format: '{value:%H:%M}' }
                            },
                            yAxis: { title: { text: 'Temperature (\u00B0C)' } },
                            series: [{
                                name: 'Temperature',
                                data: chartData,
                                color: '#2196F3',
                                marker: { enabled: false }
                            }],
                            credits: { enabled: false }
                        });
                    } else {
                        tempChart.series[0].setData(chartData);
                    }
                });
        });
    }

    const closeButton = document.querySelector('.close');
    if (closeButton) {
        closeButton.onclick = function () {
            document.getElementById('tempGraphModal').style.display = 'none';
        };
    }

    window.onclick = function (event) {
        const modal = document.getElementById('tempGraphModal');
        if (event.target === modal) {
            modal.style.display = 'none';
        }
    };

    connect();
});

// Handle min/max temp set
document.addEventListener('DOMContentLoaded', function () {
    document.getElementById('min-temp').value = minTemp;
    document.getElementById('max-temp').value = maxTemp;
    document.getElementById('save-temp-range').onclick = function () {
        minTemp = parseFloat(document.getElementById('min-temp').value);
        maxTemp = parseFloat(document.getElementById('max-temp').value);
        localStorage.setItem('minTemp', minTemp);
        localStorage.setItem('maxTemp', maxTemp);
        fetch('/set_temp_range', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `min=${minTemp}&max=${maxTemp}`
        })
        .then(r => r.text())
        .then(msg => alert(msg));
    };
});

document.addEventListener('DOMContentLoaded', function () {
    // Modal elements
    const openSettingsBtn = document.getElementById('open-settings-modal');
    const settingsModal = document.getElementById('settingsModal');
    const closeSettingsBtn = document.getElementById('close-settings-modal');
    const saveDeviceNameBtn = document.getElementById('save-device-name');
    const setDeviceNameInput = document.getElementById('set-device-name');
    const checkBtn = document.getElementById('check-updates-btn');
    const tempGraphModal = document.getElementById('tempGraphModal');
    const closeTempGraphBtn = document.querySelector('#tempGraphModal .close');

    // Open settings modal
    if (openSettingsBtn && settingsModal) {
        openSettingsBtn.onclick = function () {
            setDeviceNameInput.value = '';
            settingsModal.style.display = 'block';
        };
    }

    // Close settings modal with close button
    if (closeSettingsBtn && settingsModal) {
        closeSettingsBtn.onclick = function () {
            settingsModal.style.display = 'none';
        };
    }

    // Save device name
    if (saveDeviceNameBtn && setDeviceNameInput && settingsModal) {
        saveDeviceNameBtn.onclick = function () {
            const name = setDeviceNameInput.value.trim();
            if (!name) {
                alert('Please enter a device name.');
                return;
            }
            if (!confirm(`Are you sure you want to change the device name to "${name}-ato"?`)) {
                return;
            }
            fetch('/set_device_name', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: `device_name=${encodeURIComponent(name)}`
            })
            .then(r => r.text())
            .then(msg => {
                alert(msg);
                settingsModal.style.display = 'none';
            });
        };
    }

    // Check for updates button
    if (checkBtn && settingsModal) {
        checkBtn.onclick = function () {
            checkBtn.disabled = true;
            checkBtn.textContent = "Checking...";
            fetch('/api/check_updates', {method: 'POST'})
                .then(r => r.text())
                .then(msg => {
                    fetch('/api/status')
                        .then(r => r.json())
                        .then(data => {
                            checkBtn.textContent = "Check For Updates";
                            checkBtn.disabled = false;
                            settingsModal.style.display = 'none'; // Close modal
                            if (data.firmware_update_available || data.frontend_update_available) {
                                location.reload();
                            } else {
                                alert("No updates available.");
                            }
                        });
                })
                .catch(() => {
                    alert("Failed to check for updates.");
                    checkBtn.textContent = "Check For Updates";
                    checkBtn.disabled = false;
                    settingsModal.style.display = 'none'; // Close modal on error too
                });
        };
    }

    // Open temperature graph modal
    const tempValueElement = document.querySelector('.temp-value');
    if (tempValueElement && tempGraphModal) {
        tempValueElement.addEventListener('click', function () {
            tempGraphModal.style.display = 'block';
            fetch('/temperature_history')
                .then(response => response.json())
                .then(data => {
                    const chartData = data.map(entry => [
                        entry.timestamp * 1000,
                        entry.temperature
                    ]);
                    if (!tempChart) {
                        tempChart = Highcharts.chart('chart-temperature', {
                            chart: { type: 'line', zoomType: 'x' },
                            title: { text: '' },
                            xAxis: {
                                type: 'datetime',
                                labels: { format: '{value:%H:%M}' }
                            },
                            yAxis: { title: { text: 'Temperature (\u00B0C)' } },
                            series: [{
                                name: 'Temperature',
                                data: chartData,
                                color: '#2196F3',
                                marker: { enabled: false }
                            }],
                            credits: { enabled: false }
                        });
                    } else {
                        tempChart.series[0].setData(chartData);
                    }
                });
        });
    }

    // Close temperature graph modal with close button
    if (closeTempGraphBtn && tempGraphModal) {
        closeTempGraphBtn.onclick = function () {
            tempGraphModal.style.display = 'none';
        };
    }

    // Universal window click handler for all modals
    window.onclick = function (event) {
        if (settingsModal && event.target === settingsModal) {
            settingsModal.style.display = 'none';
        }
        if (tempGraphModal && event.target === tempGraphModal) {
            tempGraphModal.style.display = 'none';
        }
    };
});

// Maintenance timer logic
function startMaintenanceCountdown(seconds) {
    stopMaintenanceCountdown();
    updateCountdownDisplay(seconds);
    if (seconds > 0) {
        maintenanceInterval = setInterval(() => {
            seconds--;
            if (seconds <= 0) {
                stopMaintenanceCountdown();
                document.getElementById('maintenance-timer').textContent = '';
            } else {
                updateCountdownDisplay(seconds);
            }
        }, 1000);
    }
}
function stopMaintenanceCountdown() {
    if (maintenanceInterval) {
        clearInterval(maintenanceInterval);
        maintenanceInterval = null;
    }
}
function updateCountdownDisplay(seconds) {
    const el = document.getElementById('maintenance-timer');
    const m = Math.floor(seconds / 60);
    const s = seconds % 60;
    el.textContent = `Maintenance ends in: ${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

document.addEventListener('DOMContentLoaded', function () {
    const showBtn = document.getElementById('show-temp-range');
    const fields = document.getElementById('temp-range-fields');
    if (showBtn && fields) {
        showBtn.onclick = function () {
            fields.style.display = fields.style.display === 'none' ? 'flex' : 'none';
        };
    }
});

let otaRebootCheckInterval = null;
let otaRebootWasOffline = false;

function startOtaRebootWatcher() {
    if (otaRebootCheckInterval) return; // Already running
    otaRebootCheckInterval = setInterval(() => {
        fetch('/api/status', {cache: "no-store"})
            .then(r => {
                if (!r.ok) throw new Error();
                if (otaRebootWasOffline) {
                    // Device is back online after reboot, reload!
                    location.reload();
                }
                otaRebootWasOffline = false;
            })
            .catch(() => {
                otaRebootWasOffline = true;
                // Optionally, show a "Reconnecting..." message
                updateMsg.innerHTML = '<span class="spinner"></span> Device rebooting, waiting to reconnect...';
            });
    }, 2000); // Ping every 2 seconds
}
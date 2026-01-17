#!/bin/bash

# Rebuild script for Boondock Airband
# This script removes the build folder, recompiles the application, and runs it
#
# Usage: ./rebuild.sh

set -e  # Exit on error

echo "=== Boondock Airband Rebuild Script ==="

# Step 1: Stop the service
echo "Step 1: Stopping boondock-airband service..."
if systemctl is-active --quiet boondock-airband.service 2>/dev/null; then
    echo "Service is running, stopping it..."
    sudo systemctl stop boondock-airband.service
    sleep 1
    echo "Service stopped."
else
    echo "Service is not running or does not exist."
fi

# Step 2: Remove build folder if it exists
echo ""
echo "Step 2: Removing existing build folder..."
if [ -d "build" ]; then
    sudo rm -rf build
    echo "Build folder removed."
else
    echo "No build folder found."
fi

# Step 3: Create build folder
echo ""
echo "Step 3: Creating build folder..."
sudo mkdir -p build
cd build

# Step 4: Run cmake
echo ""
echo "Step 4: Running cmake..."
# Use rpi5 platform for Raspberry Pi 5 optimizations (change to 'native' for other systems)
sudo cmake .. -DCMAKE_BUILD_TYPE=Release -DPLATFORM=rpi5

# Step 5: Compile the application
echo ""
echo "Step 5: Compiling application..."
sudo make -j$(nproc)

# Step 6: Copy executable to /opt/boondock/airband
echo ""
echo "Step 6: Copying boondock_airband to /opt/boondock/airband..."
sudo mkdir -p /opt/boondock/airband
sudo cp src/boondock_airband /opt/boondock/airband/boondock_airband
sudo chmod +x /opt/boondock/airband/boondock_airband
echo "Binary copied and permissions set."

# Step 7: Install service if it doesn't exist
echo ""
echo "Step 7: Checking for systemd service..."
if [ ! -f /etc/systemd/system/boondock-airband.service ]; then
    echo "Service file not found. Installing service..."
    sudo tee /etc/systemd/system/boondock-airband.service > /dev/null << 'EOF'
[Unit]
Description=Boondock Airband Service
After=network.target

[Service]
# Replace 'kaushlesh' with your actual username if different
User=kaushlesh
Group=kaushlesh

WorkingDirectory=/opt/boondock/airband
ExecStart=/opt/boondock/airband/boondock_airband -F -e

# Only restart on failure, not when manually stopped
Restart=on-failure
# Wait 5 seconds before restarting after a crash
RestartSec=5
# Give the service 10 seconds to shut down gracefully, then force kill
TimeoutStopSec=10
# Send SIGTERM first
KillSignal=SIGTERM
# After TimeoutStopSec, send SIGKILL
FinalKillSignal=SIGKILL
# Kill all processes in the process group
KillMode=mixed
# Don't wait for remaining processes after main process exits
RemainAfterExit=no

[Install]
WantedBy=multi-user.target
EOF
    echo "Service file created. Reloading systemd daemon..."
    sudo systemctl daemon-reload
    echo "Service installed."
else
    echo "Service file already exists."
    # Reload systemd in case the binary was updated
    sudo systemctl daemon-reload
fi

# Step 8: Start the service
echo ""
echo "Step 8: Starting boondock-airband service..."
sudo systemctl start boondock-airband.service
sleep 1

# Check service status
if systemctl is-active --quiet boondock-airband.service; then
    echo "Service started successfully!"
    echo "=========================================="
    echo "Build complete! Service is running."
    echo "Use 'sudo systemctl status boondock-airband.service' to check status."
else
    echo "Warning: Service may not have started correctly."
    echo "Check status with: sudo systemctl status boondock-airband.service"
fi

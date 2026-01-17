#!/bin/bash

# Rebuild script for Boondock Airband
# This script removes the build folder, recompiles the application, and runs it
#
# Usage: ./rebuild.sh

set -e  # Exit on error

echo "=== Boondock Airband Rebuild Script ==="

# Step 1: Kill any running boondock_airband process
echo "Step 1: Checking for running boondock_airband processes..."
if pgrep -f boondock_airband > /dev/null; then
    echo "Found running boondock_airband process, killing it..."
    sudo pkill -9 -f boondock_airband
    sleep 1
    echo "Process killed."
else
    echo "No running boondock_airband process found."
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
sudo cmake .. -DCMAKE_BUILD_TYPE=Release

# Step 5: Compile the application
echo ""
echo "Step 5: Compiling application..."
sudo make -j$(nproc)

# Step 6: Run the executable
echo ""
echo "Step 6: Build complete! Starting boondock_airband..."
echo "=========================================="
sudo ./src/boondock_airband -F -e

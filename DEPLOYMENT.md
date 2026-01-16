# Deployment Guide - Running Without Recompiling

This guide explains how to copy the compiled `boondock_airband` binary to a new machine and run it without recompiling.

## Files to Copy

### 1. The Binary (Required)

Copy the compiled binary:
```bash
# From build machine
cp build/src/boondock_airband /path/to/transfer/
```

**Location on build machine:** `build/src/boondock_airband`

### 2. Configuration Files (Optional but Recommended)

Copy example configuration files:
```bash
# Copy config directory
cp -r config/ /path/to/transfer/
```

**Location:** `config/` directory with example `.conf` files

## Runtime Dependencies on New Machine

The new machine must have the **runtime libraries** installed. The binary is dynamically linked, so these shared libraries must be present.

### Required Runtime Libraries

**On Debian/Ubuntu/Raspberry Pi OS:**
```bash
sudo apt-get update
sudo apt-get install -y \
    libmp3lame0 \
    libshout3 \
    libconfig++9v5 \
    libfftw3-single3 \
    libsoapysdr0.8 \
    libpulse0 \
    libusb-1.0-0 \
    librtlsdr0
```

**On Fedora/RHEL/CentOS:**
```bash
sudo dnf install -y \
    lame \
    libshout \
    libconfig++ \
    fftw3 \
    SoapySDR \
    pulseaudio-libs \
    libusb \
    librtlsdr
```

### Optional: SDR-Specific Libraries

If you built with specific SDR support, install the corresponding runtime libraries:

- **RTL-SDR**: `librtlsdr0` (Debian) or `librtlsdr` (Fedora)
- **MiriSDR**: May need to copy `libmirisdr.so.4` if built from source
- **SoapySDR**: `libsoapysdr0.8` (Debian) or `SoapySDR` (Fedora)

## Deployment Steps

### Step 1: Check Binary Dependencies

On the **build machine**, check what libraries the binary needs:

```bash
cd build
ldd src/boondock_airband
```

This will show all required shared libraries. Example output:
```
libmp3lame.so.0 => /usr/lib/aarch64-linux-gnu/libmp3lame.so.0
libshout.so.3 => /usr/lib/aarch64-linux-gnu/libshout.so.3
libconfig++.so.9 => /usr/lib/aarch64-linux-gnu/libconfig++.so.9
libfftw3f.so.3 => /usr/lib/aarch64-linux-gnu/libfftw3f.so.3
...
```

### Step 2: Copy Files to New Machine

**Option A: Using SCP (over network)**
```bash
# Copy binary
scp build/src/boondock_airband user@newmachine:/usr/local/bin/

# Copy config files (optional)
scp -r config/ user@newmachine:/opt/boondock/config/
```

**Option B: Using USB drive or other media**
```bash
# On build machine
mkdir -p /path/to/usb/boondock_deploy
cp build/src/boondock_airband /path/to/usb/boondock_deploy/
cp -r config/ /path/to/usb/boondock_deploy/

# On new machine
sudo cp /path/to/usb/boondock_deploy/boondock_airband /usr/local/bin/
sudo chmod +x /usr/local/bin/boondock_airband
sudo cp -r /path/to/usb/boondock_deploy/config/ /opt/boondock/
```

### Step 3: Install Runtime Dependencies on New Machine

```bash
# Debian/Ubuntu/Raspberry Pi OS
sudo apt-get update
sudo apt-get install -y \
    libmp3lame0 \
    libshout3 \
    libconfig++9v5 \
    libfftw3-single3 \
    libsoapysdr0.8 \
    libpulse0 \
    libusb-1.0-0
```

### Step 4: Verify Binary Works

On the new machine, check if all dependencies are satisfied:

```bash
ldd /usr/local/bin/boondock_airband
```

All libraries should show a path (not "not found"). If any show "not found", install the missing library.

Test the binary:
```bash
/usr/local/bin/boondock_airband -h
```

### Step 5: Create Configuration File

```bash
sudo cp /opt/boondock/config/basic_multichannel.conf /etc/boondock_airband.conf
sudo nano /etc/boondock_airband.conf
```

## Architecture Compatibility

**Important:** The binary must match the target machine's architecture:

- **ARM 64-bit (aarch64)**: Raspberry Pi 4/5, ARM servers
- **ARM 32-bit (armv7l)**: Raspberry Pi 2/3 (older models)
- **x86_64**: Intel/AMD 64-bit systems
- **x86**: Intel/AMD 32-bit systems

**Check architecture:**
```bash
# On build machine
file build/src/boondock_airband

# On new machine
uname -m
```

Both should match (e.g., both `aarch64` or both `x86_64`).

## Creating a Deployment Package

You can create a simple deployment script:

```bash
#!/bin/bash
# deploy_package.sh

DEPLOY_DIR="boondock_deploy_$(uname -m)"
mkdir -p "$DEPLOY_DIR"

# Copy binary
cp build/src/boondock_airband "$DEPLOY_DIR/"

# Copy config examples
cp -r config "$DEPLOY_DIR/"

# Create README with dependencies
cat > "$DEPLOY_DIR/README_DEPLOY.txt" << EOF
Boondock-Airband Deployment Package
Architecture: $(uname -m)

INSTALLATION:
1. Install runtime dependencies:
   sudo apt-get install libmp3lame0 libshout3 libconfig++9v5 libfftw3-single3 libsoapysdr0.8 libpulse0 libusb-1.0-0

2. Copy binary:
   sudo cp boondock_airband /usr/local/bin/
   sudo chmod +x /usr/local/bin/boondock_airband

3. Create config:
   sudo cp config/basic_multichannel.conf /etc/boondock_airband.conf
   sudo nano /etc/boondock_airband.conf

4. Run:
   sudo boondock_airband -F -e -c /etc/boondock_airband.conf
EOF

# Create tarball
tar -czf "${DEPLOY_DIR}.tar.gz" "$DEPLOY_DIR"
echo "Created deployment package: ${DEPLOY_DIR}.tar.gz"
```

Run it:
```bash
chmod +x deploy_package.sh
./deploy_package.sh
```

## Troubleshooting

### "command not found"
- Binary not in PATH or not executable
- Fix: `sudo cp boondock_airband /usr/local/bin/ && sudo chmod +x /usr/local/bin/boondock_airband`

### "No such file or directory" (when running binary)
- Missing shared library or wrong architecture
- Fix: Check with `ldd boondock_airband` and install missing libraries

### "cannot open shared object file"
- Library version mismatch
- Fix: Install the correct version of the library, or rebuild on target architecture

### Architecture Mismatch
- Binary built for different CPU architecture
- Fix: Rebuild on the target machine or use a matching architecture

## Static Linking (Advanced)

If you want a truly standalone binary (no external dependencies), you would need to:
1. Rebuild with static linking flags
2. This significantly increases binary size
3. May not work for all libraries (some don't support static linking)

This is generally not recommended as it's complex and the binary becomes very large.

# Boondock-Airband

Boondock-Airband receives analog radio voice channels and produces audio streams which can be routed to various outputs, such as online streaming services like LiveATC.net. Originally based on RTLSDR-Airband, this fork supports Realtek DVB-T dongles and other SDR devices via SoapySDR.

## Overview

Boondock-Airband is a software-defined radio (SDR) application that:
- Receives analog radio voice channels (e.g., air traffic control)
- Demodulates AM/NFM signals
- Routes audio to various outputs (Icecast, files, UDP, PulseAudio, etc.)

## Building on Linux

### Prerequisites

Install build dependencies:

**On Debian/Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libmp3lame-dev \
    libshout3-dev \
    libconfig++-dev \
    libfftw3-dev \
    libsoapysdr-dev \
    libpulse-dev \
    libusb-1.0-0-dev \
    pkg-config \
    git
```

**On Fedora/RHEL/CentOS:**
```bash
sudo dnf install -y \
    gcc-c++ \
    cmake \
    lame-devel \
    libshout-devel \
    libconfig++-devel \
    fftw3-devel \
    SoapySDR-devel \
    pulseaudio-libs-devel \
    libusb-devel \
    pkgconfig \
    git
```

### Optional: RTL-SDR Library

If using RTL-SDR dongles:
```bash
# Debian/Ubuntu
sudo apt-get install librtlsdr-dev
```

### Build Steps

```bash
# Navigate to the project directory
cd /path/to/Boondock-Airband-1

# Create build directory
mkdir build
cd build

# Configure with CMake
# Basic build (AM only):
cmake .. -DCMAKE_BUILD_TYPE=Release

# With NFM (Narrow FM) support:
cmake .. -DCMAKE_BUILD_TYPE=Release -DNFM=ON

# Build (use -jN where N is number of CPU cores)
cmake --build . -j$(nproc)

# Or use make directly:
make -j$(nproc)
```

The binary will be at: `build/src/boondock_airband`

## Building on Raspberry Pi

### Prerequisites

On Raspberry Pi OS (Debian-based):

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libmp3lame-dev \
    libshout3-dev \
    libconfig++-dev \
    libfftw3-dev \
    libsoapysdr-dev \
    libpulse-dev \
    libusb-1.0-0-dev \
    pkg-config \
    git \
    librtlsdr-dev
```

### Build Options for Raspberry Pi

For Raspberry Pi, you have several platform options:

#### Option 1: Use GPU for FFT (Raspberry Pi 2/3 only - recommended for performance)

This uses the Broadcom VideoCore GPU for FFT calculations, which can significantly improve performance:

```bash
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATFORM=rpiv2 \
    -DNFM=ON \
    -DRTLSDR=ON
make -j$(nproc)
```

**Note:** This requires running as root due to GPU memory access.

#### Option 2: Native optimization (Recommended for Raspberry Pi 4/5)

Let the compiler optimize for your specific Raspberry Pi model:

```bash
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATFORM=native \
    -DNFM=ON \
    -DRTLSDR=ON
make -j$(nproc)
```

#### Option 3: Generic/Portable build

Build a portable binary (slower but works on all Pi models):

```bash
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATFORM=generic \
    -DNFM=ON \
    -DRTLSDR=ON
make -j$(nproc)
```

### Running on Raspberry Pi

1. **Install runtime dependencies:**
```bash
sudo apt-get install \
    libmp3lame0 \
    libshout3 \
    libconfig++9v5 \
    libfftw3-single3 \
    libsoapysdr0.8 \
    libpulse0 \
    libusb-1.0-0 \
    librtlsdr0
```

2. **Create configuration file:**
```bash
sudo cp config/basic_multichannel.conf /etc/boondock_airband.conf
sudo nano /etc/boondock_airband.conf
```

3. **Run the application:**
```bash
# Run directly from build directory
sudo ./build/src/boondock_airband -F -e -c /etc/boondock_airband.conf

# Or install system-wide
sudo cp build/src/boondock_airband /usr/local/bin/
sudo boondock_airband -F -e -c /etc/boondock_airband.conf
```

### Raspberry Pi Performance Tips

1. **For Raspberry Pi 2/3:** Use `-DPLATFORM=rpiv2` to leverage GPU acceleration
2. **For Raspberry Pi 4/5:** Use `-DPLATFORM=native` for best CPU optimization
3. **Reduce FFT size:** If experiencing performance issues, reduce `fft_size` in config (default 512, try 256)
4. **Limit channels:** Fewer simultaneous channels = better performance
5. **Use USB 3.0:** If using USB SDR dongles, connect to USB 3.0 ports on Pi 4/5

### Running as a Service on Raspberry Pi

1. **Copy the systemd service file:**
```bash
sudo cp init.d/rtl_airband.service /etc/systemd/system/boondock_airband.service
sudo nano /etc/systemd/system/boondock_airband.service
```

2. **Update the service file** to use `boondock_airband` instead of `rtl_airband`

3. **Enable and start the service:**
```bash
sudo systemctl daemon-reload
sudo systemctl enable boondock_airband
sudo systemctl start boondock_airband
sudo systemctl status boondock_airband
```

## Configuration

Configuration files use libconfig++ format. Example configurations are provided in the `config/` directory.

Default configuration file location: `/usr/local/etc/boondock_airband.conf`

## Command Line Options

```bash
boondock_airband [options]

Options:
  -c, --config FILE    Configuration file path (default: /usr/local/etc/boondock_airband.conf)
  -F, --foreground     Run in foreground (don't daemonize)
  -e, --stderr         Log to stderr instead of syslog
  -v, --version        Show version
  -h, --help           Show help
```

## Build Options

| Option | Description | Default |
|--------|-------------|---------|
| `-DNFM=ON` | Enable Narrow FM support | OFF |
| `-DRTLSDR=ON` | Enable RTL-SDR support | ON |
| `-DSOAPYSDR=ON` | Enable SoapySDR support | ON |
| `-DPULSEAUDIO=ON` | Enable PulseAudio output | ON |
| `-DPLATFORM=native` | Optimize for your CPU | native |
| `-DPLATFORM=rpiv2` | Use GPU FFT (Pi 2/3 only) | - |
| `-DPLATFORM=generic` | Portable binary | - |
| `-DCMAKE_BUILD_TYPE=Release` | Release build | Release |
| `-DCMAKE_BUILD_TYPE=Debug` | Debug build | - |

## Troubleshooting

### Raspberry Pi Specific Issues

1. **Permission denied errors:** Run with `sudo` when using GPU FFT (`rpiv2` platform)
2. **USB device not found:** Ensure USB device permissions are set correctly
3. **Performance issues:** 
   - Try reducing `fft_size` in configuration
   - Use `rpiv2` platform on Pi 2/3 for GPU acceleration
   - Use `native` platform on Pi 4/5
4. **Out of memory:** Reduce number of channels or FFT size

### General Issues

1. **Missing libraries:** Check with `ldd build/src/boondock_airband`
2. **Config errors:** Validate configuration file syntax
3. **USB device access:** May need to add user to appropriate groups or run with sudo

## License

Copyright (C) 2022-2025 charlie-foxtrot

Copyright (C) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>

Based on original work by Wong Man Hang <microtony@gmail.com>

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

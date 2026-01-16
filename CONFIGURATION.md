# Boondock Airband Configuration Reference

This document provides a comprehensive reference for all configuration options available in Boondock Airband, including their exact values, data types, default values, and behaviors.

## Table of Contents

1. [Global Configuration Options](#global-configuration-options)
2. [Device Configuration](#device-configuration)
3. [Channel Configuration](#channel-configuration)
4. [Output Configuration](#output-configuration)
5. [Mixer Configuration](#mixer-configuration)
6. [Input Device Types](#input-device-types)
   - [RTL-SDR](#rtl-sdr)
   - [SoapySDR](#soapysdr)
   - [MiriSDR](#mirisdr)
   - [File Input](#file-input)

---

## Global Configuration Options

These options are specified at the root level of the configuration file.

### `pidfile` (string, optional)
- **Description**: Path to the PID file where the process ID will be written
- **Default**: `/run/boondock_airband.pid`
- **Example**: `pidfile = "/var/run/boondock_airband.pid";`
- **Behavior**: The application writes its process ID to this file when running as a daemon. The file must be writable by the user running the application.

### `fft_size` (integer, optional)
- **Description**: Size of the FFT window used for frequency analysis
- **Default**: 512 (2^9)
- **Valid Values**: Must be a power of two in the range 256 (2^8) to 8192 (2^13)
- **Example**: `fft_size = 1024;`
- **Behavior**: Larger FFT sizes provide better frequency resolution but require more CPU. The value must be exactly a power of two. Invalid values cause the application to exit with an error.

### `shout_metadata_delay` (integer, optional)
- **Description**: Delay in FFT batches before sending metadata tags to Icecast streams
- **Default**: 3
- **Valid Range**: 0 to 32 (2 * TAG_QUEUE_LEN)
- **Example**: `shout_metadata_delay = 5;`
- **Behavior**: Controls when frequency change metadata is sent to Icecast. Higher values delay metadata updates but can prevent rapid switching issues.

### `localtime` (boolean, optional)
- **Description**: Use local time instead of UTC for timestamps
- **Default**: `false` (uses UTC)
- **Example**: `localtime = true;`
- **Behavior**: When enabled, all timestamps (file names, logs, etc.) use local timezone instead of UTC.

### `multiple_demod_threads` (boolean, optional)
- **Description**: Use separate demodulation thread for each device
- **Default**: `false` (single demodulation thread for all devices)
- **Example**: `multiple_demod_threads = true;`
- **Behavior**: When enabled, each device gets its own demodulation thread, which can improve performance on multi-core systems. **Note**: Not supported with BCM VideoCore FFT builds.

### `multiple_output_threads` (boolean, optional)
- **Description**: Use separate output thread for each device
- **Default**: `false` (single output thread for all devices)
- **Example**: `multiple_output_threads = true;`
- **Behavior**: When enabled, each device gets its own output processing thread, which can improve performance when using multiple devices.

### `log_scan_activity` (boolean, optional)
- **Description**: Log frequency changes during scan mode
- **Default**: `false`
- **Example**: `log_scan_activity = true;`
- **Behavior**: When enabled, logs a message each time the scanner detects activity on a new frequency in scan mode.

### `stats_filepath` (string, optional)
- **Description**: Path to file where statistics will be written
- **Default**: Not set (statistics disabled)
- **Example**: `stats_filepath = "/var/log/boondock_airband_stats.log";`
- **Behavior**: If specified, the application writes statistics to this file. The file must be writable.

### `tau` (integer, optional, NFM only)
- **Description**: Time constant for NFM de-emphasis filter in microseconds
- **Default**: 200 (0.2ms)
- **Example**: `tau = 300;`
- **Behavior**: Controls the de-emphasis filter time constant for NFM demodulation. Value of 0 disables the filter. Only available when compiled with NFM support.

### `devices` (array, required)
- **Description**: Array of device configurations
- **Type**: Array of device objects
- **Minimum**: At least one device must be defined
- **Example**: See [Device Configuration](#device-configuration) section
- **Behavior**: Each device represents an SDR receiver that will be used for capturing signals.

### `mixers` (object, optional)
- **Description**: Named mixer configurations
- **Type**: Object with named mixer entries
- **Example**: See [Mixer Configuration](#mixer-configuration) section
- **Behavior**: Mixers allow combining multiple channel outputs into a single stream.

---

## Device Configuration

Each device in the `devices` array can have the following options:

### `disable` (boolean, optional)
- **Description**: Disable this device
- **Default**: `false`
- **Example**: `disable = true;`
- **Behavior**: When set to `true`, the device is completely ignored during configuration parsing.

### `type` (string, required)
- **Description**: Type of input device
- **Valid Values**: 
  - `"rtlsdr"` - RTL-SDR USB dongle
  - `"soapysdr"` - SoapySDR-compatible device
  - `"mirisdr"` - MiriSDR device
  - `"file"` - File input (for testing/playback)
- **Example**: `type = "rtlsdr";`
- **Behavior**: Determines which input driver is used. If not specified and RTL-SDR support is compiled in, defaults to `"rtlsdr"` with a warning.

### `mode` (string, optional)
- **Description**: Operating mode for the device
- **Valid Values**: 
  - `"multichannel"` - Monitor multiple fixed frequencies simultaneously
  - `"scan"` - Scan through a list of frequencies, switching when activity is detected
- **Default**: `"multichannel"`
- **Example**: `mode = "scan";`
- **Behavior**: 
  - **multichannel**: All channels are monitored simultaneously. Requires `centerfreq` to be set.
  - **scan**: Only one channel is allowed. Device scans through frequencies in the channel's `freqs` list, switching when squelch opens.

### `sample_rate` (integer/float/string, optional)
- **Description**: Sample rate in samples per second
- **Default**: Device-specific (typically 2560000 for RTL-SDR)
- **Minimum**: Must be greater than WAVE_RATE (8000 for AM, 16000 for NFM)
- **Example**: `sample_rate = 2560000;` or `sample_rate = 2.56;` (MHz) or `sample_rate = "2.56M";`
- **Behavior**: Sets the SDR sample rate. Can be specified as integer Hz, float MHz, or string with unit suffix. Higher rates provide wider bandwidth but require more CPU.

### `centerfreq` (integer/float/string, required for multichannel mode)
- **Description**: Center frequency in Hz
- **Required**: Yes for `multichannel` mode, auto-set for `scan` mode
- **Example**: `centerfreq = 120000000;` or `centerfreq = 120.0;` (MHz) or `centerfreq = "120M";`
- **Behavior**: In multichannel mode, this is the center frequency around which all channels are monitored. In scan mode, this is automatically set based on the first frequency in the scan list.

### Device-Specific Options

See [Input Device Types](#input-device-types) section for device-specific configuration options.

### `channels` (array, required)
- **Description**: Array of channel configurations for this device
- **Type**: Array of channel objects
- **Minimum**: At least one channel must be defined
- **Example**: See [Channel Configuration](#channel-configuration) section
- **Behavior**: Each channel represents a frequency to monitor and its associated outputs.

---

## Channel Configuration

Each channel in a device's `channels` array can have the following options:

### `disable` (boolean, optional)
- **Description**: Disable this channel
- **Default**: `false`
- **Example**: `disable = true;`
- **Behavior**: When set to `true`, the channel is completely ignored during configuration parsing.

### Frequency Configuration

#### For Multichannel Mode:

##### `freq` (integer/float/string, required)
- **Description**: Frequency to monitor in Hz
- **Example**: `freq = 119500000;` or `freq = 119.5;` (MHz) or `freq = "119.5M";`
- **Behavior**: The exact frequency to monitor. Can be specified as integer Hz, float MHz, or string with unit suffix.

##### `label` (string, optional)
- **Description**: Human-readable label for this frequency
- **Example**: `label = "Tower";`
- **Behavior**: Used in metadata, file names (if `include_freq` is false), and web interface display.

#### For Scan Mode:

##### `freqs` (array, required)
- **Description**: Array of frequencies to scan through
- **Type**: Array of integers/floats/strings
- **Minimum**: At least one frequency must be specified
- **Example**: `freqs = (152.1, 168.25, 168.375);`
- **Behavior**: List of frequencies to scan. The device will switch between these frequencies when no signal is detected.

##### `labels` (array, optional)
- **Description**: Array of labels corresponding to each frequency in `freqs`
- **Type**: Array of strings
- **Length**: Must match the length of `freqs` if specified
- **Example**: `labels = ("Channel 1", "Channel 2", "Channel 3");`
- **Behavior**: Provides labels for each frequency in the scan list. If not specified, frequencies are labeled by their value.

### Audio Processing Options

#### `modulation` (string, optional)
- **Description**: Modulation type for the channel (multichannel) or all frequencies (scan)
- **Valid Values**: 
  - `"am"` - Amplitude Modulation (default)
  - `"nfm"` - Narrowband Frequency Modulation (requires NFM build)
- **Default**: `"am"`
- **Example**: `modulation = "nfm";`
- **Behavior**: Sets the demodulation method. NFM requires the application to be compiled with NFM support.

#### `modulations` (array, optional, scan mode only)
- **Description**: Array of modulation types for each frequency in scan mode
- **Type**: Array of strings
- **Length**: Must match the length of `freqs`
- **Valid Values**: Same as `modulation`
- **Example**: `modulations = ("am", "nfm", "am");`
- **Behavior**: Allows different modulation types for each frequency in scan mode. Cannot be used together with `modulation`.

#### `highpass` (integer, optional)
- **Description**: High-pass filter cutoff frequency in Hz
- **Default**: `100`
- **Example**: `highpass = 150;`
- **Behavior**: Filters out frequencies below this value. Must be less than or equal to `lowpass` if `lowpass` is set.

#### `lowpass` (integer, optional)
- **Description**: Low-pass filter cutoff frequency in Hz
- **Default**: `2500`
- **Example**: `lowpass = 3000;`
- **Behavior**: Filters out frequencies above this value. Must be greater than or equal to `highpass`.

### Squelch Configuration

#### `squelch_threshold` (integer or array, optional)
- **Description**: Squelch threshold in dBFS (decibels relative to full scale)
- **Type**: Integer or array of integers
- **Valid Range**: ≤ 0 (negative values or zero)
- **Default**: Auto squelch (0 disables manual threshold)
- **Example**: `squelch_threshold = -30;` or `squelch_threshold = (-30, -25, -35);` (scan mode)
- **Behavior**: 
  - **0**: Uses automatic squelch (recommended)
  - **Negative values**: Manual threshold in dBFS (more negative = more sensitive)
  - **Array (scan mode)**: Per-frequency thresholds
  - **Warning**: Both `squelch_threshold` and `squelch_snr_threshold` can be set but may conflict

#### `squelch_snr_threshold` (float/integer or array, optional)
- **Description**: Squelch threshold based on Signal-to-Noise Ratio (SNR) in dB
- **Type**: Float, integer, or array of floats/integers
- **Valid Range**: ≥ 0 (or -1 to disable for specific frequencies in arrays)
- **Default**: Not set (uses level-based squelch)
- **Example**: `squelch_snr_threshold = 6.0;` or `squelch_snr_threshold = (6.0, 5.0, 7.0);` (scan mode)
- **Behavior**: 
  - **Positive values**: Minimum SNR in dB required to open squelch
  - **-1**: Disables squelch for that frequency (array only)
  - **Array (scan mode)**: Per-frequency SNR thresholds
  - **Note**: More reliable than level-based squelch in noisy environments

#### `squelch` (deprecated)
- **Description**: Legacy squelch option (no longer supported)
- **Behavior**: If specified, a warning is issued and the option is ignored. Use `squelch_threshold` or `squelch_snr_threshold` instead.

### Filter Configuration

#### `notch` (float or array, optional)
- **Description**: Notch filter frequency in Hz to remove specific tones (e.g., CTCSS)
- **Type**: Float or array of floats
- **Valid Range**: > 0 (0 disables for specific frequencies in arrays)
- **Example**: `notch = 100.0;` or `notch = (100.0, 0, 150.0);` (scan mode)
- **Behavior**: 
  - **Positive values**: Frequency in Hz to filter out (typically CTCSS tones)
  - **0**: Disables notch for that frequency (array only)
  - **Array (scan mode)**: Per-frequency notch settings

#### `notch_q` (float or array, optional)
- **Description**: Quality factor (Q) for the notch filter
- **Type**: Float or array of floats (must match `notch` type)
- **Default**: `10.0`
- **Valid Range**: > 0.0
- **Example**: `notch_q = 15.0;`
- **Behavior**: Higher Q values create a narrower, more selective notch filter. Must be the same type (scalar or array) as `notch`.

#### `bandwidth` (integer/float/string or array, optional)
- **Description**: Bandwidth filter in Hz
- **Type**: Integer/float/string or array
- **Valid Range**: > 0 (0 disables for specific frequencies in arrays)
- **Example**: `bandwidth = 6000;` or `bandwidth = (6000, 0, 8000);` (scan mode)
- **Behavior**: 
  - **Positive values**: Bandwidth in Hz. A lowpass filter is applied at bandwidth/2
  - **0**: Disables bandwidth filter for that frequency (array only)
  - **Array (scan mode)**: Per-frequency bandwidth settings
  - **Note**: Enables raw I/Q processing (required for NFM)

### CTCSS Configuration

#### `ctcss` (float or array, optional)
- **Description**: CTCSS (Continuous Tone-Coded Squelch System) tone frequency in Hz
- **Type**: Float or array of floats
- **Valid Range**: > 0 (0 disables for specific frequencies in arrays)
- **Example**: `ctcss = 100.0;` or `ctcss = (100.0, 0, 150.0);` (scan mode)
- **Behavior**: 
  - **Positive values**: CTCSS tone frequency in Hz (common values: 67.0, 100.0, 150.0, etc.)
  - **0**: Disables CTCSS detection for that frequency (array only)
  - **Array (scan mode)**: Per-frequency CTCSS settings
  - Squelch only opens when the specified CTCSS tone is detected

### Audio Level Configuration

#### `ampfactor` (float or array, optional)
- **Description**: Amplification factor for audio output
- **Type**: Float or array of floats
- **Default**: `1.0`
- **Valid Range**: ≥ 0.0
- **Example**: `ampfactor = 1.5;` or `ampfactor = (1.0, 1.5, 0.8);` (scan mode)
- **Behavior**: 
  - **1.0**: No amplification (default)
  - **> 1.0**: Amplifies audio
  - **< 1.0**: Attenuates audio
  - **Array (scan mode)**: Per-frequency amplification

#### `afc` (integer, optional)
- **Description**: Automatic Frequency Control level
- **Type**: Integer (0-255)
- **Default**: `0` (disabled)
- **Example**: `afc = 2;`
- **Behavior**: 
  - **0**: AFC disabled
  - **1-255**: AFC aggressiveness (higher = more aggressive)
  - Automatically adjusts frequency to track signal drift

#### `tau` (integer, optional, NFM only, channel-level)
- **Description**: Time constant for NFM de-emphasis filter in microseconds (channel-specific override)
- **Type**: Integer
- **Default**: Device-level `tau` or 200
- **Example**: `tau = 300;`
- **Behavior**: Overrides device-level `tau` for this specific channel. Only available when compiled with NFM support.

### Output Configuration

#### `outputs` (array, required)
- **Description**: Array of output configurations for this channel
- **Type**: Array of output objects
- **Minimum**: At least one output must be defined
- **Example**: See [Output Configuration](#output-configuration) section
- **Behavior**: Each output defines where the demodulated audio is sent (file, Icecast, UDP, mixer, etc.).

---

## Output Configuration

Each output in a channel's `outputs` array can have the following options:

### `disable` (boolean, optional)
- **Description**: Disable this output
- **Default**: `false`
- **Example**: `disable = true;`
- **Behavior**: When set to `true`, the output is completely ignored during configuration parsing.

### `type` (string, required)
- **Description**: Type of output
- **Valid Values**: 
  - `"icecast"` - Icecast streaming server
  - `"file"` - MP3 file recording
  - `"rawfile"` - Raw I/Q file recording (CF32 format)
  - `"udp_stream"` - UDP audio stream
  - `"mixer"` - Mixer input (device channels only)
  - `"pulse"` - PulseAudio output (if compiled with PulseAudio support)
- **Example**: `type = "icecast";`
- **Behavior**: Determines the output destination and required parameters.

### Common Output Options

#### `continuous` (boolean, optional)
- **Description**: Keep output active even when squelch is closed
- **Default**: `false`
- **Valid For**: `file`, `rawfile`, `udp_stream`, `pulse`
- **Example**: `continuous = true;`
- **Behavior**: When `true`, output continues even when no signal is detected. Cannot be used with `split_on_transmission`.

### Icecast Output (`type = "icecast"`)

#### `server` (string, required)
- **Description**: Icecast server hostname or IP address
- **Example**: `server = "icecast.example.com";`

#### `port` (integer, required)
- **Description**: Icecast server port number
- **Example**: `port = 8080;`

#### `mountpoint` (string, required)
- **Description**: Icecast mount point path
- **Example**: `mountpoint = "/stream.mp3";`

#### `username` (string, required)
- **Description**: Icecast source username
- **Example**: `username = "source";`

#### `password` (string, required)
- **Description**: Icecast source password
- **Example**: `password = "mypassword";`

#### `name` (string, optional)
- **Description**: Stream name (metadata)
- **Example**: `name = "Tower Frequency";`

#### `genre` (string, optional)
- **Description**: Stream genre (metadata)
- **Example**: `genre = "ATC";`

#### `description` (string, optional)
- **Description**: Stream description (metadata)
- **Example**: `description = "Local airport tower frequency";`

#### `send_scan_freq_tags` (boolean, optional)
- **Description**: Send frequency change metadata during scanning
- **Default**: `false`
- **Example**: `send_scan_freq_tags = true;`
- **Behavior**: When enabled in scan mode, sends metadata updates when switching frequencies.

#### `tls` (string, optional, requires TLS support)
- **Description**: TLS encryption mode
- **Valid Values**: 
  - `"auto"` - Automatically use TLS if supported
  - `"auto_no_plain"` - Use TLS only, fail if not supported
  - `"transport"` - TLS transport encryption (RFC 2818)
  - `"upgrade"` - TLS upgrade (RFC 2817)
  - `"disabled"` - No TLS (default)
- **Default**: `"disabled"`
- **Example**: `tls = "auto";`
- **Behavior**: Only available if libshout was compiled with TLS support.

### File Output (`type = "file"`)

#### `directory` (string, required)
- **Description**: Directory where MP3 files will be saved
- **Example**: `directory = "/home/user/recordings";`
- **Behavior**: Directory must exist and be writable.

#### `filename_template` (string, required)
- **Description**: Base filename template for recordings
- **Example**: `filename_template = "tower";`
- **Behavior**: Actual filename will be: `{template}_{date}_{time}.mp3` or `{template}_{freq}_{date}_{time}.mp3` if `include_freq` is true.

#### `dated_subdirectories` (boolean, optional)
- **Description**: Create date-based subdirectories (YYYY-MM-DD)
- **Default**: `false`
- **Example**: `dated_subdirectories = true;`
- **Behavior**: When enabled, files are organized into daily subdirectories.

#### `append` (boolean, optional)
- **Description**: Append to existing file instead of creating new files
- **Default**: `true`
- **Example**: `append = false;`
- **Behavior**: When `false`, creates new file for each transmission. When `true` (default), appends to existing file.

#### `split_on_transmission` (boolean, optional)
- **Description**: Create a new file for each transmission
- **Default**: `false`
- **Example**: `split_on_transmission = true;`
- **Behavior**: 
  - Creates a new file each time squelch opens
  - Cannot be used with `continuous = true`
  - Not allowed for mixer outputs

#### `include_freq` (boolean, optional)
- **Description**: Include frequency in filename
- **Default**: `false`
- **Example**: `include_freq = true;`
- **Behavior**: When enabled, frequency (in MHz) is included in the filename, useful for scan mode.

### Raw File Output (`type = "rawfile"`)

#### `directory` (string, required)
- **Description**: Directory where raw I/Q files will be saved
- **Example**: `directory = "/home/user/raw_recordings";`

#### `filename_template` (string, required)
- **Description**: Base filename template for raw recordings
- **Example**: `filename_template = "iq_capture";`

#### `dated_subdirectories` (boolean, optional)
- **Description**: Create date-based subdirectories
- **Default**: `false`

#### `append` (boolean, optional)
- **Description**: Append to existing file
- **Default**: `true`

#### `split_on_transmission` (boolean, optional)
- **Description**: Create new file for each transmission
- **Default**: `false`
- **Behavior**: Cannot be used with `continuous = true`. Not allowed for mixer outputs.

#### `include_freq` (boolean, optional)
- **Description**: Include frequency in filename
- **Default**: `false`

**Note**: Raw file output saves complex float 32-bit I/Q samples (CF32 format) and requires `bandwidth` to be set on the channel.

### UDP Stream Output (`type = "udp_stream"`)

#### `dest_address` (string, required)
- **Description**: Destination IP address or hostname
- **Example**: `dest_address = "192.168.1.100";`

#### `dest_port` (integer/string, required)
- **Description**: Destination UDP port
- **Example**: `dest_port = 6001;` or `dest_port = "6001";`

#### `continuous` (boolean, optional)
- **Description**: Keep streaming even when squelch is closed
- **Default**: `false`

### Mixer Output (`type = "mixer"`)

#### `name` (string, required)
- **Description**: Name of the mixer to connect to
- **Example**: `name = "big_mixer";`
- **Behavior**: Must match a mixer name defined in the `mixers` section. Not allowed for mixer outputs (mixers cannot output to other mixers).

#### `ampfactor` (float, optional)
- **Description**: Amplification factor for this mixer input
- **Default**: `1.0`
- **Example**: `ampfactor = 0.8;`

#### `balance` (float, optional)
- **Description**: Stereo balance for this mixer input
- **Default**: `0.0`
- **Valid Range**: -1.0 to 1.0
- **Example**: `balance = -0.5;` (left) or `balance = 0.5;` (right)
- **Behavior**: 
  - **-1.0**: Full left
  - **0.0**: Center (mono)
  - **1.0**: Full right

### PulseAudio Output (`type = "pulse"`, requires PulseAudio support)

#### `server` (string, optional)
- **Description**: PulseAudio server address
- **Example**: `server = "192.168.1.10";`
- **Behavior**: If not specified, uses default PulseAudio server.

#### `name` (string, optional)
- **Description**: Client name
- **Default**: `"boondock_airband"`

#### `sink` (string, optional)
- **Description**: PulseAudio sink name
- **Example**: `sink = "alsa_output.usb-...";`

#### `stream_name` (string, required for mixers, optional for channels)
- **Description**: Stream name for PulseAudio
- **Example**: `stream_name = "Tower Frequency";`
- **Behavior**: 
  - **Required** for mixer outputs
  - **Optional** for device channels (defaults to frequency in MHz if not specified)

#### `continuous` (boolean, optional)
- **Description**: Keep streaming even when squelch is closed
- **Default**: `false`

---

## Mixer Configuration

Mixers combine multiple channel outputs into a single stream. Defined in the `mixers` object with named entries.

### Mixer Structure

```
mixers: {
  mixer_name: {
    highpass = 100;
    lowpass = 2500;
    outputs: (
      { type = "icecast"; ... },
      { type = "file"; ... }
    );
  }
};
```

### Mixer Options

#### `highpass` (integer, optional)
- **Description**: High-pass filter cutoff frequency in Hz
- **Default**: `100`
- **Example**: `highpass = 150;`
- **Behavior**: Must be less than or equal to `lowpass`.

#### `lowpass` (integer, optional)
- **Description**: Low-pass filter cutoff frequency in Hz
- **Default**: `2500`
- **Example**: `lowpass = 3000;`
- **Behavior**: Must be greater than or equal to `highpass`.

#### `outputs` (array, required)
- **Description**: Array of output configurations for the mixer
- **Type**: Array of output objects
- **Valid Output Types**: `icecast`, `file`, `udp_stream`, `pulse`
- **Invalid Output Types**: `mixer`, `rawfile` (not allowed for mixers)
- **Example**: See [Output Configuration](#output-configuration) section
- **Behavior**: Mixer outputs receive the combined audio from all connected channels.

### Connecting Channels to Mixers

Channels connect to mixers using a mixer output type:

```
outputs: (
  {
    type = "mixer";
    name = "mixer_name";
    ampfactor = 1.0;  // optional
    balance = 0.0;    // optional
  }
);
```

---

## Input Device Types

### RTL-SDR

Device type: `type = "rtlsdr";`

#### `index` (integer, optional)
- **Description**: USB device index (0-based)
- **Required**: Yes, if `serial` is not specified
- **Example**: `index = 0;`
- **Behavior**: Use when you have multiple RTL-SDR devices. Index can change when devices are reconnected.

#### `serial` (string, optional)
- **Description**: RTL-SDR device serial number
- **Required**: Yes, if `index` is not specified
- **Example**: `serial = "00000001";`
- **Behavior**: More reliable than index as it doesn't change when devices are reconnected. Use `rtl_test` or `rtl_eeprom` to find serial numbers.

#### `gain` (integer/float, required)
- **Description**: RF gain in dB
- **Type**: Integer or float
- **Example**: `gain = 25;` or `gain = 25.0;`
- **Behavior**: 
  - Integer values are interpreted as dB (e.g., `25` = 25 dB)
  - Float values are also interpreted as dB
  - Typical range: 0-49.6 dB in 0.1 dB steps

#### `correction` (integer, optional)
- **Description**: PPM (parts per million) frequency correction
- **Default**: `0`
- **Example**: `correction = 80;`
- **Behavior**: Compensates for crystal oscillator drift. Use `kalibrate-rtl` or similar tools to determine the correction value.

#### `buffers` (integer, optional)
- **Description**: Number of USB buffers
- **Default**: Device-specific (typically 15)
- **Valid Range**: > 0
- **Example**: `buffers = 20;`
- **Behavior**: More buffers can reduce dropouts but use more memory. Adjust if experiencing buffer underruns.

### SoapySDR

Device type: `type = "soapysdr";`

#### `device_string` (string, required)
- **Description**: SoapySDR device string
- **Example**: `device_string = "driver=airspy";` or `device_string = "driver=rtlsdr,serial=00000001";`
- **Behavior**: Use `SoapySDRUtil --find` to discover available devices and their strings.

#### `gain` (integer/float/string, optional)
- **Description**: RF gain setting
- **Type**: Integer, float, or string
- **Default**: AGC enabled if not specified
- **Example**: 
  - `gain = 25.0;` (simple numeric gain)
  - `gain = "LNA=12,MIX=10,VGA=10";` (component-specific gains)
- **Behavior**: 
  - **Numeric**: Sets overall gain in dB (disables AGC)
  - **String**: Component-specific gains in format `"name1=value1,name2=value2,..."`
  - **Not specified**: AGC is enabled
  - Use `SoapySDRUtil --args="device_string" --gain` to see available gain components

#### `correction` (integer/float, optional)
- **Description**: PPM frequency correction
- **Default**: `0`
- **Example**: `correction = 43.5;`

#### `channel` (integer, optional)
- **Description**: RF channel index
- **Default**: `0`
- **Example**: `channel = 1;`
- **Behavior**: For devices with multiple RF channels.

#### `antenna` (string, optional)
- **Description**: Antenna selection
- **Example**: `antenna = "RX";`
- **Behavior**: Use `SoapySDRUtil --args="device_string" --antenna` to see available antennas.

#### `sample_rate` (integer/float/string, optional)
- **Description**: Sample rate in Hz
- **Example**: `sample_rate = 10000000;` or `sample_rate = 10.0;` (MHz)
- **Behavior**: If not specified, the driver selects a suitable rate. Use `SoapySDRUtil --args="device_string" --rate` to see supported rates.

### MiriSDR

Device type: `type = "mirisdr";`

#### `index` (integer, optional)
- **Description**: USB device index (0-based)
- **Required**: Yes, if `serial` is not specified
- **Example**: `index = 0;`

#### `serial` (string, optional)
- **Description**: MiriSDR device serial number
- **Required**: Yes, if `index` is not specified
- **Example**: `serial = "00000001";`

#### `gain` (integer/float, required)
- **Description**: RF gain in dB
- **Example**: `gain = 25;`

#### `correction` (integer, optional)
- **Description**: PPM frequency correction
- **Default**: `0`
- **Example**: `correction = 80;`

### File Input

Device type: `type = "file";`

#### `filepath` (string, required)
- **Description**: Path to input file
- **Example**: `filepath = "/path/to/recording.cf32";`
- **Behavior**: File must contain I/Q samples in the format specified by the device's sample format.

#### `speedup_factor` (integer/float, optional)
- **Description**: Playback speed multiplier
- **Default**: `4.0`
- **Valid Range**: > 0.0
- **Example**: `speedup_factor = 2.0;`
- **Behavior**: 
  - **1.0**: Real-time playback
  - **> 1.0**: Faster than real-time (useful for testing)
  - **< 1.0**: Slower than real-time

---

## Configuration File Format

The configuration file uses libconfig format, which is similar to JSON but more flexible:

- Uses `=` for assignment
- Uses `;` to terminate statements
- Supports comments with `#`
- Arrays use parentheses: `(value1, value2, value3)`
- Objects use braces: `{ key = value; }`
- Strings can be quoted or unquoted (if they don't contain special characters)

### Example Configuration

```libconfig
# Global options
pidfile = "/var/run/boondock_airband.pid";
fft_size = 1024;
localtime = true;

# Device configuration
devices:
({
  type = "rtlsdr";
  serial = "00000001";
  gain = 25;
  correction = 80;
  centerfreq = 120.0;
  mode = "multichannel";
  
  channels:
  (
    {
      freq = 119.5;
      label = "Tower";
      modulation = "am";
      highpass = 100;
      lowpass = 2500;
      squelch_snr_threshold = 6.0;
      outputs: (
        {
          type = "icecast";
          server = "icecast.example.com";
          port = 8080;
          mountpoint = "/tower.mp3";
          username = "source";
          password = "password";
          name = "Tower Frequency";
          genre = "ATC";
        },
        {
          type = "file";
          directory = "/recordings";
          filename_template = "tower";
          split_on_transmission = true;
        }
      );
    }
  );
});

# Mixer configuration
mixers: {
  combined: {
    highpass = 100;
    lowpass = 2500;
    outputs: (
      {
        type = "icecast";
        server = "icecast.example.com";
        port = 8080;
        mountpoint = "/combined.mp3";
        username = "source";
        password = "password";
      }
    );
  }
};
```

---

## Validation Rules

### Device-Level Validation

1. At least one device must be defined
2. `type` must be a supported device type
3. `mode` must be `"multichannel"` or `"scan"`
4. `centerfreq` is required for `multichannel` mode
5. `sample_rate` must be greater than WAVE_RATE (8000 for AM, 16000 for NFM)
6. In `scan` mode, only one channel is allowed

### Channel-Level Validation

1. At least one channel must be defined per device
2. At least one output must be defined per channel
3. For `multichannel` mode: `freq` is required
4. For `scan` mode: `freqs` array is required with at least one frequency
5. `lowpass` must be ≥ `highpass` if both are set
6. `squelch_threshold` must be ≤ 0
7. `squelch_snr_threshold` must be ≥ 0 (or -1 in arrays to disable)
8. `ampfactor` must be ≥ 0
9. `balance` must be in range -1.0 to 1.0
10. `notch_q` must be > 0.0
11. `bandwidth` must be > 0 (or 0 in arrays to disable)
12. `ctcss` must be > 0 (or 0 in arrays to disable)
13. Array lengths must match when using per-frequency settings in scan mode

### Output-Level Validation

1. `type` must be a supported output type
2. Icecast outputs require: `server`, `port`, `mountpoint`, `username`, `password`
3. File outputs require: `directory`, `filename_template`
4. UDP outputs require: `dest_address`, `dest_port`
5. Mixer outputs require: `name` (must match a defined mixer)
6. `continuous` and `split_on_transmission` cannot both be `true`
7. `split_on_transmission` is not allowed for mixer outputs
8. `rawfile` output is not allowed for mixer outputs
9. `mixer` output is not allowed for mixer outputs

### Mixer-Level Validation

1. Mixer name must be unique
2. At least one output must be defined
3. `lowpass` must be ≥ `highpass`

---

## Notes

1. **Frequency Formats**: Frequencies can be specified as:
   - Integer Hz: `119500000`
   - Float MHz: `119.5`
   - String with unit: `"119.5M"` or `"119500000"`

2. **Array vs Scalar**: In scan mode, many channel options support both scalar (applies to all frequencies) and array (per-frequency) formats. Arrays must have the same length as the `freqs` array.

3. **Default Values**: Options marked as "optional" have sensible defaults. Required options will cause configuration errors if missing.

4. **Build Options**: Some features (NFM, PulseAudio, TLS) require the application to be compiled with specific support. Check your build configuration.

5. **Performance**: 
   - Larger `fft_size` provides better frequency resolution but uses more CPU
   - `multiple_demod_threads` and `multiple_output_threads` can improve performance on multi-core systems
   - Higher `sample_rate` provides wider bandwidth but requires more processing

6. **Squelch**: 
   - `squelch_threshold` (level-based) is simpler but less reliable in noisy environments
   - `squelch_snr_threshold` (SNR-based) is more reliable but requires good signal conditions
   - Setting both may cause conflicts; prefer one method

7. **File Outputs**: 
   - Files are created when squelch opens (unless `continuous = true`)
   - With `split_on_transmission = true`, each transmission gets its own file
   - With `append = true` (default), all transmissions append to the same file
   - Directory must exist and be writable

8. **Mixers**: 
   - Mixers combine multiple channel outputs into a single stream
   - Useful for creating combined feeds from multiple frequencies
   - Mixer outputs can go to Icecast, file, UDP, or PulseAudio (not to other mixers)

---

## Error Messages

Common configuration errors and their meanings:

- **"no devices defined"**: The `devices` array is empty or missing
- **"no channels configured"**: A device has no channels defined
- **"no outputs defined"**: A channel has no outputs defined
- **"invalid mode"**: Device mode is not `"multichannel"` or `"scan"`
- **"lowpass must be greater than or equal to highpass"**: Filter settings are invalid
- **"squelch_threshold must be less than or equal to 0"**: Invalid squelch threshold value
- **"unknown output type"**: Output type is not recognized
- **"unknown mixer"**: Mixer name in output doesn't match any defined mixer
- **"can't have both continuous and split_on_transmission"**: Conflicting file output options

---

**Copyright © 2026 Boondock Technologies**

For more information, visit: https://www.boondockecho.com

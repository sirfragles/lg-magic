# LG Magic Remote (MR20) Linux Kernel Driver and Tools
**Language:** **English🇬🇧** [Русский🇷🇺](README.ru.md)

![LG Magic Remote](images/lg_magic_remote.png)

## Overview

This project provides a comprehensive Linux kernel driver and toolset for the LG Magic Remote (MR20 and similar models). The driver enables full functionality of the remote including button mapping, gyroscopic airmouse control, and IMU data access. The package includes a kernel module (DKMS) and the `lg-magic` tools — a single C binary (no Python runtime dependencies) with an IMU reader, HID report analyzer, calibration utilities and the interactive `lg-magic setup` wizard that calibrates the remote end to end. The original Python scripts are kept under `scripts/` as a reference.

## Project Structure

### Kernel Module Files

- **dkms.conf** - DKMS configuration (at the repo root; DKMS builds from the source root)
- **kernel/lg_magic_main.c** - Main kernel driver implementation
- **kernel/lg_magic_airmouse.h** - Header file for airmouse calibration structures
- **kernel/lg_magic_airmouse.c** - Airmouse calibration and filtering implementation
- **kernel/Makefile** - Build system for the kernel module
- **include/lg_magic_calib.h** - `struct lg_magic_airmouse_calib`, shared verbatim between the kernel and the tools
- **51-lgimu.rules** - Udev rules for the IMU evdev device, the hidraw node and `/dev/uinput`. Installed to `/etc/udev/rules.d/` by `make install` / the .deb

### C Tools (`tools/`)

- **`lg-magic`** - single multi-call binary, `make tools` (only libc/libm):
  - `lg-magic analyze` - HIDRAW packet analyzer (like `lg_magic.py`, with auto-detection by VID/PID)
  - `lg-magic imu` - IMU reader: raw data, `--csv` recording, `--ahrs` orientation angles, `--cube` ANSI terminal cube, `--mouse` uinput airmouse
  - `lg-magic calibrate` - accelerometer (Levenberg-Marquardt) / gyroscope calibration from a CSV recording
  - `lg-magic calib2bin` - JSON calibration to the 32-byte kernel firmware blob
  - `lg-magic config` - show / change the configuration (JSON files, see below)
  - `lg-magic setup` - interactive wizard: module parameters, calibration, firmware blob, airmouse test

### Python Tools (deprecated reference)

The Python scripts are kept for reference and for regenerating the golden test data (`testdata/`). The C tools replicate their output byte for byte (see TESTING.md).

- **scripts/lg_magic.py** - HIDRAW-level packet analyzer and debug tool. Initial tool, kept for historical reasons
- **scripts/calibrate.py** - IMU calibration utility (accelerometer and gyroscope)
- **scripts/convert_calib.py** - Converts JSON calibration to binary format for kernel module
- **scripts/display_imu.py** - Real-time IMU data visualization and airmouse emulation

## Kernel Module Features

### Device Support
- Supports LG Magic Remote (HID Bluetooth device 000f:3412)
- Creates two input devices:
  - `LG Magic Remote` - Standard HID events (buttons, wheel, airmouse)
  - `LG Magic Remote IMU` - Raw IMU data (accelerometer + gyroscope)

### Button Mapping
Comprehensive button support including:
- Power, number keys (0-9), navigation buttons (UP/DOWN/LEFT/RIGHT)
- Media controls (PLAY, PAUSE, VOLUME, MUTE)
- Color buttons (RED, GREEN, YELLOW, BLUE)
- Special function buttons (HOME, BACK, SETTINGS, GUIDE)

### Airmouse Functionality
**Needs calibration before usage**
- Gyroscope-based pointer control
- Configurable sensitivity and threshold
- Low-pass filtering for smooth movement
- Automatic mode switching between navigation and pointer control

The airmouse feature is implemented in two ways:
- **Kernel Module (Production)**: Built-in airmouse processing with minimal latency, running entirely in kernel space
- **Python + Uinput (Debug)**: Raw IMU data processing in userspace via `display_imu.py --mouse` for testing and calibration validation

### IMU Data Access
- Raw accelerometer and gyroscope data via evdev
- 6-axis motion data (3-axis accel + 3-axis gyro)
- Hardware counter for timing synchronization

## Building and Installation

### Prerequisites
- Linux kernel headers
- DKMS (Dynamic Kernel Module Support)
- Build essentials (make, gcc)

### Manual Build
```bash
make
sudo insmod lg_magic.ko
```

### Debian Package (recommended)
```bash
# Build the .deb (needs devscripts, dh-dkms, debhelper)
dpkg-buildpackage -us -uc -b
sudo apt install ../lg-magic-dkms_1.0-1_amd64.deb
# The package builds/installs the module via DKMS and installs lg-magic.
# Finish the setup:
sudo lg-magic setup
```

**Releases.** GitHub Actions builds the `.deb` on every version tag
(`v*`) and attaches it to the GitHub Release for that tag. CI also runs
on every push and pull request: it builds the module and tools, runs the
full test suite, and packages the `.deb` (downloadable as a workflow
artifact).

### DKMS Installation (manual)
DKMS builds from the source root, so copy only what the module build needs:
```bash
sudo mkdir /usr/src/lg-magic-1.0
sudo cp Makefile dkms.conf COPYING /usr/src/lg-magic-1.0/
sudo cp -r kernel include /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
# The tools are not built by DKMS - build and install them separately:
make tools
sudo make install
```

### Module Parameters
The driver supports several runtime parameters:

```bash
# Load with custom parameters
sudo modprobe lg_magic airmouse=1 airmouse_threshold=300 imu_evdev=1 debug=2

# Or set via sysfs after loading
echo 1 > /sys/module/lg_magic/parameters/airmouse
echo 500 > /sys/module/lg_magic/parameters/airmouse_threshold
echo 2 > /sys/module/lg_magic/parameters/debug
```

**Parameters:**
- `airmouse` (0/1): Enable/disable airmouse functionality
- `airmouse_threshold` (int): Gyro threshold for enabling airmouse (default: 300)
- `imu_evdev` (0/1): Expose raw IMU data as separate input device
- `debug` (0-2): Debug message level (0=quiet, 1=normal, 2=verbose)

## Calibration System

### Calibration File Format
The driver loads calibration data from binary files via Linux Firmware subsystem:
- `lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` - MAC address-specific individual calibration
- `lg_magic_calib.bin` - Fallback calibration

### Creating Calibration Files

The easiest way is the wizard — it records, fits, writes the JSON and the
firmware blob, and checks that the kernel loaded it:
```bash
sudo lg-magic setup
```

Or do it manually with the C tools:

1. **Collect IMU samples for both calibrations:**
```bash
lg-magic imu --csv samples.csv
```

2. **Calculate calibration values:**
```bash
# Calibrate accelerometer (slowly rotate across all axes while collecting)
lg-magic calibrate samples.csv calib_accel.json --accel

# Calibrate gyroscope (keep remote stationary while collecting)
lg-magic calibrate samples.csv calib_gyro.json --gyro

# Combine Gyro/Accel JSONs
Combine gyro/accel sections. Adjust gyro scale. Out of scope of this project, recommended value about 0.07

```

3. **Convert to binary format:**
```bash
lg-magic calib2bin calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5
sudo cp lg_magic_calib.bin /lib/firmware/
```

### Calibration Parameters
- `alpha`: Low-pass filter coefficient (0.0-1.0)
- `mouse_k`: Airmouse sensitivity multiplier
- `gyro_bias`: Gyroscope zero-offset values
- `gyro_scale`: Gyroscope scaling factors

## lg-magic Tools Usage

All tools live in one binary. Run `lg-magic --help` or `lg-magic <cmd> --help`.

```bash
# HIDRAW packet analyzer (auto-detects the remote by VID/PID 000f:3412)
lg-magic analyze                       # or: lg-magic analyze --device /dev/hidrawN

# Raw IMU data display (auto-detects the "IMU" evdev device)
lg-magic imu

# Record raw samples (for calibration)
lg-magic imu --csv samples.csv

# Orientation angles (Madgwick AHRS)
lg-magic imu --calib calib.json --ahrs

# 3D orientation cube in the terminal (ANSI, no GPU libraries)
lg-magic imu --calib calib.json --cube

# Uinput airmouse (needs /dev/uinput access - see the udev rule)
lg-magic imu --calib calib.json --mouse

# Configuration (precedence: defaults < /etc/lg-magic/config.json <
# ~/.config/lg-magic/config.json < --config FILE < CLI flags)
lg-magic config                        # show the effective configuration
lg-magic config set mouse_k 0.5        # save into ~/.config/lg-magic/config.json
lg-magic config path                   # config file locations

# Setup wizard: module parameters, calibration, firmware blob, airmouse test
sudo lg-magic setup                    # --non-interactive accepts the defaults
```

Deliberate fixes over the Python scripts (all documented in TESTING.md):
`lg-magic analyze` auto-detects the remote instead of a hardcoded
`/dev/hidraw7`; `--cube` implies `--ahrs` (the Python `--cube` alone showed
a static cube); `--gyro` calibrations write an identity accelerometer
correction instead of empty arrays (empty arrays broke `--ahrs`).

## Python Tools Usage (deprecated)

### lg_magic.py - Packet Analyzer
```bash
# Monitor HIDRAW packets
python3 lg_magic.py
```

### display_imu.py - IMU Visualization
```bash
# Raw IMU data display
python3 display_imu.py

# AHRS
python3 display_imu.py --calib calib.json --ahrs

# 3D orientation cube
python3 display_imu.py --calib calib.json --cube

# Uinput airmouse
python3 display_imu.py --calib calib.json --mouse
```

### Airmouse debug
The Python tools can create a virtual mouse device using uinput:

```bash
# Enable uinput module
sudo modprobe uinput

# Run airmouse with calibration
python3 display_imu.py --calib calib.json --mouse
```

## Technical Details

### HID Protocol Structure

The remote uses report ID `0xFD` (30-byte total: 1 byte report ID + 29 bytes payload). The payload structure is:

| Offset | Size | Description | Format |
|--------|------|-------------|---------|
| 0 | 1 | Report ID (0xFD) | uint8_t |
| 1-2 | 2 | Packet counter | little-endian uint16 |
| 3-4 | 2 | Constant value (0xFD00) | little-endian uint16 |
| 5-6 | 2 | Gyro X | big-endian int16 |
| 7-8 | 2 | Gyro Y | big-endian int16 |
| 9-10 | 2 | Gyro Z | big-endian int16 |
| 11-12 | 2 | Accel X | big-endian int16 |
| 13-14 | 2 | Accel Y | big-endian int16 |
| 15-16 | 2 | Accel Z | big-endian int16 |
| 17-18 | 2 | Button code | big-endian uint16 |
| 19 | 1 | Wheel delta | int8 |

Other report types (0xF9, 0x01) were not observed, maybe used for other functions (like MIC)

### IMU Data Processing
- **Sampling rate**: ~50Hz (20ms intervals)
- **Data format**: Big-endian signed 16-bit values
- **Coordinate system**: Shown on image

### Airmouse Algorithm
1. Gyro data is bias-corrected and scaled
2. Low-pass filtering reduces high-frequency noise
3. Angular velocity is converted to pointer movement
4. Threshold detection switches between navigation and pointer modes. Pressing navigation buttons return to button mode.

## Filesystem Locations

- **Module**: `/lib/modules/$(uname -r)/kernel/drivers/input/misc/lg_magic.ko`
- **Calibration**: `/lib/firmware/lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` (per-MAC) and `/lib/firmware/lg_magic_calib.bin` (fallback)
- **Calibration JSON (tools)**: `/etc/lg-magic/calib.json`
- **Module parameters**: `/etc/modprobe.d/lg-magic.conf`
- **Config**: `/etc/lg-magic/config.json` and `~/.config/lg-magic/config.json`
- **Binary**: `/usr/bin/lg-magic`
- **Udev rule**: `/etc/udev/rules.d/51-lgimu.rules`
- **DKMS source**: `/usr/src/lg-magic-1.0/`

## Compatibility

- **Tested with**: LG Magic Remote MR20
- **Kernel versions**: 4.15+ (tested on 6.11)
- **lg-magic tools**: Linux with libc/libm only (no Python, no GUI libraries)
- **Python scripts** (deprecated): Python 3.6+, numpy, scipy, pyqtgraph, python-evdev

## Contributing

Please report issues and submit pull requests for:
- Additional device support
- Improved calibration algorithms
- Bug fixes and performance improvements

## License

GPL v2 - Same as Linux kernel

Copyright © 2025 [Ilya Chelyadin]. This project is not affiliated with LG Electronics.

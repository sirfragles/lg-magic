# LG Magic Remote (MR20) Linux Kernel Driver and Tools
**Language:** **English🇬🇧** [Русский🇷🇺](README.ru.md)

![LG Magic Remote](images/lg_magic_remote.png)

## Overview

This project provides a comprehensive Linux kernel driver and toolset for the LG Magic Remote (MR20 and similar models). The driver enables full functionality of the remote including button mapping, gyroscopic airmouse control, and IMU data access. The package includes both a kernel module and Python utilities for calibration, testing, and visualization.

## Project Structure

```
lg-magic/
├── kernel/                      # Linux kernel module
│   ├── lg_magic_main.c          # HID driver entry point
│   ├── lg_magic_airmouse.c      # Fixed-point airmouse math (no FPU!)
│   ├── lg_magic_airmouse.h      # Calibration structs + API
│   ├── Makefile                 # Kbuild
│   └── dkms.conf                # DKMS auto-build config
├── scripts/                     # Python userspace tools
│   ├── calibrate.py             # IMU calibration (ellipsoid fitting)
│   ├── convert_calib.py         # JSON → binary firmware converter
│   ├── display_imu.py           # IMU visualizer / AHRS / airmouse
│   ├── draw_cube.py             # 3D orientation cube (PyQtGraph)
│   ├── lg_magic.py              # Raw HIDRAW packet analyzer
│   └── uinput_mouse.py          # Virtual mouse via uinput
├── tests/                       # Test suite (84 tests, CI-validated)
├── 51-lgimu.rules               # udev rule for IMU device access
└── README.md
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│              LG Magic Remote (MR20)                         │
│          Bluetooth HID device 000F:3412                     │
│    Report ID 0xFD: counter + 6×IMU + buttons + wheel       │
└──────────────────────┬──────────────────────────────────────┘
                       │ BT HID
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                Linux Kernel HID Subsystem                   │
│              hidraw dev + raw_event callback                │
└────────┬────────────────────────────────────┬───────────────┘
         │                                    │
         ▼                                    ▼
┌────────────────────────┐      ┌─────────────────────────────┐
│   lg_magic.ko          │      │   Python Tools (userspace)   │
│                        │      │                             │
│  ┌──────────────────┐  │      │  display_imu.py  visualizer │
│  │ raw_event()      │  │      │  calibrate.py    calib math │
│  │ ┌──────────────┐ │  │ evdev│  convert_calib.py JSON→bin  │
│  │ │ Button map   │ │  │◄─────│  lg_magic.py     HIDRAW dump│
│  │ │ (31 buttons) │ │  │      │  uinput_mouse.py uinput dev │
│  │ ├──────────────┤ │  │      │  draw_cube.py    3D cube    │
│  │ │ Airmouse     │ │  │      └─────────────────────────────┘
│  │ │ (fixed-point)│ │  │
│  │ │ • bias corr  │ │  │
│  │ │ • LPF filter │ │  │      Calibration Pipeline:
│  │ │ • threshold  │ │  │      ┌──────────┐   ┌───────────┐
│  │ │ • mousemap   │ │  │      │ CSV data │──▶│ calibrate │
│  │ └──────────────┘ │  │      └──────────┘   │   .py     │
│  │                   │  │                      └─────┬─────┘
│  │ Firmware loader:  │  │                      JSON calib
│  │ IEEE 754 → int    │  │                      ┌─────▼─────┐
│  │ (no kernel FPU)   │  │                      │ convert   │
│  └──────────────────┘  │                      │ _calib.py │
│                        │                      └─────┬─────┘
│  Output: 2 evdev devs │                    32-byte binary
│  • LG Magic Remote    │                      ┌─────▼─────┐
│  • LG Magic Remote IMU│                      │/lib/      │
└────────────────────────┘                      │firmware/  │
                                                └───────────┘
```

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

### DKMS Installation
```bash
sudo mkdir /usr/src/lg-magic-1.0
sudo cp * /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
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
  Lower = easier to enter airmouse mode; higher = requires faster motion.
- `imu_evdev` (0/1): Expose raw IMU data as separate input device
- `debug` (0-2): Debug message level (0=quiet, 1=normal, 2=verbose)

### Runtime Tuning

All module parameters can be changed at runtime via sysfs without
unloading the module. This is useful for finding the right sensitivity
for your setup:

```bash
# Enable verbose logging to see airmouse events
sudo sh -c 'echo 2 > /sys/module/lg_magic/parameters/debug'
sudo dmesg -w  # watch in another terminal

# Disable/enable airmouse on the fly
sudo sh -c 'echo 0 > /sys/module/lg_magic/parameters/airmouse'
sudo sh -c 'echo 1 > /sys/module/lg_magic/parameters/airmouse'

# Tune threshold — lower = easier activation (good for desktop)
#                      higher = less jitter (good for media center)
sudo sh -c 'echo 150 > /sys/module/lg_magic/parameters/airmouse_threshold'
sudo sh -c 'echo 500 > /sys/module/lg_magic/parameters/airmouse_threshold'

# Enable IMU evdev for Python tools
sudo sh -c 'echo 1 > /sys/module/lg_magic/parameters/imu_evdev'
```

**Typical tunings:**
- **Desktop/HTPC**: `airmouse_threshold=200` — quick airmouse activation
- **Gaming/media center**: `airmouse_threshold=400` — less accidental activation
- **Debug/dev**: `debug=2 imu_evdev=1` — full logging + IMU exposed to Python tools

## Calibration System

### Calibration File Format
The driver loads calibration data from binary files via Linux Firmware subsystem:
- `lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` - MAC address-specific individual calibration
- `lg_magic_calib.bin` - Fallback calibration

### Creating Calibration Files

1. **Collect IMU samples for both calibrations:**
```bash
python3 display_imu.py --csv samples.csv
```

2. **Calculate calibration values:**
```bash
# Calibrate accelerometer (slowly rotate across all axes while collecting)
python3 calibrate.py --accel samples.csv calib.json

# Calibrate gyroscope (keep remote stationary while collecting)
# This auto-merges with the existing calib.json from the accel step
python3 calibrate.py --gyro samples.csv calib.json

# Adjust gyro scale if needed (edit calib.json, recommended value ~0.07)
```

3. **Convert to binary format:**
```bash
python3 convert_calib.py calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5
```

### Calibration Parameters
- `alpha`: Low-pass filter coefficient (0.0-1.0)
- `mouse_k`: Airmouse sensitivity multiplier

### Coordinate System

The remote's IMU uses a right-handed coordinate system.  When held flat
(buttons facing up, IR end pointing forward):

```
         +X (right)
          →
    ┌──────────┐
    │  LG       │ ← +Y (forward / IR end)
    │  Magic    │
    │  Remote   │
    │           │
    └──────────┘
          ↓
         +Z (down toward floor)
```

Axes (raw IMU, before rotation):
- **X**: lateral (right) — gyro pitch axis
- **Y**: longitudinal (forward) — gyro roll axis
- **Z**: vertical (down) — gyro yaw axis

The Python tools apply an `R_align` rotation to align the coordinate
frame with a more intuitive orientation for AHRS and airmouse use.
This rotation is **not** applied in the kernel module (the kernel
calibration directly maps gyro axes to mouse axes: gyro Z → mouse X,
gyro X → mouse Y).
- `gyro_bias`: Gyroscope zero-offset values (computed during calibration)
- `gyro_scale`: Gyroscope scaling factors (tune manually, recommended ~0.07)
- `alpha`: Low-pass filter coefficient (0.0-1.0, recommended 0.2)
- `mouse_k`: Airmouse sensitivity multiplier (0.0-1.0, recommended 0.5)

## Python Tools Usage

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

# 3D orientation cube (--cube implies --ahrs)
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
- **Calibration**: `/lib/firmware/lg_magic_calib.bin`
- **DKMS source**: `/usr/src/lg-magic-1.0/`

## Compatibility

- **Tested with**: LG Magic Remote MR20
- **Kernel versions**: 4.15+ (tested on 6.11)
- **Python**: 3.6+
- **Dependencies**: numpy, scipy, pyqtgraph, python-evdev

## Troubleshooting

### "No IMU evdev device found"
The IMU device is **disabled by default**.  Enable it:
```bash
sudo sh -c 'echo 1 > /sys/module/lg_magic/parameters/imu_evdev'
```
Or load the module with `imu_evdev=1`.

### "Airmouse not working" / no REL_X/REL_Y events
1. Check airmouse is enabled: `cat /sys/module/lg_magic/parameters/airmouse` → should be `1`
2. Check calibration loaded: `dmesg | grep -i calib` → should show "Loaded calibration from..."
3. If no calibration: place `lg_magic_calib.bin` in `/lib/firmware/` and reload module
4. Increase debug level: `echo 2 > /sys/module/lg_magic/parameters/debug` then check dmesg
5. Try lowering threshold: `echo 150 > /sys/module/lg_magic/parameters/airmouse_threshold`

### "Buttons not recognized" / wrong keycodes
Run `python3 lg_magic.py` to see raw HID codes as you press buttons.
Unknown codes can be added to `lg_btn_map[]` in `kernel/lg_magic_main.c`.

### "Unknown descriptor" warnings in dmesg
Non-0xFD HID reports (types 0xF9, 0x01) are expected and logged at debug level only.
Set `debug=2` to see them, `debug=1` to hide them entirely.

### Module fails to build
```bash
# Ensure kernel headers match running kernel
sudo apt-get install linux-headers-$(uname -r) build-essential
cd kernel && make clean && make
```

### Remote not detected after suspend
This is expected — Bluetooth reconnection is handled by the Bluetooth stack.
The kernel driver resets gyro state on resume automatically.
If the remote stays disconnected, toggle Bluetooth or re-pair.

### Calibration "failed validation"
The calibration binary contains values outside acceptable ranges.
Re-run calibration and check:
- Gyro bias should be < |100| units (raw s16)
- Gyro scale should be < |10|
- Alpha and mouse_k should be between 0.0 and 1.0

## Contributing

Please report issues and submit pull requests for:
- Additional device support
- Improved calibration algorithms
- Bug fixes and performance improvements

## License

GPL v2 - Same as Linux kernel

Copyright © 2025 [Ilya Chelyadin]. This project is not affiliated with LG Electronics.

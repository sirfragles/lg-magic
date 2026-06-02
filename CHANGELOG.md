# Changelog

All notable changes to the LG Magic Remote driver will be documented in this file.

## [Unreleased] — v2.0.0 (develop)

### Added
- **CI/CD pipeline** (GitHub Actions): Python lint + test matrix (3.9–3.12), kernel module builds (ubuntu-22.04, 24.04), kernel code quality checks, integration test, auto-release on tags
- **Test suite**: 84 tests covering calibration math, binary format, airmouse algorithm, IEEE 754 decoder, coordinate transforms, and rad→pixel conversion
- **Manual test checklist** (`tests/manual/TEST_CHECKLIST.md`): 9-section hardware validation procedure
- **Suspend/resume support**: `lgmagic_suspend()` releases keys, `lgmagic_resume()` resets gyro state
- **Button cleanup on module unload**: releases held keys before `hid_hw_stop()`
- **Runtime airmouse tuning docs**: sysfs tuning guide with typical presets
- **Architecture diagram** in README
- **Troubleshooting section** in README
- **Coordinate system documentation**: axis diagram and kernel vs Python differences
- **PyPI packaging** (`pyproject.toml`): project metadata, ruff config, pytest config
- **IEEE 754 firmware parser**: pure-integer decoder, no kernel floating-point
- **Auto-detect hidraw device**: `lg_magic.py` scans `/dev/hidraw*` via sysfs
- **Auto-merge calibration JSONs**: `calibrate.py` merges into existing output files

### Changed
- **Kernel FPU safety**: All airmouse math converted from `float` to `s64` fixed-point integer arithmetic (scale = 65536). Zero C floating-point operations in any runtime code path. On-disk firmware format preserved (backward-compatible).
- **Lazy initialization**: `uinput_mouse.py` and `draw_cube.py` no longer create devices/apps on import; factory functions + `atexit` cleanup
- **Report type handling**: Non-0xFD HID reports logged at debug level (not WARN); short reports handled gracefully
- **Button table**: Reorganized into logical groups with section comments and physical button labels
- **`--cube` now implies `--ahrs`** in `display_imu.py`
- **Madgwick filter timing**: Uses actual hardware timestamps instead of fixed 50Hz assumption

### Fixed
- Missing `import sys` in `calibrate.py`
- Invalid `save_calibration_json()` parameter ordering (`filename` after defaults)
- Firmware fallback return value now properly checked; logs message when no calibration found
- Calibration validation runs on both MAC-specific and generic firmware paths

## [1.0.0] — 2025

### Initial Release
- HID driver for LG Magic Remote (Bluetooth 000F:3412)
- 31-button key mapping with airmouse mode switching
- Gyroscope-based airmouse with low-pass filter and threshold detection
- Raw IMU evdev device (accelerometer + gyroscope)
- Calibration via firmware subsystem (MAC-specific + generic fallback)
- Python tools: `calibrate.py` (ellipsoid fitting), `convert_calib.py` (JSON→binary), `display_imu.py` (visualization/AHRS/cube/airmouse), `lg_magic.py` (HIDRAW analyzer)
- DKMS support
- udev rule for IMU device access

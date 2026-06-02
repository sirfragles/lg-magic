# LG Magic Remote — Architecture Analysis & Improvement Notes

*Auto-generated analysis — 2026-06-02*

---

## 1. High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                     LG Magic Remote (MR20)                         │
│                  Bluetooth HID device 000f:3412                     │
│               Report ID 0xFD — 30 bytes (1+29) payload             │
└────────────────────────────────┬───────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────┐
│                     Linux HID Subsystem                            │
│               hidraw + raw_event callback                          │
└────────────────────────────────┬───────────────────────────────────┘
                                 │
          ┌──────────────────────┴──────────────────────┐
          ▼                                              ▼
┌──────────────────────────────┐        ┌──────────────────────────────┐
│  Kernel Module (lg_magic.ko) │        │   Python Userspace Tools      │
│                              │        │   (via evdev / hidraw)        │
│  ┌────────────────────────┐  │        │                               │
│  │  lg_magic_main.c       │  │        │  lg_magic.py    HIDRAW dump   │
│  │  • HID driver entry    │  │        │  display_imu.py  Visualizer   │
│  │  • Probe / Remove      │  │        │  calibrate.py    IMU calib    │
│  │  • raw_event handler   │  │        │  convert_calib.py JSON→binary │
│  │  • Button mapping      │  │        │  draw_cube.py    3D render    │
│  │  • Firmware loading    │  │        │  uinput_mouse.py Virtual mouse│
│  │  • Device registration │  │        │                               │
│  └───────────┬────────────┘  │        │                               │
│              │               │        │                               │
│  ┌───────────▼────────────┐  │        │                               │
│  │  lg_magic_airmouse.c   │  │        │                               │
│  │  • Calibration struct  │  │        │                               │
│  │  • Low-pass filter     │  │        │                               │
│  │  • Mouse delta calc    │  │        │                               │
│  │  • Calib validation    │  │        │                               │
│  └────────────────────────┘  │        │                               │
│                              │        │                               │
│  Output: 2 evdev devices     │        │                               │
│  • "LG Magic Remote"         │        │                               │
│  • "LG Magic Remote IMU"     │        │                               │
└──────────────────────────────┘        └──────────────────────────────┘
```

### Data Flow
1. BT packet → HID core → `lgmagic_raw_event()` (report 0xFD only, 20-byte payload expected)
2. Parse in order: 2-byte LE counter, 2-byte constant (0xFD00), 6× 2-byte BE int16 (gyro XYZ + accel XYZ), 2-byte BE button code, 1-byte wheel delta
3. Load calibration from `/lib/firmware/lg_magic_calib[_XX_XX_XX_XX_XX_XX].bin`
4. **Airmouse pipeline**: gyro_raw → bias correction → scale → LPF(alpha) → threshold check → REL_X/REL_Y
5. **Mode switch**: navigation mode ↔ pointer mode (enters pointer on gyro > threshold, exits on nav button press)
6. Report to one or two evdev nodes; IMU evdev is optional (`imu_evdev` param)

---

## 2. Detailed File-by-File Breakdown

### kernel/lg_magic_main.c — HID Driver (~220 lines)
- **Provisions**: Two `struct input_dev` (hid for buttons/mouse, imu for raw sensor data)
- **Button table**: `lg_btn_map[]` — 31 entries mapping 16-bit HID codes to Linux keycodes. Wheel code (`0x8044`) maps to both `KEY_ENTER` and `BTN_LEFT`, selected at runtime based on mode.
- **Probe**: HID parse → HID start → firmware load (MAC-specific first, then generic fallback) → register input devices
- **raw_event**: Only handles report 0xFD with exact size == 20. Parses packet, handles button press/release state machine, wheel, airmouse, IMU reporting.
- **Module params**: `debug` (0-2), `airmouse` (bool), `airmouse_threshold` (int), `imu_evdev` (bool). All exposed via sysfs.
- **Device match**: `HID_BLUETOOTH_DEVICE(0x000f, 0x3412)` only

### kernel/lg_magic_airmouse.c — Airmouse Math (~60 lines)
- `lgmagic_calc_mouse()`: Bias-correct gyro → scale → LPF with alpha → compute mouse deltas (gyro Z → mouse X, gyro X → mouse Y) scaled by `mouse_k`. Returns threshold flag.
- `lgmagic_validate_calib()`: Sanity checks on calibration values (bias magnitude < 100, scale < 10, alpha in [0,1], mouse_k in [0,1]).
- **Uses floating point** — `float` struct fields, float arithmetic, `lgmagic_fabs()` helper.

### kernel/lg_magic_airmouse.h — Shared Header
- Defines `struct lg_magic_airmouse_calib` with 8 floats: `gyro_bias[3]`, `gyro_scale[3]`, `alpha`, `mouse_k` = 32 bytes total.
- This is the exact binary blob format loaded from firmware files.

### kernel/Makefile
- Multi-file kernel module (`lg_magic_main.o` + `lg_magic_airmouse.o` → `lg_magic.ko`)
- **Important**: `CFLAGS_lg_magic_airmouse.o += $(CC_FLAGS_FPU)` — allows FPU instructions in airmouse.c

### kernel/dkms.conf
- DKMS auto-build/install config, module placed at `/kernel/drivers/input/misc`

### scripts/lg_magic.py — Raw HIDRAW Dump Tool
- Hardcoded `/dev/hidraw7` — legacy debugging tool
- Parses reports 0xFD, 0xF9, 0x01 with format-specific handlers
- Maintains `wheel_pos` accumulator (global var)

### scripts/display_imu.py — IMU Visualizer/Reader (~130 lines)
- Finds IMU evdev by device name containing "IMU"
- Reads evdev absolute axes + MSC_SERIAL counter
- Optional CSV output with counter, dt, ax/y/z, gx/y/z
- Calibration application: `a_corr = matrix @ (a - bias)`; `g_corr = (g - bias) * scale`
- Coordinate alignment: `R_align` rotates accel & gyro (X→Y, Y→−X, Z→−Z)
- Gyro converted to rad/s via `* pi/180`
- Modes: CSV dump, calibrated display, AHRS (Madgwick → Euler angles), 3D cube (pyqtgraph), uinput airmouse
- AHRS: Madgwick filter with `sampleperiod=0.02` (but `Dt = dt` line is commented out → fixed 50Hz assumption)

### scripts/calibrate.py — IMU Calibration
- Loads CSV (counter, dt, ax, ay, az, gx, gy, gz)
- **Accel**: Sphere-fitting via `scipy.optimize.least_squares` — finds bias + 3×3 matrix such that `|M·(a-b)|` ≈ 9.80665 (1g). Needs ≥6 orientations.
- **Gyro**: Simple mean of static samples = bias
- Output: JSON with `accel.bias`, `accel.matrix`, `gyro.bias`, `gyro.scale` (scale defaults to [1,1,1])
- **BUG**: Missing `import sys` — will crash on `sys.exit(1)`

### scripts/convert_calib.py — JSON to Binary Converter
- Reads calibration JSON, packs `gyro.bias[3]`, `gyro.scale[3]`, `alpha`, `mouse_k` → 32-byte binary via `struct.pack("3f3fff", ...)`
- To use: `convert_calib.py calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5`

### scripts/draw_cube.py — 3D Cube Display
- PyQtGraph OpenGL cube rendering
- Global QApplication created at import time (side effect!)
- `update(quat)` — accepts orientation quaternion, rotates mesh via axis-angle
- Only updated from AHRS path in display_imu.py

### scripts/uinput_mouse.py — Virtual Mouse
- Global uinput device created at import time (side effect!)
- `imu_to_mouse_from_rads()` — converts angular velocity to pixel deltas via sensitivity multipliers
- `imu_to_mouse_from_euler()` — Euler angles → angular velocity → mouse (currently unused)
- No cleanup on exit (device left dangling)

### 51-lgimu.rules — udev Rule
- Matches input devices with "IMU" in name → mode 0660, group `input`, with `uaccess` tag

---

## 3. Issues & Improvement Opportunities

### 🔴 Critical / Stability Issues

| # | File | Issue | Recommendation |
|---|------|-------|----------------|
| 1 | `lg_magic_airmouse.c` | **Floating point in kernel without FPU save/restore.** The `lgmagic_calc_mouse()` function uses `float` ops inside the HID `raw_event` callback, which can run in softirq context. On x86 this will corrupt userspace FPU state without `kernel_fpu_begin()`/`kernel_fpu_end()` wrappers. The Makefile enables `CC_FLAGS_FPU` but the code lacks the required save/restore calls. | Either add `kernel_fpu_begin()`/`kernel_fpu_end()` around the float operations (around the call to `lgmagic_calc_mouse` in raw_event), or convert to fixed-point integer math (multiply calibration values by 1000 or 65536 and use int64 arithmetic). Fixed-point is preferred for kernel code. |
| 2 | `calibrate.py:86` | **Missing `import sys`.** The script calls `sys.exit(1)` in the exception handler but never imports `sys`. Runtime crash on any CSV read error. | Add `import sys` at the top of the file. |

### 🟠 Bugs & Functional Issues

| # | File | Issue | Recommendation |
|---|------|-------|----------------|
| 3 | `lg_magic_main.c:168-170` | **Generic firmware load result ignored.** The MAC-specific firmware attempts to load and jumps to `loaded:` on success. The fallback `lg_magic_load_fw("lg_magic_calib.bin", ...)` is called unconditionally but its return value is completely ignored. If both loads fail, uninitialized calibration data (zeros from kzalloc) is used — zero biases, zero scales, zero alpha, zero mouse_k. `validate_calib` will accept this (all zeros pass the bounds check). The airmouse will silently do nothing. | Store and check the return value. If both firmware loads fail, disable airmouse (or just keep it disabled by default, since zeros are safe). At minimum, log a warning. |
| 4 | `lg_magic_main.c:128-129` | **`validate_calib` only called on first successful load, not on fallback.** The validation guard only runs in the MAC-specific path. If the generic fallback file exists but is corrupt, garbage calibration data enters the kernel. | Also validate after the fallback `request_firmware`, or restructure to validate in a single place after loading. |
| 5 | `display_imu.py:105` | **Madgwick `Dt` comment.** Line `#madgwick.Dt = dt` is commented out. The Madgwick filter always uses the constructor's `sampleperiod=T_UNIT=0.02` instead of actual measured dt from the hardware counter. This produces incorrect AHRS output if the actual sampling rate deviates from 50Hz. | Uncomment `madgwick.Dt = dt` (or set it conditionally when dt > 0). |
| 6 | `display_imu.py` | **`--cube` flag is broken without `--ahrs`.** The `--cube` flag starts the pyqtgraph app and spawns the IMU reader thread, but `draw_cube.update()` is only called inside the `if args.ahrs and dt:` block. So `--cube` alone shows a static cube. The README claims `--cube` works standalone. | Either make `--cube` imply `--ahrs`, or move `draw_cube.update()` outside the AHRS-only block, or add an explicit check: `if args.cube and not args.ahrs: parser.error("--cube requires --ahrs")`. |
| 7 | `display_imu.py` | **Coordinate system mismatch between kernel and Python.** The Python code applies `R_align` (X→Y, Y→-X, Z→-Z) to accel and gyro after calibration. The kernel module does NOT apply any such rotation. This means the Python airmouse (via uinput) and the kernel airmouse have different axis mappings. If someone calibrates using the Python tools, the kernel will not behave the same way. | Either apply the same `R_align` in the kernel, or remove it from the Python tools and bake the rotation into the calibration data instead. Document the coordinate system clearly. |

### 🟡 Design & Robustness

| # | File | Issue | Recommendation |
|---|------|-------|----------------|
| 8 | `lg_magic_main.c:114-118` | **No locking on drvdata.** `drvdata->last_keycode`, `drvdata->last_btncode`, `drvdata->mode`, `drvdata->gyro_acc` are all mutated in `raw_event` which may be called concurrently. Technically HID raw_event is serialized per-device, but if this changes or the driver is extended to handle other report types from different contexts, race conditions could occur. | Document the serialization assumption prominently. Or add a spinlock for safety (low cost, defensive). |
| 9 | `lg_magic_main.c` | **No suspend/resume handling.** No `.suspend`/`.resume` callbacks in `hid_driver`. If the system suspends while the remote is connected and wakes up, the BT connection may have been dropped and re-established. The driver relies on HID subsystem defaults, which may or may not handle this gracefully. | Add at minimum dummy suspend/resume that re-initialize driver state. Test suspend/resume cycles. |
| 10 | `lg_magic_main.c` | **Hardcoded report size check.** `if (size != 20 \|\| data[0] != 0xFD)` silently drops non-0xFD or non-20-byte reports. The README acknowledges other report types exist (0xF9, 0x01). Future firmware could change these. | Log unknown reports at debug level (not warn level — they're expected). Consider handling them gracefully if their format is reverse-engineered. |
| 11 | `uinput_mouse.py:8-12` | **Global uinput device created at import time.** `uinput.Device([...])` is a module-level statement. Importing `uinput_mouse` immediately creates the virtual device, even if the user only wants to read IMU data. Also, no `atexit` or `__del__` cleanup — the device persists after the Python process exits until uinput is unloaded. | Lazy-initialize the device (e.g., a `get_device()` factory function), or create it in `imu_to_mouse_from_rads` on first call. Add cleanup via `atexit.register(device.destroy)` or `try/finally`. |
| 12 | `draw_cube.py:12` | **Global QApplication at import time.** Same issue — `import draw_cube` starts Qt. This breaks scripts that import the module for other purposes and prevents running the cube on a non-Qt main thread. | Encapsulate app creation in a function. Only create `QApplication` when `start()` is called. |
| 13 | `lg_magic.py:5` | **Hardcoded device path.** `HIDRAW_DEVICE = "/dev/hidraw7"` — will only work on one specific machine configuration. | Auto-detect: iterate `/dev/hidraw*`, check device name or vendor/product via ioctl or sysfs. |
| 14 | `lg_magic_main.c:108` | **Wheel-as-key in nav mode.** When not in pointer mode, wheel events are reported as individual KEY_UP/KEY_DOWN press-release pairs. This means apps see "tap" behavior rather than smooth scrolling. Appropriate for TV navigation but limiting for other use cases. | Consider a module parameter to control whether wheel in nav mode sends keys or REL_WHEEL events. |
| 15 | `calibrate.py` | **Accel-only or gyro-only runs produce incomplete JSON.** If you run `--accel` then `--gyro` separately, each overwrites the JSON and the first run's data is lost. The README says "Combine Gyro/Accel JSONs" manually, which is error-prone. | Add a `--combine` mode that takes two JSONs and merges them. Or add a `--all` flag that does both calibrations in one run and outputs a complete JSON. |

### 🟢 Improvements & Polish

| # | Area | Suggestion |
|---|------|------------|
| 16 | **Dependencies** | Add `requirements.txt` (or `pyproject.toml`) listing: `numpy`, `scipy`, `pyqtgraph`, `evdev`, `ahrs`, `python-uinput`, `trimesh`. The README lists them but standard Python packaging makes setup easier. |
| 17 | **Tests** | Add unit tests for: calibration math (known inputs → expected outputs), binary packing/unpacking round-trip, button map completeness, calibration validation edge cases. Integration tests for kernel module would be harder but valuable. |
| 18 | **CI/CD** | Add GitHub Actions to: build the kernel module against multiple kernel versions (via containers), run Python linting (flake8/ruff) and tests, check for missing `import sys`. |
| 19 | **Error handling** | Display meaningful error messages when calibration files are missing/malformed. Currently most scripts crash with generic Python exceptions. |
| 20 | **CLI consistency** | `display_imu.py` uses `--calib` (missing 'r') — should be `--calibration` or `--calib` consistently. `convert_calib.py` uses `json_file` as positional arg while `calibrate.py` uses `csv_file`. Unify the CLI style. |
| 21 | **Documentation** | Document the coordinate system explicitly (diagram showing axes on the remote). Document what `R_align` does and why. Explain the relationship between kernel airmouse and Python airmouse. Add architecture diagram to README. |
| 22 | **Versioning** | Add git tags for releases, a `CHANGELOG.md`, and a version string in the kernel module (`MODULE_VERSION`). |
| 23 | **Calibration UX** | The calibration workflow is multi-step and manual (collect CSV → run calibrate.py for accel → run for gyro → manually merge JSONs → convert to binary → copy to /lib/firmware). A single `calibrate_all.py` script that guides the user through the process would dramatically improve usability. |
| 24 | **DKMS integration** | The Makefile `obj-m += lg_magic.o` is in the kernel/ subdirectory but dkms.conf references `MAKE[0]="make KDIR=..."`. DKMS expects the Makefile and source in the DKMS source root. The current layout may require adjustment for DKMS to work out-of-the-box. | Verify DKMS paths and add a top-level Makefile wrapper if needed. |
| 25 | **Architecture support** | The `CC_FLAGS_FPU` variable is arch-dependent. On ARM it may be `-mfpu=neon`; on x86 it's nothing special. The Makefile should handle this or the code should avoid floats. |
| 26 | **Performance** | The HID raw_event handler runs at ~50Hz. Float operations are minimal but could be optimized: pre-compute `mouse_k` and other constants at probe time, use a simple array for gyro_acc rather than struct field access in the hot path. |
| 27 | **IMU evdev default** | `imu_evdev=0` by default but the Python tools require it. New users will be confused why `display_imu.py` can't find the device. | Default to `1` or add prominent documentation that it must be enabled. |
| 28 | **Report ID discovery** | The `lg_magic.py` tool hardcodes report IDs 0xFD, 0xF9, 0x01. A more robust approach would parse the HID report descriptor from sysfs to discover available report IDs and sizes dynamically. |
| 29 | **Multiple remotes** | The driver uses `hid_get_drvdata(hdev)` and the HID subsystem binds per-device, so it technically supports multiple remotes. But calibration firmware is MAC-address-specific with a fixed naming pattern. If two remotes share the same MAC (unlikely but possible with spoofed BT), they'd share calibration. This is fine but worth documenting. |
| 30 | **AHRS yaw drift** | The Madgwick AHRS uses only gyro + accel (no magnetometer). Yaw will drift over time without a magnetometer reference. The README doesn't mention this limitation. | Document that yaw is unreliable, or add a heading-reset mechanism (e.g., reset yaw to 0 on button press). |

---

## 4. Dependency Graph

```
Kernel Module:
  lg_magic.ko
  ├── lg_magic_main.c
  │     depends: linux/hid.h, linux/input.h, linux/firmware.h
  │     depends: lg_magic_airmouse.h
  └── lg_magic_airmouse.c
        depends: linux/types.h, lg_magic_airmouse.h

Python Tools:
  display_imu.py
  ├── calibrate.py  (CSV loading shared concept)
  ├── convert_calib.py  (binary loading concept)
  ├── draw_cube.py  (imported for --cube)
  │     depends: pyqtgraph, numpy, trimesh
  └── uinput_mouse.py  (imported for --mouse)
        depends: uinput, numpy

  calibrate.py
  └── depends: numpy, scipy

  convert_calib.py
  └── depends: (stdlib only — json, struct)

  lg_magic.py
  └── depends: (stdlib only)
```

---

## 5. Key Strengths

- **Clean split** between kernel (production, low-latency) and Python (debugging, calibration, experimentation)
- **Dual airmouse** implementation is a smart development strategy — iterate in Python, deploy in C
- **Calibration via firmware subsystem** is the correct Linux approach — standard, well-understood, no custom file formats
- **DKMS support** makes it distribution-friendly
- **Good use of evdev** — standard input subsystem, no custom protocols
- **The calibration algorithm** (ellipsoid fitting for accelerometer) is the right approach for IMU calibration
- **Module parameters with sysfs** exposure allows runtime tuning
- **MAC-address-specific calibration** is the right level of granularity
- **GPLv2 license** matches the kernel, no licensing conflicts

---

## 6. Quick Wins (Priority Order)

1. **Fix `import sys` bug in calibrate.py** — one line, prevents crash.
2. **Add `kernel_fpu_begin/end` in airmouse path** (or convert to fixed-point) — prevents potential corruption on x86. Most important correctness fix.
3. **Fix firmware fallback return value ignored** — add a warning log when both loads fail.
4. **Uncomment `madgwick.Dt = dt`** in display_imu.py — makes AHRS actually use real timing.
5. **Fix `--cube` without `--ahrs`** — either error or auto-enable AHRS.
6. **Add `requirements.txt`** — standardize dependencies.
7. **Default `imu_evdev=1`** — improves out-of-box experience.
8. **Lazy-init uinput device in uinput_mouse.py** — avoids side effects on import.

# LG Magic Remote — Development Roadmap

*Branch: `develop` | Target: v2.0.0*

---

## Overview

This roadmap transforms the LG Magic Remote driver from a working prototype into a production-grade Linux kernel driver. It is structured in milestones ordered by risk/impact priority.

Each milestone is self-contained and produces a releasable state. The milestones build on each other.

---

## Milestone 0: CI/CD Foundation ✅ (DONE)

**Goal**: A green CI pipeline that builds, lints, and tests on every push/PR.

**Deliverables**:
- [x] `.github/workflows/ci.yml` — CI pipeline with:
  - Python lint (ruff) on 3.11
  - Python unit tests across 3.9, 3.10, 3.11, 3.12
  - Kernel module build against host kernels on ubuntu-22.04, ubuntu-24.04
  - Integration test (calibration round-trip)
  - Release packaging on git tags
- [x] `pyproject.toml` with project metadata, ruff config, pytest config
- [x] `requirements.txt` and `requirements-dev.txt`
- [x] `tests/test_calibrate.py` — CSV loading, gyro bias, accel calibration, JSON output
- [x] `tests/test_convert_calib.py` — binary packing round-trip
- [x] `tests/test_airmouse.py` — calibration validation, LPF response, threshold, bias correction
- [x] `tests/integration/calib_roundtrip.sh` — end-to-end CSV→JSON→binary

---

## Milestone 1: Critical Bug Fixes 🚨

**Goal**: Fix the most impactful bugs identified in ANALYSIS_NOTES.md.

### 1.1 Fix `import sys` in calibrate.py
- **File**: `scripts/calibrate.py`
- **Change**: Add `import sys` at top of file
- **Risk**: Zero
- **Test**: Existing integration test covers this

### 1.2 Kernel FPU safety
- **File**: `kernel/lg_magic_airmouse.c`, `kernel/lg_magic_main.c`
- **Problem**: Float arithmetic in HID raw_event without FPU save/restore
- **Solution A** (preferred): Convert to fixed-point integer math
  - Multiply all calibration values by 65536 at load time
  - Use `s64` accumulators, shift right 16 at output
  - Remove `CFLAGS_lg_magic_airmouse.o += $(CC_FLAGS_FPU)` from Makefile
  - Remove all `float` from kernel code
- **Solution B** (simpler): Add `kernel_fpu_begin()`/`kernel_fpu_end()` around `lgmagic_calc_mouse()` call in raw_event
- **Risk**: High (kernel instability on x86)
- **Test**: Build + boot test on real x86 hardware; `perf` to verify no FPU state warnings

### 1.3 Firmware fallback return value
- **File**: `kernel/lg_magic_main.c`
- **Change**: Store return value of generic firmware load, log warning if both MAC-specific and generic fail
- **Risk**: Low

### 1.4 Validate calibration on fallback load
- **File**: `kernel/lg_magic_main.c`
- **Change**: Call `lgmagic_validate_calib()` after generic firmware load (not just MAC-specific)
- **Risk**: Low

---

## Milestone 2: Python Tool Fixes 🐍

**Goal**: Fix bugs and design issues in Python scripts.

### 2.1 Lazy initialization for uinput_mouse
- **File**: `scripts/uinput_mouse.py`
- **Change**: Move `uinput.Device(...)` into a function; add `atexit` cleanup
- **Risk**: Low
- **Test**: Import uinput_mouse without root — should not error

### 2.2 Lazy initialization for draw_cube
- **File**: `scripts/draw_cube.py`
- **Change**: Move `QApplication([])` into `start()` function; guard with `QApplication.instance()` check
- **Risk**: Low

### 2.3 Fix Madgwick timing
- **File**: `scripts/display_imu.py`
- **Change**: Uncomment `madgwick.Dt = dt`
- **Risk**: Low

### 2.4 Fix `--cube` without `--ahrs`
- **File**: `scripts/display_imu.py`
- **Change**: Make `--cube` imply `--ahrs`, or error with helpful message
- **Risk**: Low

### 2.5 Auto-detect hidraw device
- **File**: `scripts/lg_magic.py`
- **Change**: Scan `/dev/hidraw*`, check name for "LG" or vendor/product via sysfs
- **Risk**: Low

### 2.6 Coordinate system documentation
- **File**: `README.md`
- **Change**: Add diagram showing remote axes; document `R_align` transformation; explain kernel vs Python coordinate handling
- **Risk**: Zero

### 2.7 Merge calibration JSONs automatically
- **File**: `scripts/calibrate.py` or new `scripts/merge_calib.py`
- **Change**: Add `--combine` flag that takes two JSONs and merges; or add `--all` flag to do both accel+gyro in one run
- **Risk**: Low

---

## Milestone 3: Kernel Module Robustness 🛡️

**Goal**: Production-quality kernel driver.

### 3.1 Suspend/resume support
- **File**: `kernel/lg_magic_main.c`
- **Change**: Add `.suspend` and `.resume` callbacks to `hid_driver`
  - Suspend: release any held keys, reset mode
  - Resume: re-read calibration firmware (in case of re-probe), reset gyro accumulator
- **Risk**: Medium
- **Test**: Manual suspend/resume cycle testing

### 3.2 Handle report types gracefully
- **File**: `kernel/lg_magic_main.c`
- **Change**: Accept known report types (0xF9, 0x01) with appropriate handling or at debug-level logging; don't warn for unknown but expected types
- **Risk**: Low

### 3.3 Button cleanup on device removal
- **File**: `kernel/lg_magic_main.c`
- **Change**: In `lgmagic_remove()`, release any currently-held buttons before stopping HID
- **Risk**: Low

### 3.4 Readable button table with comments
- **File**: `kernel/lg_magic_main.c`
- **Change**: Add inline comments next to each button entry documenting the physical button label (e.g. `{ 0x8043, KEY_SETUP }, // [Settings gear icon]`)
- **Risk**: Zero

### 3.5 Run-time airmouse sensitivity tuning
- **File**: `kernel/lg_magic_main.c`
- **Change**: Already exposed as module params; add documentation in README for real-time tuning workflow (sysfs echo commands with example values)
- **Risk**: Zero

---

## Milestone 4: Testing & Quality 🧪

**Goal**: Comprehensive test coverage.

### 4.1 Increase Python test coverage
- Add tests for `display_imu.py` logic (coordinate transform, calibration application)
- Add tests for `uinput_mouse.py` math (rads→pixel conversion)
- Add tests for edge cases: empty CSV, NaN values, calibration with singular matrix
- Target: >80% coverage on calibration and conversion code

### 4.2 Kernel module static analysis
- Integrate `sparse` or `smatch` into CI
- Run `checkpatch.pl --strict` against kernel code (already in CI, ensure it passes cleanly)

### 4.3 Kernel module unit tests (KUnit)
- **Optional** — requires building against a kernel with KUnit support
- Test calibration validation edge cases in-kernel
- Test button table consistency (no duplicate codes, all codes documented)

### 4.4 Hardware-in-the-loop tests (manual)
- Document test procedure:
  1. Pair remote with test machine
  2. Load module with `imu_evdev=1 debug=2`
  3. Press each button, verify correct keycode via `evtest`
  4. Calibrate airmouse, verify pointer movement
  5. Suspend/resume, verify remote still works
- Create test checklist in `tests/manual/TEST_CHECKLIST.md`

---

## Milestone 5: Documentation & DX 📚

**Goal**: Great developer and user experience.

### 5.1 Architecture diagram
- Add ASCII art or SVG architecture diagram to README
- Show data flow from Bluetooth → HID → kernel → evdev → userspace

### 5.2 Calibration guide with screenshots
- Step-by-step screenshots of calibration workflow
- Common pitfalls: "For accel calibration, rotate slowly through 6 orientations"
- Show example output after successful calibration

### 5.3 Troubleshooting section
- "IMU device not found" → enable `imu_evdev=1`
- "Airmouse not working" → check calibration loaded, verify with debug=2
- "Buttons not recognized" → run `lg_magic.py` to see raw codes, add mapping

### 5.4 CONTRIBUTING.md
- How to set up dev environment
- Kernel module build instructions for different distros
- Python venv setup
- How to run tests
- Coding style (checkpatch for C, ruff for Python)

### 5.5 CHANGELOG.md
- Start tracking changes from v1.0.0
- Keep it updated per milestone

---

## Milestone 6: Release v2.0.0 🚀

**Goal**: Tag and release.

### 6.1 Version bump
- Update version in `pyproject.toml` to `2.0.0`
- Update `dkms.conf` `PACKAGE_VERSION` to `2.0`
- Add `MODULE_VERSION("2.0");` in kernel module

### 6.2 Release notes
- Summarize all changes since v1.0.0
- Breaking changes: fixed-point math change (users must re-run calibration), coordinate system alignment
- New features: CI/CD, test suite, suspend/resume support

### 6.3 Tag and publish
```bash
git tag -a v2.0.0 -m "v2.0.0: Production-grade release"
git push origin v2.0.0
```
- GitHub Actions will automatically build and attach release artifacts

---

## Milestone 7: Future / Nice-to-Have 🔮

**Goal**: Longer-term improvements (post-v2.0.0).

### 7.1 Standalone calibration wizard
- New script: `scripts/calibrate_wizard.py`
- Interactive CLI: "Place remote flat on table (gyro calibration)" → collect → "Rotate to 6 positions (accel calibration)" → guide → "Test airmouse" → save
- One-command calibration experience

### 7.2 Systemd service for auto-calibration loading
- On boot: detect paired LG Magic Remote, load appropriate calibration from user home directory or system firmware path
- Handle reconnect events

### 7.3 Additional device support
- Reverse-engineer and support LG Magic Remote MR21, MR22 variants
- Support non-Bluetooth variants if they exist

### 7.4 ConfigFS-based runtime calibration update
- Instead of firmware files on disk, expose calibration via configfs
- Allows calibration tools to upload calibration without root (via udev uaccess)

### 7.5 Magnetometer integration (if hardware supports it)
- Investigate if remote has a magnetometer (reports 0xF9, 0x01 may contain this)
- If yes: full 9-DOF AHRS, no yaw drift

### 7.6 Python package on PyPI
- Publish `lg-magic` as a pip-installable package with console entry points
- `lg-magic-calibrate`, `lg-magic-display`, etc.

### 7.7 i18n
- The README is already bilingual (EN/RU)
- Add i18n to Python tools for CLI messages

---

## Dependencies Between Milestones

```
M0 (CI/CD) ──► M1 (Critical fixes) ──► M2 (Python fixes) ──► M3 (Kernel robustness)
                                                                    │
                                                                    ▼
                                               M4 (Testing) ◄──────┘
                                                                    │
                                                                    ▼
                                               M5 (Documentation) ◄─┘
                                                                    │
                                                                    ▼
                                               M6 (Release v2.0.0)
                                                                    │
                                                                    ▼
                                               M7 (Future)
```

M2 and M3 can proceed in parallel after M1.

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| Kernel FPU fix breaks airmouse on ARM | High | Test on ARM SBC (Raspberry Pi) before releasing |
| Fixed-point conversion changes airmouse feel | Medium | Keep sensitivity tunable; document new calibration values |
| Suspend/resume change breaks on certain BT chipsets | Medium | Test on Intel, Broadcom, Realtek BT adapters |
| Breaking calibration format change | Low | Support both old and new formats with auto-detection |

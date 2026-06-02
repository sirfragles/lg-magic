# LG Magic Remote — Development Roadmap

*Branch: `develop` | Target: v2.0.0 | Last updated: 2026-06-02*

---

## Overview

This roadmap transforms the LG Magic Remote driver from a working prototype into a production-grade Linux kernel driver. It is structured in milestones ordered by risk/impact priority.

Each milestone is self-contained and produces a releasable state. The milestones build on each other.

---

## Milestone 0: CI/CD Foundation ✅ (DONE — 2026-06-02)

**Goal**: A green CI pipeline that builds, lints, and tests on every push/PR.

**Deliverables**:
- [x] `.github/workflows/ci.yml` — CI pipeline with:
  - Python lint (ruff) on 3.11
  - Python unit tests across 3.9, 3.10, 3.11, 3.12
  - Kernel module build against host kernels on ubuntu-22.04, ubuntu-24.04
  - Kernel code quality checks (float usage, license headers, style)
  - Integration test (calibration round-trip)
  - Release packaging on git tags
- [x] `pyproject.toml` with project metadata, ruff config, pytest config
- [x] `requirements.txt` and `requirements-dev.txt`
- [x] `tests/test_calibrate.py` — 9 tests: CSV loading, gyro bias, accel calibration, JSON output
- [x] `tests/test_convert_calib.py` — 2 tests: binary packing round-trip, struct size
- [x] `tests/test_airmouse.py` — 15 tests: calibration validation, LPF response, threshold, bias correction
- [x] `tests/integration/calib_roundtrip.sh` — end-to-end CSV→JSON→binary→validate
- [x] Branched `develop` from `master`, pushed to GitHub
- [x] CI all-green on every commit

**Commits**: `5c133c0`, `a2a4b01`

---

## Milestone 1: Critical Bug Fixes ✅ (DONE — 2026-06-02)

**Goal**: Fix the most impactful bugs identified in ANALYSIS_NOTES.md.

### 1.1 Fix `import sys` in calibrate.py ✅
- **File**: `scripts/calibrate.py`
- **Change**: Added `import sys` at top of file
- **Additional**: Fixed `save_calibration_json()` parameter ordering (filename after defaults — invalid Python), converted callers to keyword args

### 1.2 Kernel FPU safety ✅ (SOLUTION A — fixed-point)
- **Files**: `kernel/lg_magic_airmouse.c`, `kernel/lg_magic_airmouse.h`, `kernel/lg_magic_main.c`, `kernel/Makefile`
- **Approach**: Complete elimination of C floating-point operations from the kernel module
  - **On-disk format**: `struct lg_magic_airmouse_calib` preserved (IEEE 754 floats, 32 bytes) — backward-compatible firmware files, used only for `sizeof()` at compile time
  - **Runtime format**: New `struct lg_magic_airmouse_calib_fp` — `s64 gyro_bias[3]`, `s32 gyro_scale[3]`, `s32 alpha`, `s32 mouse_k`, all scaled by 2^16 = 65536
  - **Firmware parser**: `ieee754_f32_to_fp()` — pure integer IEEE 754 decoder, no C float types
  - **Airmouse math**: `lgmagic_calc_mouse()` — LPF `(alpha*corr + (SCALE-alpha)*acc) >> 16`, mouse `(gyro_acc * mouse_k) / SCALE²`
  - **Validation**: `lgmagic_validate_calib_fp()` — bounds checked in fixed-point (bias ±100·SCALE, scale ±10·SCALE)
  - **Makefile**: Removed `CFLAGS_lg_magic_airmouse.o += $(CC_FLAGS_FPU)`
- **Result**: Zero C float operations in any runtime code path. Hot path (HID raw_event → calc_mouse) is 100% `s64` integer.
- **CI**: Kernel builds pass on ubuntu-22.04 (6.17 kernel) and ubuntu-24.04 (6.17 kernel) without SSE/FPU flags

### 1.3 Firmware fallback return value ✅
- **File**: `kernel/lg_magic_main.c`
- **Change**: `lgmagic_load_fw()` now properly returns error codes; `lgmagic_probe()` checks the generic fallback return value and logs an informative message when no calibration is found

### 1.4 Validate calibration on fallback load ✅
- **File**: `kernel/lg_magic_main.c`
- **Change**: Validation is now done inside `lgmagic_load_fw()` which is called for both MAC-specific and generic firmware paths. Validation happens after fixed-point conversion using `lgmagic_validate_calib_fp()`. On failure, the calib struct is zeroed (safe no-op default).

**Commits**: `17feb80`, `f1de624`, `616a2ce`

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

### 4.5 IEEE 754 decoder tests
- Verify `ieee754_f32_to_fp()` round-trips against known float values
- Test edge cases: zero, subnormals, negative values, large/small exponents
- Can be done as Python tests that reimplement the decoder for comparison

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
- [x] Already created as part of M0
- Keep updated as conventions evolve

### 5.5 CHANGELOG.md
- Start tracking changes from v1.0.0
- Keep it updated per milestone

---

## Milestone 6: Release v2.0.0 🚀

**Goal**: Tag and release.

### 6.1 Version bump
- Update version in `pyproject.toml` to `2.0.0`
- Update `dkms.conf` `PACKAGE_VERSION` to `2.0`
- Already added `MODULE_VERSION("2.0")` in kernel module (M1)

### 6.2 Release notes
- Summarize all changes since v1.0.0
- Breaking changes: none for calibration format (backward-compatible); airmouse math is now fixed-point (same behavior, safer)
- New features: CI/CD, test suite (26 tests), FPU-safe kernel math, improved firmware loading
- Bug fixes: `import sys` in calibrate.py, `save_calibration_json()` syntax, firmware fallback handling

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

## Progress Summary

| Milestone | Status | Completion Date |
|-----------|--------|----------------|
| M0: CI/CD Foundation | ✅ Done | 2026-06-02 |
| M1: Critical Bug Fixes | ✅ Done | 2026-06-02 |
| M2: Python Tool Fixes | ⬜ Open | — |
| M3: Kernel Robustness | ⬜ Open | — |
| M4: Testing & Quality | ⬜ Open | — |
| M5: Documentation & DX | ⬜ Open | — |
| M6: Release v2.0.0 | ⬜ Open | — |
| M7: Future | ⬜ Open | — |

---

## Dependencies Between Milestones

```
M0 (CI/CD) ✅ ──► M1 (Critical fixes) ✅ ──► M2 (Python fixes) ──► M3 (Kernel robustness)
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

M2 and M3 can proceed in parallel.

---

## Risk Register

| Risk | Impact | Mitigation | Status |
|------|--------|------------|--------|
| Kernel FPU fix breaks airmouse on ARM | High | Test on ARM SBC (Raspberry Pi) before releasing | Pending ARM test |
| Fixed-point conversion changes airmouse feel | Medium | Same effective math; sensitivity tunable via module params | ✅ Low risk — math is equivalent |
| Suspend/resume change breaks on certain BT chipsets | Medium | Test on Intel, Broadcom, Realtek BT adapters | Pending |
| Calibration format change breaks existing users | Low | Format is backward-compatible (same 32-byte IEEE 754 blob) | ✅ Resolved |
| IEEE 754 decoder has edge-case bugs | Low | Add unit tests for decoder (M4.5); values are in well-behaved range | Pending tests |

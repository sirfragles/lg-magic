# Testing lg-magic

How the LG Magic Remote driver and its userspace tools are tested.

There are four layers:

1. **Portable unit tests** - the eight numeric/parse/IO modules under
   `tools/`. They compile and run on any host with a C99 compiler (macOS
   included) via `make -C tools check`.
2. **CLI smoke tests** - `tools/tests/cli_smoke.sh`, run automatically by
   `make -C tools check` once the Linux-only `lg-magic` binary has been
   built.
3. **Reference-data parity checks** - the C tools re-run the reference
   Python pipeline (`scripts/`) over the committed fixtures in `testdata/`
   and must agree with the committed reference outputs.
4. **Integration / packaging / hardware checks** - need a Linux host with
   the kernel driver, and finally the remote itself.

Fixtures are committed on purpose: **a missing fixture is a test FAILURE,
never a reason to embed invented golden data in a test.**

---

## 1. Portable unit tests

```sh
cd tools
make check          # build and run all test binaries
```

Each test binary prints one `PASS`/`FAIL` line per check, a
`test_xxx: N passed, M failed` summary, and exits nonzero if anything
failed. `make check` fails if any binary fails.

Current state (all eight binaries green):

| test binary            | module(s) under test                     | checks |
|------------------------|------------------------------------------|-------:|
| `tests/test_json`      | `src/json.c`                             |     59 |
| `tests/test_matrix`    | `src/matrix.c`                           |     36 |
| `tests/test_csv`       | `src/csv.c`                              |     10 |
| `tests/test_madgwick`  | `src/madgwick.c`                         |      2 |
| `tests/test_lm`        | `src/lm.c`                               |      9 |
| `tests/test_calib_blob`| `src/calib.c` + `include/lg_magic_calib.h`|    27 |
| `tests/test_config`    | `src/config.c`                           |     33 |
| `tests/test_cube_math` | `src/cube.c`                             |     19 |

Notes and conventions:

- Every test compiles against **all** portable sources
  (`src/{matrix,json,csv,calib,madgwick,lm,cube,config}.c`), so an API
  change in one module breaks every test that uses it - by design.
- Tests run with the working directory `tools/`; fixtures are found at
  `../testdata/` (the harness `tu_fixture_path()` also tries `testdata/`).
  Helper code is shared in `tests/test_util.h`.
- Tests are hermetic and deterministic: fixed seeds (no
  `rand()`/time-based data), and temporary files only under
  `${TMPDIR:-/tmp}` via `tu_temp_path()`, removed on exit.
- `test_config` points `HOME` at a scratch directory so the real user
  config is never touched. One assumption cannot be neutralised: the
  system-wide `/etc/lg-magic/config.json` is read by `config_load()`, so
  the test suite assumes **no `/etc/lg-magic/config.json` exists on the
  host**.
- `test_madgwick` replays `../testdata/madgwick_ref.csv` (500 rows, 50 Hz
  recordings) through `madgwick_init()`/`madgwick_update_imu()` and
  compares every quaternion component against the reference within
  `1e-6`, allowing for a sign flip (q and -q are the same rotation).
- `test_lm` builds its own synthetic ellipsoid data with a known bias, so
  bias recovery is checkable in absolute terms; the matrix M is compared
  only through the gauge-invariant product M^T M (see section 3 for why
  raw parameters are never comparable).
- `test_calib_blob` compile-time checks the blob layout
  (`_Static_assert`, 32 bytes, offsets 0/12/24/28) and byte-compares
  `calib_to_blob()` output against the golden `ref_calib.bin`.

Adding a test: a new `tests/test_x.c` needs one explicit rule in
`tools/Makefile` (pattern rules are avoided for BSD make).

## 2. CLI smoke tests

`make -C tools check` runs `sh tests/cli_smoke.sh ./lg-magic` only when the
binary exists. The binary is Linux-only (evdev/hidraw/uinput); build it
with:

```sh
cd tools
make lg-magic      # needs a Linux kernel build tree environment
```

The smoke script then covers:

- top-level handling in `src/main.c`: `--help`/`-h` (usage on stdout,
  exit 0), no arguments (usage on stderr, exit 1), `--version`
  (`lg-magic 1.0`), unknown subcommand, `--config FILE` argument handling
  and the global-flag-in-any-position rule;
- the documented contract that every subcommand (`analyze`, `imu`,
  `calibrate`, `calib2bin`, `config`, `setup`) prints its own usage on
  `--help` and exits 0;
- the `config` subcommand end to end **against a scratch `HOME`** (it
  must never touch the real user config): defaults, `path`, `set` with
  save + reload round trip, error paths (unknown key, non-numeric value,
  missing value, unknown argument, extra arguments);
- the `--config FILE` merge (extra file overrides the user file) with the
  flag in both positions, and a missing `--config` file being skipped
  rather than fatal.

On hosts where the binary does not exist (macOS, or Linux before the
userspace build is complete) the script prints `SKIP` and exits 0 so
`make check` still passes.

## 3. Reference-data parity checks (no hardware needed)

`testdata/` contains outputs of the reference Python implementation
(`scripts/`), produced once and committed. The C tools must reproduce
them:

| fixture            | produced by            | purpose |
|--------------------|------------------------|---------|
| `madgwick_ref.csv` | Madgwick replay at 50 Hz (ahrs `updateIMU`, beta 0.1), seeded with `accel_to_quat()` of the first row | replay parity of `madgwick.c`, covered by `test_madgwick` (1e-6) |
| `ref_accel.json`   | `scripts/calibrate.py` (scipy) on `sample_imu.csv` | accel calibration reference |
| `ref_gyro.json`    | gyro-only calibration (empty accel arrays) | gyro bias reference; also the empty-array robustness case in `test_calib_blob` |
| `ref_calib.json`   | hand-built calibration | `calib_load()` values in `test_calib_blob` |
| `ref_calib.bin`    | `scripts/convert_calib.py` output | 32-byte LE float blob golden |
| `sample_imu.csv`   | raw IMU recording (`counter,dt,ax,ay,az,gx,gy,gz`, dt empty when unknown) | input for calibrate/parity runs |

Checks that run on a Linux host with the full binary:

1. **Gyro calibration parity (gauge-free).** Fit the gyro calibration
   from `sample_imu.csv` with the C `calibrate` command and compare the
   mean gyro bias to `ref_gyro.json` (`[1.5115, -2.199, 0.798]`). A
   mean/offset bias is invariant under the rotation gauge, so it must
   agree within `1e-6` in the same normalized units.

2. **Accel calibration parity - corrected norms, not parameters.**
   `ref_accel.json` holds the parameters of a scipy TRF fit, which is
   **only identifiable up to a left rotation**: `r(x) = ||M(a - b)|| -
   9.80665` is unchanged by `M' = R M`, and scipy and the C LM solver
   legitimately land on different rotated minima with identical cost.
   Raw parameter comparison is therefore meaningless. Check instead:
   run the C `calibrate` on `sample_imu.csv` and verify every corrected
   sample norm `||M(a_i - b)||` stays within `0.05 m/s^2` of `9.80665`
   (recording noise is ~15 counts ~ 0.015 m/s^2, so this is a wide
   margin). Equivalently, the final mean-squared cost of both solvers
   must be `~3e-4` (C: 0.000283777, scipy: 0.000298288 on this fixture).

3. **Blob byte parity.** Converting the C calibration JSON with
   `alpha = 0.2` and `mouse_k = 0.5` must produce a 32-byte blob
   byte-identical to `ref_calib.bin` (little-endian floats:
   `gyro_bias[3] @ 0`, `gyro_scale[3] @ 12`, `alpha @ 24`,
   `mouse_k @ 28`). Also covered at unit level by `test_calib_blob`
   (`memcmp` against the committed binary fixture), including the range
   checks the kernel applies on load (`|bias| <= 100`, `|scale| <= 10`,
   `alpha, mouse_k in [0, 1]`).

4. **Madgwick replay parity.** Replay `madgwick_ref.csv` through the C
   filter as described above - covered continuously by `test_madgwick`.
   Note the fixture convention: rows with exactly zero gyro (rows 0 and 3)
   are the ahrs "zero gyro" early-return rows, so they record the
   unchanged quaternion and every implementation agrees on them.

## 4. Integration tests without hardware (Linux)

Once the kernel module and `lg-magic` build, and **before** touching real
hardware, exercise the driver end to end with a virtual IMU:

1. Create a fake uinput device whose name contains `IMU` (the driver and
   tools auto-detect the IMU input device by name; the real device is
   `LG Magic Remote IMU`). `scripts/uinput_mouse.py` shows the
   `python-uinput` API to copy; emit synthetic accel/gyro events in the
   axis layout documented in `kernel/lg_magic_airmouse.h`.
2. `lg-magic imu --csv` must stream CSV rows
   (`counter,dt,ax,ay,az,gx,gy,gz`, dt empty when the sample interval is
   unknown) that match the injected events; `--ahrs` must fuse them into
   a stable orientation, and `--mouse` must move the pointer for nonzero
   gyro and stay still at rest. Exact flags per `lg-magic imu --help`.
3. Capture a HID report stream and decode it with `lg-magic analyze`,
   comparing against `python3 scripts/lg_magic.py` on the same bytes.

## 5. Packaging test (Linux container)

Run in a clean Debian/Ubuntu container (a Linux container keeps the
dkms/kernel steps off the developer host):

```sh
dpkg-buildpackage -us -uc -b     # build the .deb
# in a second clean container:
dpkg -i ../lg-magic_1.0_*.deb
```

Verify: the package installs without errors; the dkms module source is
present at `/usr/src/lg-magic-1.0` and `dkms add/build/install` succeed
(`lg_magic` lands in `/kernel/drivers/input/misc`); the udev rule
(`51-lgimu.rules`) is installed and `udevadm control --reload` +
`udevadm trigger` complete without errors; `lg-magic --version` works;
and `dpkg -P lg-magic` purges everything, including the dkms module
registration.

## 6. Manual hardware checklist

With the LG Magic Remote (MR20) paired over Bluetooth:

```sh
sudo lg-magic setup          # wizard: configure, calibrate, install
```

1. `setup` finds the remote and both input devices.
2. Air-mouse mode: the pointer tracks hand motion, gyro rest drift is
   small, and the buttons still click.
3. Cube mode: the cube rotates with the remote; **Ctrl+C restores the
   system cursor** (no stuck grab).
4. The driver logs `Loading LG Magic calibration` (`dmesg`) when a
   calibration blob is loaded.
5. `lg-magic analyze` output matches `python3 scripts/lg_magic.py`
   decoding the same report stream.

#!/usr/bin/env bash
# Integration test: Full calibration round-trip
# 1. Generate synthetic IMU CSV
# 2. Run calibrate.py --gyro
# 3. Run calibrate.py --accel
# 4. Merge JSONs
# 5. Convert to binary via convert_calib.py
# 6. Verify binary structure
set -euo pipefail

SCRIPTS="$(cd "$(dirname "$0")/../../scripts" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

CSV="$TMPDIR/samples.csv"
GYRO_JSON="$TMPDIR/gyro.json"
ACCEL_JSON="$TMPDIR/accel.json"
MERGED_JSON="$TMPDIR/calib.json"
BIN="$TMPDIR/calib.bin"

echo "=== Step 1: Generate synthetic IMU data ==="
python3 -c "
import numpy as np
# Stationary gyro samples (bias = 10, -5, 2) with noise
g_bias = np.array([10.0, -5.0, 2.0])
g_samples = np.random.RandomState(42).normal(g_bias, 2.0, (200, 3))

# Accel samples from 6 orientations (bias = 100, -50, 200)
g = 9.80665
a_bias = np.array([100.0, -50.0, 200.0])
dirs = np.array([[g,0,0],[-g,0,0],[0,g,0],[0,-g,0],[0,0,g],[0,0,-g]])
a_samples = np.vstack([a_bias + d + np.random.RandomState(42).normal(0,1,3) for d in dirs])

# Interleave: repeat gyro to match accel count
n = len(a_samples)
g_pad = np.tile(g_samples, (n // len(g_samples) + 1, 1))[:n]

# Synthesize counter column
counter = np.arange(n).reshape(-1, 1)
dt = np.full((n, 1), 0.02)

data = np.hstack([counter, dt, a_samples, g_pad])
with open('$CSV','w') as f:
    for row in data:
        f.write(','.join(str(v) for v in row) + '\n')
"
echo "  Generated $(wc -l < "$CSV") samples"

echo ""
echo "=== Step 2: Gyro calibration ==="
python3 "$SCRIPTS/calibrate.py" --gyro "$CSV" "$GYRO_JSON"
echo "  Output: $GYRO_JSON"

echo ""
echo "=== Step 3: Accel calibration ==="
python3 "$SCRIPTS/calibrate.py" --accel "$CSV" "$ACCEL_JSON"
echo "  Output: $ACCEL_JSON"

echo ""
echo "=== Step 4: Merge JSONs ==="
python3 -c "
import json

with open('$GYRO_JSON') as f:
    gyro_data = json.load(f)
with open('$ACCEL_JSON') as f:
    accel_data = json.load(f)

merged = {
    'accel': accel_data['accel'],
    'gyro': gyro_data['gyro'],
}
# Accept default scale [1,1,1] — real calibration would tune this
with open('$MERGED_JSON','w') as f:
    json.dump(merged, f, indent=4)
"
echo "  Merged to $MERGED_JSON"

echo ""
echo "=== Step 5: Convert to binary ==="
python3 "$SCRIPTS/convert_calib.py" "$MERGED_JSON" "$BIN" --alpha 0.2 --mouse_k 0.5
echo "  Binary: $BIN ($(wc -c < "$BIN") bytes)"

echo ""
echo "=== Step 6: Validate binary ==="
python3 -c "
import struct
with open('$BIN','rb') as f:
    data = f.read()
assert len(data) == 32, f'Expected 32 bytes, got {len(data)}'
vals = struct.unpack('3f3fff', data)
gyro_bias = vals[0:3]
gyro_scale = vals[3:6]
alpha = vals[6]
mouse_k = vals[7]
print(f'  gyro_bias = {gyro_bias}')
print(f'  gyro_scale = {gyro_scale}')
print(f'  alpha = {alpha}')
print(f'  mouse_k = {mouse_k}')
assert abs(alpha - 0.2) < 0.001, f'alpha mismatch: {alpha}'
assert abs(mouse_k - 0.5) < 0.001, f'mouse_k mismatch: {mouse_k}'
# Bias should be close to [10, -5, 2]
assert abs(gyro_bias[0] - 10.0) < 3.0, f'gyro_bias[0] off: {gyro_bias[0]}'
assert abs(gyro_bias[1] - (-5.0)) < 3.0, f'gyro_bias[1] off: {gyro_bias[1]}'
assert abs(gyro_bias[2] - 2.0) < 3.0, f'gyro_bias[2] off: {gyro_bias[2]}'
"

echo ""
echo "=== ALL INTEGRATION TESTS PASSED ==="

"""Tests for kernel-side calibration struct and validation logic.

These tests verify the calibration validation and airmouse math by
reimplementing the logic in Python (for testability without a kernel build).
"""

import pytest

# ── Reimplementation of kernel logic for testing ────────────────────

FLOAT_EPSILON = 1e-6


def fabs(val):
    if val < 0:
        return -val
    return val


def validate_calib(gyro_bias, gyro_scale, alpha, mouse_k):
    """Reimplementation of lgmagic_validate_calib."""
    for i in range(3):
        if fabs(gyro_bias[i]) > 100.0 or fabs(gyro_scale[i]) > 10.0:
            return 1
    if alpha < 0.0 or alpha > 1.0:
        return 1
    if mouse_k < 0.0 or mouse_k > 1.0:
        return 1
    return 0


def calc_mouse(gyro_bias, gyro_scale, alpha, mouse_k, gyro_acc, threshold, gyro):
    """Reimplementation of lgmagic_calc_mouse.

    gyro_acc is mutated in place (list of 3 floats).
    Returns (mouse_x, mouse_y, is_big_move).
    """
    for i in range(3):
        gyro_corr = (float(gyro[i]) - gyro_bias[i]) * gyro_scale[i]
        gyro_acc[i] = alpha * gyro_corr + (1.0 - alpha) * gyro_acc[i]

    mouse_x = int(gyro_acc[2] * mouse_k)  # gyro Z → mouse X
    mouse_y = int(gyro_acc[0] * mouse_k)  # gyro X → mouse Y

    bigmove = 0
    if fabs(gyro_acc[0]) > threshold or fabs(gyro_acc[2]) > threshold:
        bigmove = 1

    return mouse_x, mouse_y, bigmove


# ── Tests ───────────────────────────────────────────────────────────


class TestValidateCalib:
    """Calibration validation tests."""

    def test_valid_calib(self):
        assert validate_calib([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], 0.2, 0.5) == 0

    def test_bias_too_large(self):
        assert validate_calib([101.0, 0.0, 0.0], [1.0, 1.0, 1.0], 0.2, 0.5) == 1

    def test_bias_negative_too_large(self):
        assert validate_calib([0.0, -101.0, 0.0], [1.0, 1.0, 1.0], 0.2, 0.5) == 1

    def test_scale_too_large(self):
        assert validate_calib([0.0, 0.0, 0.0], [1.0, 11.0, 1.0], 0.2, 0.5) == 1

    def test_alpha_out_of_range_low(self):
        assert validate_calib([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], -0.1, 0.5) == 1

    def test_alpha_out_of_range_high(self):
        assert validate_calib([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], 1.1, 0.5) == 1

    def test_mouse_k_out_of_range(self):
        assert validate_calib([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], 0.2, 1.1) == 1

    def test_all_zeros_is_valid(self):
        """Zero calibration is technically valid (means no airmouse)."""
        assert validate_calib([0.0, 0.0, 0.0], [0.0, 0.0, 0.0], 0.0, 0.0) == 0

    def test_boundary_values(self):
        """Test exact boundary values — should be valid."""
        assert validate_calib([100.0, -100.0, 100.0], [10.0, 10.0, 10.0], 0.0, 1.0) == 0
        assert validate_calib([100.0, 100.0, 100.0], [10.0, 10.0, 10.0], 1.0, 0.0) == 0


class TestCalcMouse:
    """Airmouse calculation tests."""

    def test_no_movement_with_zero_gyro(self):
        """Zero gyro input → zero mouse output."""
        gyro_acc = [0.0, 0.0, 0.0]
        mx, my, big = calc_mouse(
            [0.0, 0.0, 0.0],  # bias
            [1.0, 1.0, 1.0],  # scale
            0.2,  # alpha
            0.5,  # mouse_k
            gyro_acc,
            300,  # threshold
            [0, 0, 0, 0, 0, 0],  # gyro raw (6 values, only first 3 used)
        )
        assert mx == 0
        assert my == 0
        assert big == 0

    def test_constant_gyro_produces_steady_output(self):
        """After LPF settles, constant gyro → constant mouse output."""
        gyro_acc = [0.0, 0.0, 0.0]
        gyro_raw = [0, 0, 0, 100, 0, 500]  # X=100, Y=0, Z=500

        results = []
        for _ in range(20):
            mx, my, big = calc_mouse(
                [0.0, 0.0, 0.0],
                [0.01, 0.01, 0.01],
                0.2,
                0.5,
                gyro_acc,
                300,
                gyro_raw,
            )
            results.append((mx, my, big))

        # LPF should settle — last few outputs should be similar
        last_mx = [r[0] for r in results[-5:]]
        assert len(set(last_mx)) <= 3  # nearly constant

    def test_threshold_detection(self):
        """Gyro above threshold → bigmove flag set."""
        gyro_acc = [0.0, 0.0, 0.0]

        # Gyro X = 50000 * scale=1 = 50000 → LPF alpha=1.0 → acc = 50000
        # threshold=300 → 50000 > 300 → bigmove=1
        _, _, big = calc_mouse(
            [0.0, 0.0, 0.0],
            [1.0, 1.0, 1.0],
            1.0,  # alpha=1 → no filtering
            0.5,
            gyro_acc,
            300,
            [50000, 0, 0, 0, 0, 0],
        )
        assert big == 1

    def test_below_threshold(self):
        """Gyro below threshold → no bigmove."""
        gyro_acc = [0.0, 0.0, 0.0]
        _, _, big = calc_mouse(
            [0.0, 0.0, 0.0],
            [0.01, 0.01, 0.01],
            1.0,
            0.5,
            gyro_acc,
            300,
            [10, 0, 0, 0, 0, 0],  # small gyro value
        )
        assert big == 0

    def test_bias_correction(self):
        """Bias is subtracted before scaling."""
        gyro_acc = [0.0, 0.0, 0.0]
        # gyro X = 100, bias X = 50 → corrected = 50 → scale 0.02 → 1.0
        # LPF alpha=1.0 → acc = 1.0 → mouse = 1.0 * 0.5 = 0
        mx, my, _ = calc_mouse(
            [50.0, 0.0, 0.0],  # bias
            [0.02, 0.02, 0.02],  # scale
            1.0,  # alpha
            0.5,  # mouse_k
            gyro_acc,
            300,
            [100, 0, 0, 0, 0, 0],
        )
        assert mx == 0  # gyro Z = 0 → no X mouse movement
        # my = gyro_acc[0] * mouse_k = (100-50)*0.02 * 0.5 = 0.5 → int=0
        assert my == 0

    def test_lpf_response(self):
        """LPF with alpha=0.5: output converges to 50% each step."""
        gyro_acc = [0.0, 0.0, 0.0]
        # gyro X=100, bias=0, scale=1, alpha=0.5, mouse_k=0 (no mouse output)
        calc_mouse([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], 0.5, 0.0, gyro_acc, 300, [100, 0, 0, 0, 0, 0])
        # After first call: gyro_acc[0] = 0.5*100 + 0.5*0 = 50
        assert gyro_acc[0] == pytest.approx(50.0)
        # Second call
        calc_mouse([0.0, 0.0, 0.0], [1.0, 1.0, 1.0], 0.5, 0.0, gyro_acc, 300, [100, 0, 0, 0, 0, 0])
        # gyro_acc[0] = 0.5*100 + 0.5*50 = 75
        assert gyro_acc[0] == pytest.approx(75.0)

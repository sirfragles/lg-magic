"""Tests for display_imu.py — coordinate transform and calibration math.

These test the pure-math functions without requiring evdev, Qt, or uinput.
We mock the hardware-dependent modules to isolate the math functions.
"""

import json
import math
import sys
import tempfile
from pathlib import Path
from unittest.mock import MagicMock

import numpy as np
import pytest

# Mock heavy dependencies before importing display_imu
sys.modules["evdev"] = MagicMock()
sys.modules["draw_cube"] = MagicMock()
sys.modules["uinput_mouse"] = MagicMock()
sys.modules["ahrs"] = MagicMock()
sys.modules["ahrs.filters"] = MagicMock()
sys.modules["ahrs.common"] = MagicMock()
sys.modules["ahrs.common.orientation"] = MagicMock()

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import display_imu  # noqa: E402  # import after path setup for test isolation


class TestApplyCalibration:
    """Test apply_calibration() math."""

    @pytest.fixture
    def sample_calib(self):
        return {
            "accel": {
                "bias": np.array([100.0, -50.0, 200.0]),
                "matrix": np.eye(3),
            },
            "gyro": {
                "bias": np.array([10.0, -5.0, 2.0]),
                "scale": np.array([1.0, 1.0, 1.0]),
            },
        }

    def test_accel_bias_only(self, sample_calib):
        """Accel calibration subtracts bias."""
        raw_accel = [150.0, 0.0, 250.0]
        raw_gyro = [0.0, 0.0, 0.0]
        a_corr, _ = display_imu.apply_calibration(raw_accel, raw_gyro, sample_calib)

        # 150 - 100 = 50, 0 - (-50) = 50, 250 - 200 = 50
        assert np.allclose(a_corr, [50.0, 50.0, 50.0])

    def test_accel_matrix_transform(self):
        """Matrix is applied after bias subtraction."""
        calib = {
            "accel": {
                "bias": np.array([0.0, 0.0, 0.0]),
                "matrix": np.array([[2.0, 0.0, 0.0], [0.0, 0.5, 0.0], [0.0, 0.0, 1.0]]),
            },
            "gyro": {
                "bias": np.array([0.0, 0.0, 0.0]),
                "scale": np.array([1.0, 1.0, 1.0]),
            },
        }
        a_corr, _ = display_imu.apply_calibration([10.0, 20.0, 30.0], [0.0, 0.0, 0.0], calib)

        assert np.allclose(a_corr, [20.0, 10.0, 30.0])

    def test_gyro_bias_correction(self, sample_calib):
        """Gyro calibration subtracts bias."""
        raw_accel = [0.0, 0.0, 0.0]
        raw_gyro = [20.0, -10.0, 5.0]
        _, g_corr = display_imu.apply_calibration(raw_accel, raw_gyro, sample_calib)

        # 20-10=10, -10-(-5)=-5, 5-2=3
        assert np.allclose(g_corr, [10.0, -5.0, 3.0])

    def test_gyro_scale(self):
        """Gyro scale multiplies after bias subtraction."""
        calib = {
            "accel": {
                "bias": np.array([0.0, 0.0, 0.0]),
                "matrix": np.eye(3),
            },
            "gyro": {
                "bias": np.array([0.0, 0.0, 0.0]),
                "scale": np.array([2.0, 0.5, 1.0]),
            },
        }
        _, g_corr = display_imu.apply_calibration([0.0, 0.0, 0.0], [10.0, 10.0, 10.0], calib)

        assert np.allclose(g_corr, [20.0, 5.0, 10.0])

    def test_both_accel_and_gyro(self, sample_calib):
        """Both calibrations applied simultaneously."""
        a_corr, g_corr = display_imu.apply_calibration(
            [150.0, -20.0, 250.0], [25.0, -10.0, 8.0], sample_calib
        )

        # Accel: [150-100, -20+50, 250-200] = [50, 30, 50]
        assert np.allclose(a_corr, [50.0, 30.0, 50.0])
        # Gyro: [25-10, -10+5, 8-2] = [15, -5, 6]
        assert np.allclose(g_corr, [15.0, -5.0, 6.0])


class TestRAlign:
    """Test the coordinate alignment rotation matrix."""

    def test_x_axis_swap(self):
        """Old Y becomes new X (negated by R_align row 0)."""
        v = np.array([0.0, 5.0, 0.0])
        result = display_imu.R_align @ v
        # Row 0: [0, -1, 0] → new X = 0*X + -1*Y + 0*Z = -5
        assert np.allclose(result, [-5.0, 0.0, 0.0])

    def test_y_axis_swap(self):
        """Old X becomes new Y (negated)."""
        v = np.array([3.0, 0.0, 0.0])
        result = display_imu.R_align @ v
        assert np.allclose(result, [0.0, -3.0, 0.0])

    def test_z_axis_flip(self):
        """Old Z becomes new Z (negated)."""
        v = np.array([0.0, 0.0, 4.0])
        result = display_imu.R_align @ v
        assert np.allclose(result, [0.0, 0.0, -4.0])

    def test_identity_on_zero(self):
        """Zero vector stays zero."""
        v = np.array([0.0, 0.0, 0.0])
        result = display_imu.R_align @ v
        assert np.allclose(result, [0.0, 0.0, 0.0])

    def test_orthogonal(self):
        """R_align should be orthogonal (rotation matrix)."""
        R = display_imu.R_align
        assert np.allclose(R @ R.T, np.eye(3), atol=1e-10)


class TestAccelToQuat:
    """Test accel_to_quat orientation computation."""

    def test_flat_face_up(self):
        """Remote flat on table, face UP (buttons toward ceiling): accel = [0, 0, +g]."""
        q = display_imu.accel_to_quat(np.array([0.0, 0.0, 9.80665]))
        # Should be near identity quaternion (no rotation)
        assert abs(q[0]) > 0.99  # w ≈ 1

    def test_upright(self):
        """Remote standing upright: accel = [0, -g, 0]."""
        q = display_imu.accel_to_quat(np.array([0.0, -9.80665, 0.0]))
        # Should represent ~90-degree pitch rotation
        assert abs(q[0]) < 1.0  # not identity
        assert abs(q[0]) > 0.5  # not fully rotated either

    def test_sideways(self):
        """Remote on its side: accel = [g, 0, 0]."""
        q = display_imu.accel_to_quat(np.array([9.80665, 0.0, 0.0]))
        assert abs(q[0]) < 1.0
        assert abs(q[0]) > 0.5

    def test_normalized(self):
        """Output quaternion should be unit length."""
        q = display_imu.accel_to_quat(np.array([3.0, -4.0, 5.0]))
        norm = math.sqrt(sum(x * x for x in q))
        assert abs(norm - 1.0) < 1e-6

    def test_zero_accel_doesnt_crash(self):
        """Zero acceleration produces NaN (division by zero) but doesn't crash."""
        import warnings

        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            _ = display_imu.accel_to_quat(np.array([0.0, 0.0, 0.0]))
        # Result is NaN quaternion — should not have raised


class TestLoadCalibration:
    """Test load_calibration() JSON parsing."""

    def test_valid_json(self):
        """Load valid calibration JSON with all fields."""
        data = {
            "accel": {"bias": [1.0, 2.0, 3.0], "matrix": [[1, 0, 0], [0, 1, 0], [0, 0, 1]]},
            "gyro": {"bias": [0.1, 0.2, 0.3], "scale": [1.0, 1.0, 1.0]},
        }
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json.dump(data, f)
            fname = f.name

        calib = display_imu.load_calibration(fname)
        Path(fname).unlink()

        assert isinstance(calib["accel"]["matrix"], np.ndarray)
        assert isinstance(calib["accel"]["bias"], np.ndarray)
        assert np.allclose(calib["accel"]["bias"], [1.0, 2.0, 3.0])

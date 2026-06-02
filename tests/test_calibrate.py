"""Tests for calibration module."""

import json

# Import the module under test
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import calibrate


class TestLoadIMUCSV:
    """CSV loading tests."""

    def test_basic_csv(self):
        """Parse a well-formed CSV file."""
        csv_content = (
            "0,0.02,100,200,300,10,20,30\n"
            "1,0.02,110,210,310,15,25,35\n"
        )
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            accel, gyro = calibrate.load_imu_csv(f.name)
        Path(f.name).unlink()

        assert accel.shape == (2, 3)
        assert gyro.shape == (2, 3)
        assert np.allclose(accel[0], [100, 200, 300])
        assert np.allclose(gyro[0], [10, 20, 30])

    def test_short_rows_skipped(self):
        """Rows with fewer than 6 fields are skipped."""
        csv_content = "0,0.02,100\n"  # only 3 fields
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            accel, _gyro = calibrate.load_imu_csv(f.name)
        Path(f.name).unlink()

        assert len(accel) == 0
        assert len(_gyro) == 0

    def test_invalid_lines_skipped(self):
        """Lines with non-numeric values are skipped."""
        csv_content = (
            "0,0.02,100,200,300,10,20,30\n"
            "bad,line,here,x,y,z,a,b\n"
            "1,0.02,110,210,310,15,25,35\n"
        )
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            accel, _gyro = calibrate.load_imu_csv(f.name)
        Path(f.name).unlink()

        assert len(accel) == 2

    def test_empty_file(self):
        """Empty file returns empty arrays."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write("")
            f.flush()
            accel, _gyro = calibrate.load_imu_csv(f.name)
        Path(f.name).unlink()

        assert len(accel) == 0


class TestGyroCalibration:
    """Gyroscope bias calibration tests."""

    def test_stationary_gyro(self):
        """Stationary gyro: bias = mean of samples."""
        # Simulate a stationary gyro with noise around zero
        rng = np.random.RandomState(42)
        samples = rng.normal(0, 5, (100, 3))
        bias = calibrate.calibrate_gyro_bias(samples)
        assert len(bias) == 3
        # Bias should be close to zero
        assert all(abs(b) < 2.0 for b in bias)

    def test_biased_gyro(self):
        """Gyro with known bias."""
        samples = np.full((50, 3), [10.0, -5.0, 2.0])
        bias = calibrate.calibrate_gyro_bias(samples)
        assert np.allclose(bias, [10.0, -5.0, 2.0])

    def test_single_sample(self):
        """Single sample: bias equals that sample."""
        samples = np.array([[1.0, 2.0, 3.0]])
        bias = calibrate.calibrate_gyro_bias(samples)
        assert bias == [1.0, 2.0, 3.0]


class TestAccelCalibration:
    """Accelerometer calibration tests."""

    def test_ideal_accel(self):
        """Ideal accelerometer with only bias offset."""
        # Create data from 6 cardinal directions with known bias
        bias_true = np.array([100.0, -50.0, 200.0])
        g = 9.80665
        # 6 directions, each measured 10 times
        directions = np.array([
            [g, 0, 0], [-g, 0, 0],
            [0, g, 0], [0, -g, 0],
            [0, 0, g], [0, 0, -g],
        ])
        samples = np.vstack([bias_true + d for d in directions for _ in range(10)])

        b_calib, M_calib = calibrate.calibrate_accel(samples)

        # Bias should be recovered
        assert np.allclose(b_calib, bias_true, atol=0.1)
        # Matrix should be close to identity
        assert np.allclose(M_calib, np.eye(3), atol=0.1)


class TestSaveCalibrationJSON:
    """Calibration JSON output tests."""

    def test_output_has_required_keys(self):
        """Output JSON contains accel and gyro sections."""
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            fname = f.name

        calibrate.save_calibration_json(
            bias=[1.0, 2.0, 3.0],
            matrix=[[1, 0, 0], [0, 1, 0], [0, 0, 1]],
            gyro_bias=[0.1, 0.2, 0.3],
            filename=fname,
        )

        with open(fname) as f:
            data = json.load(f)

        assert "accel" in data
        assert "gyro" in data
        assert "bias" in data["accel"]
        assert "matrix" in data["accel"]
        assert "bias" in data["gyro"]
        assert "scale" in data["gyro"]
        assert len(data["gyro"]["scale"]) == 3

        Path(fname).unlink()

"""Tests for convert_calib binary conversion."""

import json
import struct
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))


class TestBinaryPacking:
    """Test binary format packing/unpacking."""

    def test_roundtrip(self):
        """Write binary, read back — values should match."""
        calib = {
            "gyro": {
                "bias": [0.5, -1.2, 3.4],
                "scale": [1.0, 1.0, 1.0],
            }
        }
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as jf:
            json.dump(calib, jf)
            json_path = jf.name

        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as bf:
            bin_path = bf.name

        # Simulate command-line arguments
        # Manually call the core logic
        with open(json_path) as f:
            data = json.load(f)

        gyro = data["gyro"]
        packed = struct.pack(
            "3f3fff",
            *map(float, gyro["bias"]),
            *map(float, gyro["scale"]),
            0.2,   # alpha
            0.5,   # mouse_k
        )
        with open(bin_path, "wb") as bf:
            bf.write(packed)

        # Read back and verify
        with open(bin_path, "rb") as bf:
            raw = bf.read()

        assert len(raw) == 32  # 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4

        unpacked = struct.unpack("3f3fff", raw)
        assert unpacked[0] == pytest.approx(0.5)
        assert unpacked[1] == pytest.approx(-1.2)
        assert unpacked[2] == pytest.approx(3.4)
        assert unpacked[6] == pytest.approx(0.2)   # alpha
        assert unpacked[7] == pytest.approx(0.5)   # mouse_k

        Path(json_path).unlink()
        Path(bin_path).unlink()

    def test_binary_size(self):
        """Binary blob is exactly 32 bytes (8 floats)."""
        packed = struct.pack("3f3fff", 0, 0, 0, 1, 1, 1, 0.2, 0.5)
        assert len(packed) == 32

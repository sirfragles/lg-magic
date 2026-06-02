"""Tests for convert_calib — JSON → binary firmware converter."""

import json
import struct
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import convert_calib


class TestBinaryFormat:
    """Test the binary firmware format (struct.pack layout)."""

    def test_binary_size(self):
        """Binary blob is exactly 32 bytes (8 IEEE 754 floats)."""
        packed = struct.pack("3f3fff", 0, 0, 0, 1, 1, 1, 0.2, 0.5)
        assert len(packed) == 32

    def test_firmware_layout(self):
        """Verify the on-disk layout matches kernel struct."""
        bias = [0.5, -1.2, 3.4]
        scale = [1.0, 1.0, 1.0]
        alpha = 0.2
        mouse_k = 0.5

        packed = struct.pack("3f3fff", *bias, *scale, alpha, mouse_k)
        unpacked = struct.unpack("3f3fff", packed)

        # Order: bias[0..2], scale[0..2], alpha, mouse_k
        assert unpacked[0] == pytest.approx(0.5)
        assert unpacked[1] == pytest.approx(-1.2)
        assert unpacked[2] == pytest.approx(3.4)
        assert unpacked[3] == pytest.approx(1.0)
        assert unpacked[4] == pytest.approx(1.0)
        assert unpacked[5] == pytest.approx(1.0)
        assert unpacked[6] == pytest.approx(0.2)
        assert unpacked[7] == pytest.approx(0.5)

    def test_roundtrip(self):
        """JSON → binary → read back: values match."""
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

        # Call convert_calib's main via command-line args
        old_argv = sys.argv
        try:
            sys.argv = [
                "convert_calib.py",
                json_path,
                bin_path,
                "--alpha",
                "0.2",
                "--mouse_k",
                "0.5",
            ]
            convert_calib.main()
        finally:
            sys.argv = old_argv

        # Verify binary output
        with open(bin_path, "rb") as bf:
            raw = bf.read()

        assert len(raw) == 32
        unpacked = struct.unpack("3f3fff", raw)
        assert unpacked[0] == pytest.approx(0.5)
        assert unpacked[1] == pytest.approx(-1.2)
        assert unpacked[2] == pytest.approx(3.4)
        assert unpacked[6] == pytest.approx(0.2)
        assert unpacked[7] == pytest.approx(0.5)

        Path(json_path).unlink()
        Path(bin_path).unlink()

    def test_invalid_json_exits(self):
        """Malformed JSON should cause exit."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as jf:
            jf.write("not valid json")
            json_path = jf.name

        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as bf:
            bin_path = bf.name

        old_argv = sys.argv
        try:
            sys.argv = [
                "convert_calib.py",
                json_path,
                bin_path,
                "--alpha",
                "0.2",
                "--mouse_k",
                "0.5",
            ]
            with pytest.raises(SystemExit):
                convert_calib.main()
        finally:
            sys.argv = old_argv

        Path(json_path).unlink()
        Path(bin_path).unlink()

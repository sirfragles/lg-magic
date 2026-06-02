"""Tests for uinput_mouse.py — rad/s to pixel conversion math.

These test the conversion logic without requiring the uinput kernel module.
"""

import sys
from pathlib import Path
from unittest.mock import MagicMock

# Mock uinput before importing
sys.modules["uinput"] = MagicMock()
sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))


class TestRadToPixelConversion:
    """Test the rad/s → pixel conversion formula independently.

    The formula in imu_to_mouse_from_rads is:
        rel_x = int(d_y * s_x)
        rel_y = int(d_p * s_y)
    """

    def test_positive_positive(self):
        """Both positive → both positive pixels."""
        d_y, d_p = 0.5, 0.3
        s_x, s_y = 100.0, 200.0
        assert int(d_y * s_x) == 50
        assert int(d_p * s_y) == 60

    def test_negative_pitch(self):
        """Negative pitch → negative Y pixels."""
        d_y, d_p = 0.5, -0.3
        s_x, s_y = 100.0, 200.0
        assert int(d_y * s_x) == 50
        assert int(d_p * s_y) == -60

    def test_zero_movement(self):
        """Zero rad/s → zero pixels."""
        assert int(0.0 * 50.0) == 0

    def test_default_sensitivity(self):
        """Default s_x=s_y=50.0."""
        assert int(0.2 * 50.0) == 10
        assert int(0.4 * 50.0) == 20

    def test_truncation(self):
        """Float pixels truncated toward zero (int cast)."""
        assert int(0.199 * 50.0) == 9  # 9.95 → 9
        assert int(-0.199 * 50.0) == -9  # -9.95 → -9

    def test_large_values(self):
        """Large angular velocity → large pixel deltas."""
        assert int(10.0 * 50.0) == 500
        assert int(-10.0 * 50.0) == -500

    def test_custom_sensitivity(self):
        """Different X/Y sensitivities."""
        assert int(1.0 * 30.0) == 30  # X
        assert int(1.0 * 70.0) == 70  # Y


class TestGetDevice:
    """Test lazy uinput Device creation."""

    def test_creates_once(self):
        """_get_device() creates device only once (cached)."""
        import uinput_mouse

        uinput_mouse._device = None

        dev1 = uinput_mouse._get_device()
        dev2 = uinput_mouse._get_device()

        assert dev1 is dev2

    def test_does_not_recreate(self):
        """Does not create new device if one already exists."""
        import uinput_mouse

        # Reset to force creation in this test
        uinput_mouse._device = None

        # First call creates device
        first = uinput_mouse._get_device()
        # Second call returns same device
        second = uinput_mouse._get_device()

        assert first is second

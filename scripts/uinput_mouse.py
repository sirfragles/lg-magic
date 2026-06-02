"""Virtual mouse device via uinput for LG Magic Remote airmouse.

Lazy-initializes the uinput device on first use to avoid side effects
on import and ensure proper cleanup on exit.
"""

import atexit
from contextlib import suppress

import numpy as np
import uinput

_device = None


def _get_device():
    """Create (or return existing) uinput virtual mouse device."""
    global _device
    if _device is None:
        _device = uinput.Device([uinput.REL_X, uinput.REL_Y, uinput.BTN_LEFT, uinput.BTN_RIGHT])
    return _device


def _cleanup():
    """Destroy the uinput device on exit."""
    global _device
    if _device is not None:
        with suppress(Exception):
            _device.destroy()
        _device = None


atexit.register(_cleanup)


def imu_to_mouse_from_rads(d_y, d_p, s_x=50.0, s_y=50.0):
    """Convert angular velocity (rad/s) to relative mouse pixel deltas.

    Args:
        d_y: Yaw angular velocity (rad/s) → horizontal mouse movement.
        d_p: Pitch angular velocity (rad/s) → vertical mouse movement.
        s_x: Horizontal sensitivity (pixels per rad/s).
        s_y: Vertical sensitivity (pixels per rad/s).
    """
    rel_x = int(d_y * s_x)
    rel_y = int(d_p * s_y)

    dev = _get_device()
    dev.emit(uinput.REL_X, rel_x, syn=False)
    dev.emit(uinput.REL_Y, rel_y, syn=True)


# ── Euler-angle-based variant (currently unused, kept for reference) ──

_prev_pitch = None
_prev_yaw = None


def imu_to_mouse_from_euler(euler, dt, s_x=50.0, s_y=50.0):
    """Convert Euler angles to mouse deltas (experimental).

    Uses pitch/yaw differences over dt to compute angular velocity,
    then delegates to imu_to_mouse_from_rads.
    """
    global _prev_pitch, _prev_yaw

    _roll, pitch, yaw = euler

    if _prev_pitch is None:
        _prev_pitch, _prev_yaw = pitch, yaw
        return

    delta_yaw = yaw - _prev_yaw
    delta_pitch = pitch - _prev_pitch

    _prev_pitch, _prev_yaw = pitch, yaw

    # Wrap around -pi..pi
    delta_yaw = (delta_yaw + np.pi) % (2 * np.pi) - np.pi
    delta_pitch = (delta_pitch + np.pi) % (2 * np.pi) - np.pi

    delta_yaw /= dt
    delta_pitch /= dt
    imu_to_mouse_from_rads(delta_yaw, delta_pitch, s_x, s_y)

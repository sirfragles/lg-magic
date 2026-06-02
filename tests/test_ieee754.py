"""Tests for the IEEE 754 float → fixed-point decoder.

The C kernel module uses ieee754_f32_to_fp() to parse calibration
firmware without C floating-point operations.  These tests verify the
algorithm against known IEEE 754 values using Python's struct module
as a reference.
"""

import struct

import pytest

# ── Python reimplementation of the C ieee754_f32_to_fp() ──────────

SCALE = 65536  # LGMAGIC_FP_SCALE
SCALE_S32 = 65536


def ieee754_f32_to_fp_s64(bytes_in, scale=SCALE):
    """Reimplementation of C ieee754_f32_to_fp() for s64 output."""
    assert len(bytes_in) == 4

    raw = bytes_in[0] | (bytes_in[1] << 8) | (bytes_in[2] << 16) | (bytes_in[3] << 24)
    sign = raw >> 31
    exp = ((raw >> 23) & 0xFF) - 127
    mantissa = raw & 0x7FFFFF

    if exp == 128:  # NaN or Inf
        return 0

    if exp > -127:
        mantissa |= 1 << 23  # normalized: add implicit 1
    else:
        exp = -126
        if mantissa == 0:
            return 0

    result = mantissa * scale
    shift = exp - 23

    if shift >= 0:
        if shift < 63:
            result <<= shift
        else:
            return 0
    else:
        shift = -shift
        if shift < 63:
            result >>= shift
        else:
            return 0

    return -result if sign else result


def float_to_bytes(value):
    """Convert Python float to IEEE 754 little-endian bytes."""
    return struct.pack("<f", value)


# ── Tests ─────────────────────────────────────────────────────────


class TestIeee754Decoder:
    """Test IEEE 754 f32 → fixed-point conversion."""

    def test_zero(self):
        """0.0 → 0."""
        assert ieee754_f32_to_fp_s64(float_to_bytes(0.0)) == 0

    def test_one(self):
        """1.0 → 65536."""
        assert ieee754_f32_to_fp_s64(float_to_bytes(1.0)) == 65536

    def test_half(self):
        """0.5 → 32768."""
        assert ieee754_f32_to_fp_s64(float_to_bytes(0.5)) == 32768

    def test_negative(self):
        """-1.0 → -65536."""
        assert ieee754_f32_to_fp_s64(float_to_bytes(-1.0)) == -65536

    def test_typical_gyro_bias(self):
        """Typical gyro bias value: 50.0."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(50.0))
        assert val == 50 * 65536  # 3,276,800

    def test_typical_alpha(self):
        """LPF alpha: 0.2."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(0.2))
        assert abs(val - int(0.2 * 65536)) <= 1  # within 1 LSB

    def test_typical_mouse_k(self):
        """Mouse sensitivity: 0.07."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(0.07))
        assert abs(val - int(0.07 * 65536)) <= 1

    def test_typical_negative_bias(self):
        """Negative gyro bias: -15.3."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(-15.3))
        expected = int(-15.3 * 65536)
        assert abs(val - expected) <= 2  # rounding diff

    def test_small_positive(self):
        """Very small value: 0.0001."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(0.0001))
        # 0.0001 * 65536 = 6.5536 → int truncation → 6
        assert 0 <= val <= 10

    def test_large_positive(self):
        """Large but valid: 99.9 (max bias)."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(99.9))
        expected = int(99.9 * 65536)
        assert abs(val - expected) <= 2

    def test_scale_value(self):
        """Gyro scale: 0.07."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(0.07))
        # For s32 conversion we'd cast; as s64 it's just a positive value
        assert val == int(0.07 * 65536)  # should be exact for 0.07? maybe not
        # Tolerance: 1 LSB
        assert abs(val - 4587) <= 1  # 0.07 * 65536 ≈ 4587.52

    @pytest.mark.parametrize(
        "value",
        [
            0.0,
            1.0,
            -1.0,
            0.5,
            50.0,
            0.2,
            0.07,
            -15.3,
            99.9,
            0.0001,
            -0.0001,
            1e-5,
            -1e-5,
        ],
    )
    def test_roundtrip_accuracy(self, value):
        """Decoder result should be within 2 LSB of struct.unpack."""
        bytes_in = float_to_bytes(value)
        decoded = ieee754_f32_to_fp_s64(bytes_in)
        expected = int(value * SCALE)
        # Allow ±2 LSB tolerance for rounding differences
        assert abs(decoded - expected) <= 2

    def test_nan_returns_zero(self):
        """NaN should decode to 0."""
        nan_bytes = struct.pack("<f", float("nan"))
        assert ieee754_f32_to_fp_s64(nan_bytes) == 0

    def test_inf_returns_zero(self):
        """Infinity should decode to 0."""
        inf_bytes = struct.pack("<f", float("inf"))
        assert ieee754_f32_to_fp_s64(inf_bytes) == 0

    def test_neg_inf_returns_zero(self):
        """Negative infinity should decode to 0."""
        neg_inf_bytes = struct.pack("<f", float("-inf"))
        assert ieee754_f32_to_fp_s64(neg_inf_bytes) == 0

    def test_subnormal_small(self):
        """Very small subnormal number decodes near zero."""
        # Smallest positive subnormal: 1.4e-45
        sub_bytes = struct.pack("<f", 1.4e-45)
        val = ieee754_f32_to_fp_s64(sub_bytes)
        # Should be 0 after truncation
        assert val == 0

    def test_sign_symmetry(self):
        """+v and -v should be exact opposites."""
        for v in [1.0, 50.0, 0.2, 0.07]:
            pos = ieee754_f32_to_fp_s64(float_to_bytes(v))
            neg = ieee754_f32_to_fp_s64(float_to_bytes(-v))
            assert pos == -neg


class TestIeee754EdgeCases:
    """Edge cases for the decoder."""

    def test_max_normalized(self):
        """Largest normalized float decodes correctly."""
        val = ieee754_f32_to_fp_s64(float_to_bytes(3.4e38))
        # Should overflow → return 0
        assert val == 0

    def test_exact_powers_of_two(self):
        """Powers of two should be exact."""
        for exp in range(-10, 11):
            v = 2.0**exp
            decoded = ieee754_f32_to_fp_s64(float_to_bytes(v))
            expected = int(v * SCALE)
            assert decoded == expected, f"2^{exp} failed: {decoded} != {expected}"

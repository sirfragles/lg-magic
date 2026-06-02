/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  This is part of lg_magic_dkms
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include "lg_magic_airmouse.h"

/*
 * Decode a 4-byte IEEE 754 single-precision float (little-endian) to a
 * fixed-point s64 value.  No C floating-point operations are used.
 *
 * Returns (s64)(ieee754_value * scale), truncated toward zero.
 * Returns 0 for NaN and Infinity inputs.
 */
static s64 ieee754_f32_to_fp(const u8 *bytes, s32 scale)
{
	u32 raw, sign, mantissa;
	s32 exp, shift;
	s64 result;

	/* Decode little-endian IEEE 754 binary32 */
	raw = (u32)bytes[0] | ((u32)bytes[1] << 8) |
	      ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);

	sign = raw >> 31;
	exp = (s32)((raw >> 23) & 0xFF) - 127; /* unbiased exponent */
	mantissa = raw & 0x7FFFFF;              /* 23-bit fraction */

	/* NaN or Infinity → treat as zero (calibration should never be) */
	if (exp == 128)
		return 0;

	if (exp > -127) {
		/* Normalized: add implicit leading 1 at bit 23 */
		mantissa |= 1U << 23;
	} else {
		/* Subnormal or zero: fix exponent, no implicit 1 */
		exp = -126;
		if (mantissa == 0)
			return 0;
	}

	/*
	 * ieee754_value = (-1)^sign * mantissa * 2^(exp - 23)
	 *
	 * We want:  result = value * scale
	 *          result = (-1)^sign * mantissa * scale * 2^(exp - 23)
	 *
	 * Compute with 64-bit intermediate to avoid overflow, then
	 * apply the power-of-two factor via shift.
	 */
	result = (s64)mantissa * (s64)scale;

	shift = exp - 23;
	if (shift >= 0) {
		if (shift < 63)
			result <<= shift;
		else
			return 0; /* overflow — shouldn't happen with calib values */
	} else {
		shift = -shift;
		if (shift < 63)
			result >>= shift;
		else
			return 0;
	}

	return sign ? -result : result;
}

/*
 * Validate fixed-point calibration values are within reasonable bounds.
 *
 * Bias:   ±100 * SCALE  (raw gyro units, scaled)
 * Scale:  ±10  * SCALE  (sensitivity multiplier, scaled)
 * Alpha:   0..SCALE      (LPF coefficient)
 * MouseK:  0..SCALE      (mouse sensitivity)
 */
int lgmagic_validate_calib_fp(struct lg_magic_airmouse_calib_fp *calib)
{
	s64 bias_max = 100LL * LGMAGIC_FP_SCALE;
	s32 scale_max = 10 * LGMAGIC_FP_SCALE;
	int i;

	for (i = 0; i < 3; i++) {
		if (calib->gyro_bias[i] > bias_max ||
		    calib->gyro_bias[i] < -bias_max)
			return 1;
		if (calib->gyro_scale[i] > scale_max ||
		    calib->gyro_scale[i] < -scale_max)
			return 1;
	}
	if (calib->alpha < 0 || calib->alpha > LGMAGIC_FP_SCALE)
		return 1;
	if (calib->mouse_k < 0 || calib->mouse_k > LGMAGIC_FP_SCALE)
		return 1;
	return 0;
}

/*
 * Convert on-disk IEEE 754 calibration blob to fixed-point runtime format.
 *
 * The firmware blob layout is 8 consecutive IEEE 754 single-precision
 * floats (little-endian, 32 bytes total):
 *   [0..3]   gyro_bias[0]
 *   [4..7]   gyro_bias[1]
 *   [8..11]  gyro_bias[2]
 *   [12..15] gyro_scale[0]
 *   [16..19] gyro_scale[1]
 *   [20..23] gyro_scale[2]
 *   [24..27] alpha
 *   [28..31] mouse_k
 *
 * Each value is decoded to fixed-point via ieee754_f32_to_fp().
 * No C floating-point operations are used.
 */
int lgmagic_convert_calib_to_fp(const u8 *fw_data, size_t fw_size,
				struct lg_magic_airmouse_calib_fp *dst)
{
	const u8 *p = fw_data;

	if (fw_size < 32)
		return -EINVAL;

	dst->gyro_bias[0]  = ieee754_f32_to_fp(p + 0, LGMAGIC_FP_SCALE);
	dst->gyro_bias[1]  = ieee754_f32_to_fp(p + 4, LGMAGIC_FP_SCALE);
	dst->gyro_bias[2]  = ieee754_f32_to_fp(p + 8, LGMAGIC_FP_SCALE);
	dst->gyro_scale[0] = (s32)ieee754_f32_to_fp(p + 12, LGMAGIC_FP_SCALE);
	dst->gyro_scale[1] = (s32)ieee754_f32_to_fp(p + 16, LGMAGIC_FP_SCALE);
	dst->gyro_scale[2] = (s32)ieee754_f32_to_fp(p + 20, LGMAGIC_FP_SCALE);
	dst->alpha         = (s32)ieee754_f32_to_fp(p + 24, LGMAGIC_FP_SCALE);
	dst->mouse_k       = (s32)ieee754_f32_to_fp(p + 28, LGMAGIC_FP_SCALE);

	return 0;
}

/*
 * Fixed-point airmouse calculation (no kernel FPU usage).
 *
 * Data-flow (all values in fixed-point, scale = LGMAGIC_FP_SCALE = 65536):
 *
 *   1.  diff       =  gyro_raw(i) * SCALE  -  bias_fp(i)           (s64)
 *   2.  gyro_corr  =  diff * scale_fp(i) / SCALE                   (s64)
 *   3.  gyro_acc   =  alpha*gyro_corr + (1-alpha)*gyro_acc         (s64, LPF)
 *   4.  mouse      =  gyro_acc * mouse_k / SCALE^2                 (s16 px)
 *   5.  |gyro_acc| > threshold * SCALE  →  bigmove = 1
 *
 * Steps 1-3 operate in fixed-point with SCALE factor.
 * Step 4 converts to integer pixels (mouse_k is also SCALE-scaled,
 * so product is SCALE^2-scaled; divide twice for integer output).
 */
int lgmagic_calc_mouse(struct lg_magic_airmouse_calib_fp *calib,
		       s64 *gyro_acc, u16 threshold,
		       s16 *gyro, s16 *mouse)
{
	s64 threshold_fp = (s64)threshold * LGMAGIC_FP_SCALE;
	int i;

	for (i = 0; i < 3; i++) {
		s64 diff, gyro_corr;

		/* 1. Bias correction: scale raw s16 up, subtract bias */
		diff = (s64)gyro[i] * LGMAGIC_FP_SCALE - calib->gyro_bias[i];

		/* 2. Apply sensitivity scale */
		gyro_corr = (diff * (s64)calib->gyro_scale[i])
			    >> LGMAGIC_FP_SHIFT;

		/*
		 * 3. Low-pass filter:
		 *    acc = (alpha*corr + (SCALE-alpha)*acc) / SCALE
		 *
		 *    alpha is in [0, SCALE], so (SCALE-alpha) is non-negative.
		 */
		gyro_acc[i] = ((s64)calib->alpha * gyro_corr
			       + ((s64)LGMAGIC_FP_SCALE - calib->alpha)
				 * gyro_acc[i])
			      >> LGMAGIC_FP_SHIFT;
	}

	/*
	 * 4. Fixed-point → integer pixels.
	 *    gyro_acc is SCALE-scaled, mouse_k is SCALE-scaled.
	 *    Product is SCALE^2-scaled — divide by SCALE^2 for s16 pixels.
	 *
	 *    Gyro axes → mouse axes:  Z-rotation → X-mouse, X-rotation → Y-mouse
	 */
	mouse[0] = (s16)((gyro_acc[2] * (s64)calib->mouse_k)
			 / ((s64)LGMAGIC_FP_SCALE * LGMAGIC_FP_SCALE));
	mouse[1] = (s16)((gyro_acc[0] * (s64)calib->mouse_k)
			 / ((s64)LGMAGIC_FP_SCALE * LGMAGIC_FP_SCALE));

	/* 5. Threshold detection: switch to airmouse mode on large motion */
	if (gyro_acc[0] > threshold_fp || gyro_acc[0] < -threshold_fp ||
	    gyro_acc[2] > threshold_fp || gyro_acc[2] < -threshold_fp)
		return 1;
	return 0;
}

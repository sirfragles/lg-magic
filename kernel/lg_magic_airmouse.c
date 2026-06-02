/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  LG Magic Remote — fixed-point airmouse math
 *
 *  All arithmetic uses 64-bit integers with 16-bit fractional scale.
 *  Zero C floating-point operations in any code path.
 *
 *  Copyright (C) 2025 Ilya Chelyadin <ilya77105@gmail.com>
 */

#include <linux/types.h>
#include <linux/errno.h>
#include "lg_magic_airmouse.h"

/* ── IEEE 754 binary32 → fixed-point decoder ──────────────────────── */

/*
 * Decode a 4-byte little-endian IEEE 754 single-precision float
 * and multiply by 'scale'.  No C float types are used.
 *
 * Returns 0 for NaN, Infinity, and unreasonably large values.
 */
static s64 ieee754_to_fp(const u8 *bytes, s32 scale)
{
	u32 raw, sign, mantissa;
	s32 exp, shift;
	s64 result;

	/* Decode LE bytes */
	raw = (u32)bytes[0] | ((u32)bytes[1] << 8)
	      | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);

	sign     = raw >> 31;
	exp      = ((s32)(raw >> 23) & 0xFF) - 127;
	mantissa = raw & 0x7FFFFF;

	/* NaN / Inf → treat as zero (should never appear in valid calib) */
	if (exp == 128)
		return 0;

	if (exp > -127)       /* normalized: add implicit leading 1 */
		mantissa |= 1U << 23;
	else if (mantissa == 0)
		return 0;      /* zero */
	else
		exp = -126;    /* subnormal: fix exponent, no implicit 1 */

	/*
	 * IEEE 754 value = (-1)^sign · mantissa · 2^(exp - 23)
	 * Fixed-point    = value · scale
	 *                = (-1)^sign · mantissa · scale · 2^(exp - 23)
	 */
	result = (s64)mantissa * (s64)scale;

	shift = exp - 23;
	if (shift >= 0) {
		if (shift > 62)
			return 0;  /* overflow guard */
		result <<= shift;
	} else {
		shift = -shift;
		if (shift > 62)
			return 0;
		result >>= shift;
	}

	return sign ? -result : result;
}

/* ── Calibration validation ───────────────────────────────────────── */

int lgmagic_validate_calib_fp(struct lg_magic_airmouse_calib_fp *calib)
{
	static const s64 bias_max  = 100LL * LGMAGIC_FP_SCALE;
	static const s32 scale_max =  10   * LGMAGIC_FP_SCALE;
	int i;

	for (i = 0; i < 3; i++) {
		if (calib->gyro_bias[i]  >  bias_max ||
		    calib->gyro_bias[i]  < -bias_max ||
		    calib->gyro_scale[i] >  scale_max ||
		    calib->gyro_scale[i] < -scale_max)
			return -EINVAL;
	}

	if (calib->alpha   < 0 || calib->alpha   > LGMAGIC_FP_SCALE ||
	    calib->mouse_k < 0 || calib->mouse_k > LGMAGIC_FP_SCALE)
		return -EINVAL;

	return 0;
}

/* ── Firmware → fixed-point conversion ────────────────────────────── */

int lgmagic_convert_calib_to_fp(const u8 *fw_data, size_t fw_size,
				struct lg_magic_airmouse_calib_fp *dst)
{
	if (fw_size < 32)
		return -EINVAL;

	/*
	 * Firmware layout (8 × f32 LE, 32 bytes total):
	 *   [ 0.. 3]  gyro_bias[0]
	 *   [ 4.. 7]  gyro_bias[1]
	 *   [ 8..11]  gyro_bias[2]
	 *   [12..15]  gyro_scale[0]
	 *   [16..19]  gyro_scale[1]
	 *   [20..23]  gyro_scale[2]
	 *   [24..27]  alpha
	 *   [28..31]  mouse_k
	 */
	dst->gyro_bias[0]  = ieee754_to_fp(fw_data +  0, LGMAGIC_FP_SCALE);
	dst->gyro_bias[1]  = ieee754_to_fp(fw_data +  4, LGMAGIC_FP_SCALE);
	dst->gyro_bias[2]  = ieee754_to_fp(fw_data +  8, LGMAGIC_FP_SCALE);
	dst->gyro_scale[0] = (s32)ieee754_to_fp(fw_data + 12, LGMAGIC_FP_SCALE);
	dst->gyro_scale[1] = (s32)ieee754_to_fp(fw_data + 16, LGMAGIC_FP_SCALE);
	dst->gyro_scale[2] = (s32)ieee754_to_fp(fw_data + 20, LGMAGIC_FP_SCALE);
	dst->alpha         = (s32)ieee754_to_fp(fw_data + 24, LGMAGIC_FP_SCALE);
	dst->mouse_k       = (s32)ieee754_to_fp(fw_data + 28, LGMAGIC_FP_SCALE);

	return 0;
}

/* ── Airmouse pipeline ────────────────────────────────────────────── */

bool lgmagic_calc_mouse(struct lg_magic_airmouse_calib_fp *calib,
			s64 *filter_state, u16 threshold,
			const s16 *gyro_raw,
			s16 *mouse_dx_out, s16 *mouse_dy_out)
{
	const s64 threshold_fp = (s64)threshold * LGMAGIC_FP_SCALE;
	int axis;

	for (axis = 0; axis < 3; axis++) {
		s64 diff, corr;

		/* 1. Subtract bias (both scaled by SCALE) */
		diff = (s64)gyro_raw[axis] * LGMAGIC_FP_SCALE
		       - calib->gyro_bias[axis];

		/* 2. Apply sensitivity scale, divide back to SCALE range */
		corr = (diff * (s64)calib->gyro_scale[axis])
		       >> LGMAGIC_FP_SHIFT;

		/*
		 * 3. Low-pass filter (exponential moving average):
		 *      new = (α · sample + (1−α) · old) / SCALE
		 *
		 *    α is stored as α_fp ∈ [0, SCALE].
		 *    (SCALE − α_fp) is the complementary weight.
		 *    The sum is divided by SCALE via right-shift.
		 */
		filter_state[axis] =
			((s64)calib->alpha * corr
			 + ((s64)LGMAGIC_FP_SCALE - calib->alpha)
			   * filter_state[axis])
			>> LGMAGIC_FP_SHIFT;
	}

	/*
	 * 4. Fixed-point → integer pixels.
	 *
	 *    filter_state is SCALE-scaled, mouse_k is SCALE-scaled.
	 *    Product is SCALE²-scaled → divide by SCALE² for s16 pixels.
	 *
	 *    Axis mapping: gyro Z (yaw)  → horizontal mouse  (X)
	 *                  gyro X (roll) → vertical mouse    (Y)
	 */
	*mouse_dx_out = (s16)((filter_state[2] * (s64)calib->mouse_k)
			      / LGMAGIC_FP_SCALE_SQ);
	*mouse_dy_out = (s16)((filter_state[0] * (s64)calib->mouse_k)
			      / LGMAGIC_FP_SCALE_SQ);

	/*
	 * 5. Threshold check — enter airmouse mode if yaw or roll
	 *    exceeds the configured threshold.
	 */
	return (filter_state[0] >  threshold_fp ||
		filter_state[0] < -threshold_fp ||
		filter_state[2] >  threshold_fp ||
		filter_state[2] < -threshold_fp);
}

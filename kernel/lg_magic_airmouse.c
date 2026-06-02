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
#include "lg_magic_airmouse.h"

/*
 * Validate the on-disk (float) calibration blob.
 * Uses float comparisons intentionally — this runs once at probe time,
 * not in the hot HID event path.
 */
int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (calib->gyro_bias[i] < -100.0f ||
		    calib->gyro_bias[i] > 100.0f ||
		    calib->gyro_scale[i] < -10.0f ||
		    calib->gyro_scale[i] > 10.0f)
			return 1;
	}
	if (calib->alpha < 0.0f || calib->alpha > 1.0f)
		return 1;
	if (calib->mouse_k < 0.0f || calib->mouse_k > 1.0f)
		return 1;
	return 0;
}

/*
 * Convert on-disk float calibration to fixed-point runtime format.
 * Called once at probe after successful firmware load + validation.
 */
int lgmagic_convert_calib_to_fp(struct lg_magic_airmouse_calib *src,
				struct lg_magic_airmouse_calib_fp *dst)
{
	int i;

	for (i = 0; i < 3; i++) {
		dst->gyro_bias[i] =
			(s64)(src->gyro_bias[i] * (float)LGMAGIC_FP_SCALE);
		dst->gyro_scale[i] =
			(s32)(src->gyro_scale[i] * (float)LGMAGIC_FP_SCALE);
	}
	dst->alpha = (s32)(src->alpha * (float)LGMAGIC_FP_SCALE);
	dst->mouse_k = (s32)(src->mouse_k * (float)LGMAGIC_FP_SCALE);
	return 0;
}

/*
 * Fixed-point airmouse calculation.
 *
 * Data-flow (all values in fixed-point with scale = LGMAGIC_FP_SCALE):
 *
 *   1.  diff       = gyro_raw(i) * SCALE  -  bias_fp(i)      (s64)
 *   2.  gyro_corr  = diff * scale_fp(i) / SCALE               (s64)
 *   3.  gyro_acc   = alpha * gyro_corr + (1-alpha) * gyro_acc (s64, LPF)
 *   4.  mouse      = gyro_acc * mouse_k / SCALE^2             (s16 px)
 *   5.  |gyro_acc| > threshold * SCALE  →  bigmove flag
 *
 * Steps 1-3 produce gyro_acc in fixed-point (scaled by SCALE).
 * Step 4 converts to integer pixels: mouse_k is also SCALE-scaled,
 * so we divide by SCALE twice (SCALE * SCALE) to get integer output.
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
		 *    acc = (alpha * corr + (SCALE - alpha) * acc) / SCALE
		 */
		gyro_acc[i] = ((s64)calib->alpha * gyro_corr
			       + ((s64)LGMAGIC_FP_SCALE - calib->alpha)
				 * gyro_acc[i])
			      >> LGMAGIC_FP_SHIFT;
	}

	/*
	 * 4. Convert fixed-point gyro accumulator to integer mouse pixels.
	 *    mouse_k is SCALE-scaled, gyro_acc is SCALE-scaled, so the
	 *    product is SCALE^2-scaled.  Divide by SCALE^2 for integer pixels.
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

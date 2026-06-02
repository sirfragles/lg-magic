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

#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

#include <linux/types.h>

/*
 * Fixed-point arithmetic: all fractional values are multiplied by
 * LGMAGIC_FP_SCALE (2^16 = 65536) to avoid kernel floating-point.
 *
 * This is critical because the HID raw_event callback can run in
 * softirq context where bare FPU usage corrupts userspace state
 * without kernel_fpu_begin()/kernel_fpu_end() guards.
 */
#define LGMAGIC_FP_SCALE 65536
#define LGMAGIC_FP_SHIFT 16

/*
 * On-disk calibration format (IEEE 754 float).
 * Backward-compatible with existing lg_magic_calib.bin firmware files.
 * Only used during firmware loading; converted to fixed-point at probe time.
 */
struct lg_magic_airmouse_calib {
	float gyro_bias[3];
	float gyro_scale[3];
	float alpha;
	float mouse_k;
};

/*
 * Runtime calibration in fixed-point.
 * All values are multiplied by LGMAGIC_FP_SCALE.
 * gyro_bias uses s64 to safely hold raw-s16-extreme * SCALE values.
 */
struct lg_magic_airmouse_calib_fp {
	s64 gyro_bias[3];   /* bias  * SCALE  */
	s32 gyro_scale[3];  /* scale * SCALE  */
	s32 alpha;          /* alpha * SCALE  (0 .. SCALE) */
	s32 mouse_k;        /* k     * SCALE  (0 .. SCALE) */
};

/* Validate a float-format calibration blob (on-disk). */
int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib);

/* Convert float calibration to fixed-point runtime format. */
int lgmagic_convert_calib_to_fp(struct lg_magic_airmouse_calib *src,
				struct lg_magic_airmouse_calib_fp *dst);

/*
 * Compute mouse deltas from gyro data.
 * @calib:     fixed-point calibration (read-only)
 * @gyro_acc:  LPF accumulators (s64[3], mutated in-place, scaled by SCALE)
 * @threshold: raw threshold (scaled internally by SCALE)
 * @gyro:      raw s16 gyro triple [gx, gy, gz]
 * @mouse:     output s16 [rel_x, rel_y] in pixels
 * Returns:    1 if gyro exceeds threshold (enter airmouse mode), 0 otherwise
 */
int lgmagic_calc_mouse(struct lg_magic_airmouse_calib_fp *calib,
		       s64 *gyro_acc, u16 threshold,
		       s16 *gyro, s16 *mouse);

#endif /* LG_MAGIC_AIRMOUSE_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  LG Magic Remote — airmouse math (fixed-point, no kernel FPU)
 *
 *  All fractional values use 16-bit fixed-point scale (×65536).
 *  The on-disk firmware format remains IEEE 754 float for backward
 *  compatibility; conversion happens once at probe time via a
 *  pure-integer decoder.
 *
 *  Copyright (C) 2025 Ilya Chelyadin <ilya77105@gmail.com>
 */

#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

#include <linux/types.h>

/* ── Fixed-point constants ────────────────────────────────────────── */

/*
 * 16 fractional bits — good balance of precision and range.
 *   SCALE     = 2^16      (multiplier)
 *   SHIFT     = 16        (>> by this to divide by SCALE)
 *   SCALE_SQ  = 2^32      (pre-computed for mouse-pixel conversion)
 */
#define LGMAGIC_FP_SCALE    65536
#define LGMAGIC_FP_SHIFT    16
#define LGMAGIC_FP_SCALE_SQ ((s64)LGMAGIC_FP_SCALE * LGMAGIC_FP_SCALE)

/* ── On-disk firmware format (IEEE 754 float, backward-compat) ────── */

/*
 * 32-byte firmware blob: 8 consecutive little-endian IEEE 754 floats.
 * This struct is never instantiated — it exists only for sizeof() and
 * as documentation of the binary layout.  All runtime math uses the
 * fixed-point struct below.
 */
struct lg_magic_airmouse_calib {
	float gyro_bias[3];
	float gyro_scale[3];
	float alpha;
	float mouse_k;
};

/* ── Runtime calibration (fixed-point, no FPU needed) ─────────────── */

/*
 * All fields are scaled by LGMAGIC_FP_SCALE.
 * gyro_bias uses s64 because raw-s16-extreme × SCALE = 2^31,
 * which overflows s32 by 1.
 */
struct lg_magic_airmouse_calib_fp {
	s64 gyro_bias[3];
	s32 gyro_scale[3];
	s32 alpha;     /* 0 .. SCALE */
	s32 mouse_k;   /* 0 .. SCALE */
};

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * Validate that decoded calibration values fall within reasonable bounds:
 *   |gyro_bias|  ≤ 100 · SCALE
 *   |gyro_scale| ≤ 10  · SCALE
 *   0 ≤ alpha    ≤ SCALE
 *   0 ≤ mouse_k  ≤ SCALE
 *
 * Returns 0 if valid, non-zero if any field is out of range.
 */
int lgmagic_validate_calib_fp(struct lg_magic_airmouse_calib_fp *calib);

/*
 * Convert raw firmware bytes (IEEE 754 f32) to fixed-point runtime format.
 *
 * @fw_data:  pointer to 32 bytes of firmware data
 * @fw_size:  must be ≥ 32
 * @dst:      output fixed-point calibration
 *
 * No C floating-point operations are used — the decoder parses the
 * IEEE 754 bit pattern with pure integer arithmetic.
 *
 * Returns 0 on success, -EINVAL if fw_size is too small.
 */
int lgmagic_convert_calib_to_fp(const u8 *fw_data, size_t fw_size,
				struct lg_magic_airmouse_calib_fp *dst);

/*
 * Process one gyro sample through the airmouse pipeline.
 *
 * Pipeline (all fixed-point, scale = SCALE):
 *   1. Bias correction:  diff = gyro_raw · SCALE − bias        (s64)
 *   2. Scale:            corr = diff · scale / SCALE            (s64)
 *   3. LPF:              acc  = (α·corr + (1−α)·acc) / SCALE   (s64)
 *   4. Pixels:           mx   = acc_z · mouse_k / SCALE²       (s16)
 *                        my   = acc_x · mouse_k / SCALE²       (s16)
 *   5. Threshold:        |acc_x| > threshold·SCALE  or
 *                        |acc_z| > threshold·SCALE  →  return true
 *
 * @calib:              fixed-point calibration (read-only)
 * @filter_state:       LPF accumulators [x, y, z], mutated in-place
 * @threshold:          raw threshold (scaled internally by SCALE)
 * @gyro_raw:           raw s16 gyro values [gx, gy, gz]
 * @mouse_dx_out:       output relative X (pixels)
 * @mouse_dy_out:       output relative Y (pixels)
 *
 * Returns true if gyro magnitude exceeds threshold (enter airmouse mode).
 */
bool lgmagic_calc_mouse(struct lg_magic_airmouse_calib_fp *calib,
			s64 *filter_state, u16 threshold,
			const s16 *gyro_raw,
			s16 *mouse_dx_out, s16 *mouse_dy_out);

#endif /* LG_MAGIC_AIRMOUSE_H */

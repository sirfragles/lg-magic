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
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/compiler.h>

#include "lg_magic_airmouse.h"

#define LGMAGIC_DRV_VERSION "2.0"

static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug message level (0=quiet, 1=normal, 2=verbose)");

#define lgmagic_dev_dbg(dev, fmt, ...)                                     \
	do {                                                               \
		if (debug >= 2)                                            \
			dev_dbg(dev, fmt, ##__VA_ARGS__);                  \
	} while (0)

#define lgmagic_dev_info(dev, fmt, ...)                                    \
	do {                                                               \
		if (debug >= 1)                                            \
			dev_info(dev, fmt, ##__VA_ARGS__);                 \
	} while (0)

#define lgmagic_dev_warn(dev, fmt, ...)                                    \
	do {                                                               \
		if (debug >= 1)                                            \
			dev_warn(dev, fmt, ##__VA_ARGS__);                 \
	} while (0)

#define lgmagic_dev_err(dev, fmt, ...)                                     \
	do {                                                               \
		dev_err(dev, fmt, ##__VA_ARGS__);                          \
	} while (0)

static int airmouse = 1;
module_param(airmouse, int, 0644);
MODULE_PARM_DESC(airmouse, "Report mouse events");

static int airmouse_threshold = 300;
module_param(airmouse_threshold, int, 0644);
MODULE_PARM_DESC(airmouse_threshold, "Airmouse enable threshold");

static int imu_evdev = 0;
module_param(imu_evdev, int, 0644);
MODULE_PARM_DESC(imu_evdev, "Expose raw IMU");

struct lgmagic_drvdata {
	struct input_dev *input_hid;
	struct input_dev *input_imu;

	u16 last_keycode;
	u16 last_btncode;
	s64 gyro_acc[3]; /* LPF accumulators (fixed-point, scaled by 65536) */
	int mode;
	struct lg_magic_airmouse_calib_fp calib;
};

#define LGMAGIC_CODE_WHEEL 0x8044

static const struct {
	u16 code;
	u16 keycode;
} lg_btn_map[] = {
	/* ── Power ─────────────────────────────────────── */
	{ 0x8000, KEY_POWER },          /* [POWER] button (top-right) */
	{ 0x8099, KEY_SLEEP },          /* [SLEEP] secondary power */

	/* ── Number pad ────────────────────────────────── */
	{ 0x8010, KEY_0 }, { 0x8011, KEY_1 }, { 0x8012, KEY_2 },
	{ 0x8013, KEY_3 }, { 0x8014, KEY_4 }, { 0x8015, KEY_5 },
	{ 0x8016, KEY_6 }, { 0x8017, KEY_7 }, { 0x8018, KEY_8 }, { 0x8019, KEY_9 },

	/* ── Center wheel/OK ───────────────────────────── */
	{ LGMAGIC_CODE_WHEEL, KEY_ENTER }, /* [OK] center press */
	{ LGMAGIC_CODE_WHEEL, BTN_LEFT },  /* [OK] → mouse left click (airmouse mode) */

	/* ── Navigation ────────────────────────────────── */
	{ 0x8040, KEY_UP },
	{ 0x8041, KEY_DOWN },
	{ 0x8006, KEY_RIGHT },
	{ 0x8007, KEY_LEFT },

	/* ── Volume / Audio ────────────────────────────── */
	{ 0x8002, KEY_VOLUMEUP },        /* [VOL+] */
	{ 0x8003, KEY_VOLUMEDOWN },      /* [VOL-] */
	{ 0x8009, KEY_MUTE },            /* [MUTE] */
	{ 0x808B, KEY_VOICECOMMAND },    /* [MIC] voice / Google Assistant */

	/* ── Home / Navigation keys ────────────────────── */
	{ 0x807C, KEY_HOME },            /* [HOME] house icon */
	{ 0x8028, KEY_BACK },            /* [BACK] arrow */
	{ 0x8043, KEY_SETUP },           /* [SETTINGS] gear icon */
	{ 0x80AB, KEY_PROGRAM },         /* [GUIDE] program guide */

	/* ── Media / TV controls ───────────────────────── */
	{ 0x8053, KEY_LIST },            /* [LIST] channel list */
	{ 0x8045, KEY_MENU },            /* [...] more options */
	{ 0x805D, KEY_MEDIA },           /* [IVI / Prime Video] streaming */
	{ 0x800B, KEY_TV },              /* [TV / INPUT] source */
	{ 0x8098, KEY_CONTEXT_MENU },    /* [STB MENU] set-top box menu */
	{ 0x8081, KEY_VIDEO },           /* [MOVIES] */

	/* ── Channel ───────────────────────────────────── */
	/* Note: 0x8000 KEY_CHANNELUP collides with POWER, disabled */
	{ 0x8001, KEY_CHANNELDOWN },     /* [CH-] */

	/* ── Playback ──────────────────────────────────── */
	{ 0x80B0, KEY_PLAY },            /* [▶ PLAY] */
	{ 0x80BA, KEY_PAUSE },           /* [⏸ PAUSE] */

	/* ── Color buttons (teletext / smart TV) ───────── */
	{ 0x8072, KEY_RED },
	{ 0x8071, KEY_GREEN },
	{ 0x8063, KEY_YELLOW },
	{ 0x8061, KEY_BLUE },
};

static int lgmagic_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 reporting = 0;
	u16 counter;
	s16 imu[6];
	s16 mouse[2] = { 0 };
	u16 btn_code;
	s8 wheel;
	int i;

	if (unlikely(!drvdata || !drvdata->input_hid || !drvdata->input_imu)) {
		lgmagic_dev_warn(&hdev->dev,
				 "No drvdata or no input dev");
		return 0;
	}

	if (unlikely(data[0] != 0xFD)) {
		/* Known non-IMU report types — log at debug only */
		if (data[0] == 0xF9 || data[0] == 0x01) {
			lgmagic_dev_dbg(&hdev->dev,
					"Known report type 0x%02x (size %d) — ignored",
					data[0], size);
		} else {
			lgmagic_dev_dbg(&hdev->dev,
					"Unknown report type 0x%02x (size %d)",
					data[0], size);
		}
		return 0;
	}

	if (unlikely(size < 20)) {
		lgmagic_dev_dbg(&hdev->dev,
				"Short 0xFD report: %d bytes (expected >= 20)",
				size);
		return 0;
	}

	/* Button is last two bytes before wheel */
	btn_code = (data[17] << 8) | data[18];
	wheel = (s8)data[19];

	/* Parse counter (little-endian) */
	counter = data[1] | (data[2] << 8);

	/*
	 * Parse IMU data only if needed — skip the 6-element loop when
	 * both airmouse and IMU evdev are disabled (most common case).
	 */
	if (airmouse || imu_evdev) {
		for (i = 0; i < 6; i++)
			imu[i] = (data[5 + 2 * i] << 8) | data[6 + 2 * i];
	}

	/* ── Button handling ────────────────────────────────── */
	if (unlikely(btn_code != drvdata->last_btncode)) {
		input_report_key(drvdata->input_hid, drvdata->last_keycode,
				 0);
		reporting = 1;
		drvdata->last_keycode = 0;
		drvdata->last_btncode = btn_code;
		if (btn_code != 0) {
			for (i = 0; i < ARRAY_SIZE(lg_btn_map); i++) {
				if (lg_btn_map[i].code == btn_code) {
					u16 report_keycode =
						lg_btn_map[i].keycode;

					if (lg_btn_map[i].code ==
						    LGMAGIC_CODE_WHEEL &&
					    drvdata->mode)
						report_keycode = BTN_LEFT;
					else
						drvdata->mode = 0;

					input_report_key(drvdata->input_hid,
							 report_keycode, 1);
					drvdata->last_keycode =
						report_keycode;
					break;
				}
			}
		}
	}

	/* ── Wheel handling ─────────────────────────────────── */
	if (wheel != 0) {
		if (drvdata->mode) {
			input_report_rel(drvdata->input_hid, REL_WHEEL,
					 wheel);
		} else {
			input_report_key(drvdata->input_hid,
					 wheel > 0 ? KEY_UP : KEY_DOWN, 1);
			input_report_key(drvdata->input_hid,
					 wheel > 0 ? KEY_UP : KEY_DOWN, 0);
		}
		reporting = 1;
	}

	/* ── Airmouse (fixed-point, no kernel FPU) ────────────── */
	if (airmouse) {
		int bigmove;

		bigmove = lgmagic_calc_mouse(&drvdata->calib,
					     drvdata->gyro_acc,
					     airmouse_threshold, imu,
					     mouse);
		if (bigmove)
			drvdata->mode = 1;

		if (drvdata->mode == 1) {
			input_report_rel(drvdata->input_hid, REL_X,
					 mouse[0]);
			input_report_rel(drvdata->input_hid, REL_Y,
					 mouse[1]);
		}
	}

	if (mouse[0] || mouse[1])
		reporting = 1;

	if (reporting)
		input_sync(drvdata->input_hid);

	if (!imu_evdev)
		return 0;

	/* Report counter */
	input_event(drvdata->input_imu, EV_MSC, MSC_SERIAL, counter);

	/* Report IMU axes */
	input_report_abs(drvdata->input_imu, ABS_X, imu[3]);
	input_report_abs(drvdata->input_imu, ABS_Y, imu[4]);
	input_report_abs(drvdata->input_imu, ABS_Z, imu[5]);
	input_report_abs(drvdata->input_imu, ABS_RX, imu[0]);
	input_report_abs(drvdata->input_imu, ABS_RY, imu[1]);
	input_report_abs(drvdata->input_imu, ABS_RZ, imu[2]);

	input_sync(drvdata->input_imu);

	return 0;
}

static void lgmagic_sanitize_mac(const char *uniq, char *out)
{
	size_t i;

	for (i = 0; uniq[i] && i < 17; i++) {
		if (uniq[i] == ':')
			out[i] = '_';
		else
			out[i] = uniq[i];
	}
}

/*
 * Load calibration firmware, validate it, and convert to fixed-point.
 * Returns 0 on success (calibration loaded and applied),
 * non-zero if no valid calibration found (driver operates without airmouse).
 */
static int lgmagic_load_fw(const char *fwname, struct device *dev,
			   struct lgmagic_drvdata *drvdata)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, fwname, dev);
	if (ret != 0)
		return ret;

	if (fw->size < sizeof(struct lg_magic_airmouse_calib)) {
		lgmagic_dev_warn(dev,
				 "Calibration file %s too small (%zu < %zu)",
				 fwname, fw->size,
				 sizeof(struct lg_magic_airmouse_calib));
		release_firmware(fw);
		return -EINVAL;
	}

	/* Convert raw firmware bytes → fixed-point, then validate */
	ret = lgmagic_convert_calib_to_fp(fw->data, fw->size, &drvdata->calib);
	if (ret != 0) {
		lgmagic_dev_warn(dev, "Failed to convert calibration %s", fwname);
		release_firmware(fw);
		return ret;
	}

	if (lgmagic_validate_calib_fp(&drvdata->calib)) {
		lgmagic_dev_warn(dev,
				 "Calibration %s failed validation — airmouse disabled",
				 fwname);
		memset(&drvdata->calib, 0, sizeof(drvdata->calib));
		release_firmware(fw);
		return -EINVAL;
	}

	release_firmware(fw);

	lgmagic_dev_info(dev, "Loaded calibration from %s", fwname);
	return 0;
}

static int lgmagic_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	int ret, i;
	struct lgmagic_drvdata *drvdata;
	bool calib_loaded = false;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	/*
	 * Load calibration: try MAC-address-specific firmware first,
	 * fall back to generic.  Log if neither succeeds.
	 */
	if (strlen(hdev->uniq) == 17) {
		char addr_fw_name[] =
			"lg_magic_calib_XX_XX_XX_XX_XX_XX.bin";

		lgmagic_sanitize_mac(hdev->uniq,
				     addr_fw_name +
					     sizeof("lg_magic_calib_") - 1);
		if (lgmagic_load_fw(addr_fw_name, &hdev->dev, drvdata) == 0)
			calib_loaded = true;
	}

	if (!calib_loaded) {
		ret = lgmagic_load_fw("lg_magic_calib.bin", &hdev->dev,
				      drvdata);
		if (ret != 0)
			lgmagic_dev_info(
				&hdev->dev,
				"No valid calibration found — airmouse disabled. "
				"Place lg_magic_calib.bin in /lib/firmware/");
	}

	/* ── HID input device (buttons + mouse) ──────────────── */
	drvdata->input_hid = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_hid)
		return -ENOMEM;

	drvdata->input_hid->name = "LG Magic Remote";
	drvdata->input_hid->id.bustype = hdev->bus;
	drvdata->input_hid->id.vendor = hdev->vendor;
	drvdata->input_hid->id.product = hdev->product;

	set_bit(EV_KEY, drvdata->input_hid->evbit);
	set_bit(EV_REL, drvdata->input_hid->evbit);
	set_bit(REL_WHEEL, drvdata->input_hid->relbit);
	set_bit(REL_X, drvdata->input_hid->relbit);
	set_bit(REL_Y, drvdata->input_hid->relbit);

	for (i = 0; i < ARRAY_SIZE(lg_btn_map); i++)
		set_bit(lg_btn_map[i].keycode, drvdata->input_hid->keybit);

	ret = input_register_device(drvdata->input_hid);
	if (ret)
		return ret;

	/* ── IMU input device (raw sensor data) ──────────────── */
	drvdata->input_imu = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_imu)
		return -ENOMEM;

	drvdata->input_imu->name = "LG Magic Remote IMU";
	drvdata->input_imu->id.bustype = hdev->bus;
	drvdata->input_imu->id.vendor = hdev->vendor;
	drvdata->input_imu->id.product = hdev->product;

	set_bit(EV_ABS, drvdata->input_imu->evbit);
	set_bit(EV_MSC, drvdata->input_imu->evbit);
	set_bit(MSC_SERIAL, drvdata->input_imu->mscbit);

	for (i = ABS_X; i <= ABS_RZ; i++) {
		set_bit(i, drvdata->input_imu->absbit);
		input_set_abs_params(drvdata->input_imu, i, -32768, 32767, 4,
				     4);
	}

	if (imu_evdev) {
		ret = input_register_device(drvdata->input_imu);
		if (ret)
			return ret;
	}

	return 0;
}

static void lgmagic_remove(struct hid_device *hdev)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);

	/* Release any held button before stopping HID */
	if (drvdata && drvdata->input_hid) {
		if (drvdata->last_keycode) {
			input_report_key(drvdata->input_hid,
					 drvdata->last_keycode, 0);
			input_sync(drvdata->input_hid);
		}
		drvdata->last_keycode = 0;
		drvdata->mode = 0;
	}

	hid_hw_stop(hdev);
}

/*
 * Suspend: release held keys and reset state before system sleep.
 * The Bluetooth connection will drop; on resume the remote reconnects.
 */
static int lgmagic_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata && drvdata->input_hid) {
		if (drvdata->last_keycode) {
			input_report_key(drvdata->input_hid,
					 drvdata->last_keycode, 0);
			input_sync(drvdata->input_hid);
			drvdata->last_keycode = 0;
		}
		drvdata->mode = 0;
	}

	return 0;
}

/*
 * Resume: reset gyro accumulator (stale data after reconnect) and mode.
 */
static int lgmagic_resume(struct hid_device *hdev)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata) {
		memset(drvdata->gyro_acc, 0, sizeof(drvdata->gyro_acc));
		drvdata->mode = 0;
		drvdata->last_keycode = 0;
	}

	return 0;
}

static const struct hid_device_id lgmagic_devices[] = {
	{ HID_BLUETOOTH_DEVICE(0x000f, 0x3412) }, /* LG Magic Remote */
	{}
};
MODULE_DEVICE_TABLE(hid, lgmagic_devices);

static struct hid_driver lgmagic_driver = {
	.name = "lgmagic",
	.id_table = lgmagic_devices,
	.raw_event = lgmagic_raw_event,
	.probe = lgmagic_probe,
	.remove = lgmagic_remove,
	.suspend = lgmagic_suspend,
	.resume = lgmagic_resume,
};

module_hid_driver(lgmagic_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ilya \"Kokokoshka\" Chelyadin <ilya77105@gmail.com>");
MODULE_DESCRIPTION("LG Magic Remote HID Driver");
MODULE_VERSION(LGMAGIC_DRV_VERSION);

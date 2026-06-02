/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  LG Magic Remote (MR20) — Linux HID driver
 *
 *  Copyright (C) 2025 Ilya Chelyadin <ilya77105@gmail.com>
 *
 *  Bluetooth HID device: vendor 0x000F, product 0x3412
 *
 *  Features:
 *   - 31-button key mapping with airmouse/navigation mode switching
 *   - Gyroscope-based airmouse (fixed-point integer math, no kernel FPU)
 *   - Optional raw IMU evdev device (accelerometer + gyroscope)
 *   - Calibration via Linux firmware subsystem
 *   - Suspend/resume support
 *   - Runtime tuning via sysfs module parameters
 */
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/compiler.h>

#include "lg_magic_airmouse.h"

#define LGMAGIC_DRV_VERSION    "2.0"
#define LGMAGIC_CODE_WHEEL     0x8044
#define LGMAGIC_REPORT_ID      0xFD
#define LGMAGIC_REPORT_MIN_LEN 20
#define LGMAGIC_MAC_STRLEN     17

/* ── Module parameters ────────────────────────────────────────────── */

static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level: 0=quiet, 1=normal, 2=verbose");

static int airmouse = 1;
module_param(airmouse, int, 0644);
MODULE_PARM_DESC(airmouse, "Enable airmouse pointer (0/1)");

static int airmouse_threshold = 300;
module_param(airmouse_threshold, int, 0644);
MODULE_PARM_DESC(airmouse_threshold,
		 "Gyro magnitude required to enter airmouse mode (default 300)");

static int imu_evdev;
module_param(imu_evdev, int, 0644);
MODULE_PARM_DESC(imu_evdev,
		 "Expose raw IMU as separate evdev device (0/1)");

/* ── Conditional logging macros ───────────────────────────────────── */

#define lgmagic_dev_dbg(dev, fmt, ...)                              \
	do { if (debug >= 2) dev_dbg(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_info(dev, fmt, ...)                             \
	do { if (debug >= 1) dev_info(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_warn(dev, fmt, ...)                             \
	do { if (debug >= 1) dev_warn(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_err(dev, fmt, ...)                              \
	do { dev_err(dev, fmt, ##__VA_ARGS__); } while (0)

/* ── Per-device driver state ──────────────────────────────────────── */

struct lgmagic_drvdata {
	struct input_dev *input_hid;       /* buttons + wheel + airmouse */
	struct input_dev *input_imu;       /* raw accel/gyro (optional) */

	u16 held_keycode;                  /* currently pressed key, 0=none */
	u16 last_button_code;              /* HID button field from prev frame */
	s64 gyro_filter_state[3];          /* LPF accumulators (fp-scaled) */
	bool is_airmouse_mode;             /* true = pointer, false = nav */
	struct lg_magic_airmouse_calib_fp calib;
};

/* ── Button lookup table ──────────────────────────────────────────── */

/*
 * Maps 16-bit HID button codes to Linux input keycodes.
 * The wheel-code (0x8044) appears twice: KEY_ENTER in nav mode,
 * BTN_LEFT in airmouse mode.  Selection is at runtime.
 */
static const struct {
	u16 hid_code;
	u16 linux_keycode;
} button_map[] = {
	/* Power */
	{ 0x8000, KEY_POWER },
	{ 0x8099, KEY_SLEEP },

	/* Number pad */
	{ 0x8010, KEY_0 },  { 0x8011, KEY_1 },  { 0x8012, KEY_2 },
	{ 0x8013, KEY_3 },  { 0x8014, KEY_4 },  { 0x8015, KEY_5 },
	{ 0x8016, KEY_6 },  { 0x8017, KEY_7 },  { 0x8018, KEY_8 },
	{ 0x8019, KEY_9 },

	/* Center OK / wheel press  (mode-dependent: ENTER or BTN_LEFT) */
	{ LGMAGIC_CODE_WHEEL, KEY_ENTER },
	{ LGMAGIC_CODE_WHEEL, BTN_LEFT  },

	/* D-pad */
	{ 0x8040, KEY_UP    },
	{ 0x8041, KEY_DOWN  },
	{ 0x8006, KEY_RIGHT },
	{ 0x8007, KEY_LEFT  },

	/* Volume / audio */
	{ 0x8002, KEY_VOLUMEUP     },
	{ 0x8003, KEY_VOLUMEDOWN   },
	{ 0x8009, KEY_MUTE         },
	{ 0x808B, KEY_VOICECOMMAND },

	/* Home / navigation */
	{ 0x807C, KEY_HOME          },
	{ 0x8028, KEY_BACK          },
	{ 0x8043, KEY_SETUP         },
	{ 0x80AB, KEY_PROGRAM       },

	/* Media / TV */
	{ 0x8053, KEY_LIST          },
	{ 0x8045, KEY_MENU          },
	{ 0x805D, KEY_MEDIA         },
	{ 0x800B, KEY_TV            },
	{ 0x8098, KEY_CONTEXT_MENU  },
	{ 0x8081, KEY_VIDEO         },

	/* Channel  (CH+ disabled — code 0x8000 collides with POWER) */
	{ 0x8001, KEY_CHANNELDOWN   },

	/* Playback */
	{ 0x80B0, KEY_PLAY  },
	{ 0x80BA, KEY_PAUSE },

	/* Color buttons */
	{ 0x8072, KEY_RED    },
	{ 0x8071, KEY_GREEN  },
	{ 0x8063, KEY_YELLOW },
	{ 0x8061, KEY_BLUE   },
};

/*
 * Look up a HID button code in the map.
 * Returns the Linux keycode, or 0 if not found.
 */
static u16 button_lookup(u16 hid_code, bool airmouse_mode)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(button_map); i++) {
		if (button_map[i].hid_code != hid_code)
			continue;

		/* Wheel code: choose ENTER or BTN_LEFT based on mode */
		if (hid_code == LGMAGIC_CODE_WHEEL)
			return airmouse_mode ? BTN_LEFT : KEY_ENTER;

		return button_map[i].linux_keycode;
	}
	return 0; /* unknown code */
}

/* ── HID report parsing ───────────────────────────────────────────── */

/*
 * Report 0xFD payload layout (20 bytes after report ID):
 *
 *   Offset  Size  Content            Endian
 *   ──────────────────────────────────────
 *    0-1     2    Packet counter       LE u16
 *    2-3     2    Constant (0xFD00)    LE u16
 *    4-5     2    Gyro X               BE s16
 *    6-7     2    Gyro Y               BE s16
 *    8-9     2    Gyro Z               BE s16
 *   10-11    2    Accel X              BE s16
 *   12-13    2    Accel Y              BE s16
 *   14-15    2    Accel Z              BE s16
 *   16-17    2    Button code          BE u16
 *   18       1    Wheel delta          s8
 */
static void parse_imu_data(const u8 *data, s16 *gyro, s16 *accel)
{
	/*
	 * Unrolled loop: 3 gyro values (offsets 5-10) then
	 * 3 accel values (offsets 11-16), all big-endian s16.
	 */
	gyro[0]  = (data[5]  << 8) | data[6];
	gyro[1]  = (data[7]  << 8) | data[8];
	gyro[2]  = (data[9]  << 8) | data[10];
	accel[0] = (data[11] << 8) | data[12];
	accel[1] = (data[13] << 8) | data[14];
	accel[2] = (data[15] << 8) | data[16];
}

static void report_imu_evdev(struct input_dev *idev, u16 counter,
			     const s16 *gyro, const s16 *accel)
{
	input_event(idev, EV_MSC, MSC_SERIAL, counter);
	input_report_abs(idev, ABS_X,  accel[0]);
	input_report_abs(idev, ABS_Y,  accel[1]);
	input_report_abs(idev, ABS_Z,  accel[2]);
	input_report_abs(idev, ABS_RX, gyro[0]);
	input_report_abs(idev, ABS_RY, gyro[1]);
	input_report_abs(idev, ABS_RZ, gyro[2]);
	input_sync(idev);
}

/* ── raw_event — main HID callback (softirq context) ──────────────── */

static int lgmagic_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct lgmagic_drvdata *priv = hid_get_drvdata(hdev);
	bool events_emitted = false;
	s16 gyro[3], accel[3];
	s16 mouse_dx, mouse_dy;
	u16 btn_code, counter;
	s8 wheel_delta;
	u16 new_keycode;

	/* ── Guards ────────────────────────────────────────── */
	if (unlikely(!priv || !priv->input_hid || !priv->input_imu))
		return 0;

	if (unlikely(data[0] != LGMAGIC_REPORT_ID)) {
		if (data[0] == 0xF9 || data[0] == 0x01)
			lgmagic_dev_dbg(&hdev->dev,
				"Report 0x%02x (size %d) — ignored",
				data[0], size);
		else
			lgmagic_dev_dbg(&hdev->dev,
				"Unknown report 0x%02x (size %d)",
				data[0], size);
		return 0;
	}

	if (unlikely(size < LGMAGIC_REPORT_MIN_LEN))
		return 0;

	/* ── Parse fixed fields ─────────────────────────────── */
	counter    = data[1] | (data[2] << 8);          /* LE u16 */
	btn_code   = (data[17] << 8) | data[18];        /* BE u16 */
	wheel_delta = (s8)data[19];

	/* ── Parse IMU (skip if neither feature needs it) ──── */
	if (airmouse || imu_evdev)
		parse_imu_data(data, gyro, accel);

	/* ── Button state machine ──────────────────────────── */
	if (unlikely(btn_code != priv->last_button_code)) {
		/* Release previous key */
		input_report_key(priv->input_hid, priv->held_keycode, 0);
		events_emitted = true;
		priv->held_keycode = 0;
		priv->last_button_code = btn_code;

		if (btn_code != 0) {
			new_keycode = button_lookup(btn_code,
						    priv->is_airmouse_mode);

			/* Any non-wheel button press exits airmouse mode */
			if (btn_code != LGMAGIC_CODE_WHEEL)
				priv->is_airmouse_mode = false;

			if (new_keycode) {
				input_report_key(priv->input_hid,
						 new_keycode, 1);
				priv->held_keycode = new_keycode;
			}
		}
	}

	/* ── Wheel ─────────────────────────────────────────── */
	if (wheel_delta != 0) {
		if (priv->is_airmouse_mode)
			input_report_rel(priv->input_hid, REL_WHEEL,
					 wheel_delta);
		else
			input_report_key(priv->input_hid,
					 wheel_delta > 0 ? KEY_UP
							 : KEY_DOWN,
					 1);
		if (!priv->is_airmouse_mode)
			input_report_key(priv->input_hid,
					 wheel_delta > 0 ? KEY_UP
							 : KEY_DOWN,
					 0);
		events_emitted = true;
	}

	/* ── Airmouse (fixed-point integer, no kernel FPU) ─── */
	mouse_dx = 0;
	mouse_dy = 0;

	if (airmouse) {
		bool above_threshold;

		above_threshold = lgmagic_calc_mouse(
			&priv->calib, priv->gyro_filter_state,
			airmouse_threshold, gyro, &mouse_dx, &mouse_dy);

		if (above_threshold)
			priv->is_airmouse_mode = true;

		if (priv->is_airmouse_mode) {
			input_report_rel(priv->input_hid, REL_X, mouse_dx);
			input_report_rel(priv->input_hid, REL_Y, mouse_dy);
		}
	}

	if (mouse_dx || mouse_dy)
		events_emitted = true;

	if (events_emitted)
		input_sync(priv->input_hid);

	/* ── Raw IMU evdev (optional) ──────────────────────── */
	if (imu_evdev)
		report_imu_evdev(priv->input_imu, counter, gyro, accel);

	return 0;
}

/* ── MAC address sanitizer ────────────────────────────────────────── */

/*
 * Replace colons with underscores in the Bluetooth MAC string.
 * Used to construct MAC-specific firmware filenames.
 */
static void mac_to_filename(const char *mac, char *out)
{
	int i;

	for (i = 0; mac[i] && i < LGMAGIC_MAC_STRLEN; i++)
		out[i] = (mac[i] == ':') ? '_' : mac[i];
}

/* ── Firmware loading ─────────────────────────────────────────────── */

/*
 * Load calibration from /lib/firmware/, validate, convert to fixed-point.
 * Returns 0 if calibration is loaded and valid.
 * On failure, drvdata->calib is zeroed (airmouse operates as no-op).
 */
static int load_calibration(const char *fw_name, struct device *dev,
			    struct lgmagic_drvdata *priv)
{
	const struct firmware *fw;
	int err;

	err = request_firmware(&fw, fw_name, dev);
	if (err)
		return err;

	if (fw->size < sizeof(struct lg_magic_airmouse_calib)) {
		lgmagic_dev_warn(dev,
			"Firmware %s too small (%zu bytes, need >= %zu)",
			fw_name, fw->size,
			sizeof(struct lg_magic_airmouse_calib));
		release_firmware(fw);
		return -EINVAL;
	}

	err = lgmagic_convert_calib_to_fp(fw->data, fw->size, &priv->calib);
	if (err) {
		lgmagic_dev_warn(dev, "Cannot parse firmware %s", fw_name);
		release_firmware(fw);
		return err;
	}

	/* Reject out-of-range calibration values */
	if (lgmagic_validate_calib_fp(&priv->calib)) {
		lgmagic_dev_warn(dev,
			"Firmware %s failed validation — airmouse disabled",
			fw_name);
		memset(&priv->calib, 0, sizeof(priv->calib));
		release_firmware(fw);
		return -EINVAL;
	}

	release_firmware(fw);
	lgmagic_dev_info(dev, "Calibration loaded: %s", fw_name);
	return 0;
}

/*
 * Try MAC-specific firmware first, fall back to generic.
 * Logs a helpful message if neither is found.
 */
static void load_calibration_for_device(struct hid_device *hdev,
					struct lgmagic_drvdata *priv)
{
	bool loaded = false;

	if (strlen(hdev->uniq) == LGMAGIC_MAC_STRLEN) {
		char mac_fw[] = "lg_magic_calib_XX_XX_XX_XX_XX_XX.bin";

		mac_to_filename(hdev->uniq,
				mac_fw + sizeof("lg_magic_calib_") - 1);
		loaded = (load_calibration(mac_fw, &hdev->dev, priv) == 0);
	}

	if (!loaded) {
		if (load_calibration("lg_magic_calib.bin", &hdev->dev, priv))
			lgmagic_dev_info(&hdev->dev,
				"No calibration found — airmouse disabled. "
				"Place lg_magic_calib.bin in /lib/firmware/");
	}
}

/* ── Input device registration helpers ────────────────────────────── */

static int register_hid_input(struct hid_device *hdev,
			      struct lgmagic_drvdata *priv)
{
	int i;

	priv->input_hid = devm_input_allocate_device(&hdev->dev);
	if (!priv->input_hid)
		return -ENOMEM;

	priv->input_hid->name = "LG Magic Remote";
	priv->input_hid->id.bustype = hdev->bus;
	priv->input_hid->id.vendor  = hdev->vendor;
	priv->input_hid->id.product = hdev->product;

	set_bit(EV_KEY, priv->input_hid->evbit);
	set_bit(EV_REL, priv->input_hid->evbit);
	set_bit(REL_WHEEL, priv->input_hid->relbit);
	set_bit(REL_X,     priv->input_hid->relbit);
	set_bit(REL_Y,     priv->input_hid->relbit);

	for (i = 0; i < ARRAY_SIZE(button_map); i++)
		set_bit(button_map[i].linux_keycode,
			priv->input_hid->keybit);

	return input_register_device(priv->input_hid);
}

static int register_imu_input(struct hid_device *hdev,
			      struct lgmagic_drvdata *priv)
{
	int axis;

	priv->input_imu = devm_input_allocate_device(&hdev->dev);
	if (!priv->input_imu)
		return -ENOMEM;

	priv->input_imu->name = "LG Magic Remote IMU";
	priv->input_imu->id.bustype = hdev->bus;
	priv->input_imu->id.vendor  = hdev->vendor;
	priv->input_imu->id.product = hdev->product;

	set_bit(EV_ABS, priv->input_imu->evbit);
	set_bit(EV_MSC, priv->input_imu->evbit);
	set_bit(MSC_SERIAL, priv->input_imu->mscbit);

	for (axis = ABS_X; axis <= ABS_RZ; axis++) {
		set_bit(axis, priv->input_imu->absbit);
		input_set_abs_params(priv->input_imu, axis,
				     -32768, 32767, 4, 4);
	}

	if (!imu_evdev)
		return 0;

	return input_register_device(priv->input_imu);
}

/* ── HID driver callbacks ─────────────────────────────────────────── */

static int lgmagic_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct lgmagic_drvdata *priv;
	int err;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	hid_set_drvdata(hdev, priv);

	err = hid_parse(hdev);
	if (err)
		return err;

	err = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (err)
		return err;

	load_calibration_for_device(hdev, priv);

	err = register_hid_input(hdev, priv);
	if (err)
		return err;

	err = register_imu_input(hdev, priv);
	if (err)
		return err;

	return 0;
}

static void release_held_key(struct lgmagic_drvdata *priv)
{
	if (!priv->input_hid || !priv->held_keycode)
		return;

	input_report_key(priv->input_hid, priv->held_keycode, 0);
	input_sync(priv->input_hid);
	priv->held_keycode = 0;
}

static void lgmagic_remove(struct hid_device *hdev)
{
	struct lgmagic_drvdata *priv = hid_get_drvdata(hdev);

	if (priv) {
		release_held_key(priv);
		priv->is_airmouse_mode = false;
	}

	hid_hw_stop(hdev);
}

static int lgmagic_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct lgmagic_drvdata *priv = hid_get_drvdata(hdev);

	if (priv) {
		release_held_key(priv);
		priv->is_airmouse_mode = false;
	}

	return 0;
}

static int lgmagic_resume(struct hid_device *hdev)
{
	struct lgmagic_drvdata *priv = hid_get_drvdata(hdev);

	if (priv) {
		memset(priv->gyro_filter_state, 0,
		       sizeof(priv->gyro_filter_state));
		priv->is_airmouse_mode = false;
		priv->held_keycode = 0;
	}

	return 0;
}

/* ── Driver registration ──────────────────────────────────────────── */

static const struct hid_device_id lgmagic_devices[] = {
	{ HID_BLUETOOTH_DEVICE(0x000f, 0x3412) },
	{}
};
MODULE_DEVICE_TABLE(hid, lgmagic_devices);

static struct hid_driver lgmagic_driver = {
	.name      = "lgmagic",
	.id_table  = lgmagic_devices,
	.raw_event = lgmagic_raw_event,
	.probe     = lgmagic_probe,
	.remove    = lgmagic_remove,
	.suspend   = lgmagic_suspend,
	.resume    = lgmagic_resume,
};

module_hid_driver(lgmagic_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ilya \"Kokokoshka\" Chelyadin <ilya77105@gmail.com>");
MODULE_DESCRIPTION("LG Magic Remote HID Driver");
MODULE_VERSION(LGMAGIC_DRV_VERSION);

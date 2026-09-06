/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * config.c - lg-magic user configuration.
 *
 * Precedence: built-in defaults < /etc/lg-magic/config.json <
 * ~/.config/lg-magic/config.json < --config FILE < CLI flags.
 * Files are JSON objects, parsed with our own json.c; unknown keys are
 * ignored, malformed files produce a warning and are skipped.
 */
#include "config.h"

#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CFG_SYSTEM_PATH "/etc/lg-magic/config.json"
#define CFG_USER_DIR "/.config/lg-magic"

struct config *g_cfg;
const char *g_tool_version = "1.0";

enum {
	X_IMU_DEVICE = 1 << 0,
	X_HIDRAW_DEVICE = 1 << 1,
	X_DEFAULT_CALIB = 1 << 2,
	X_LPF_ALPHA = 1 << 3,
	X_MOUSE_SCALE = 1 << 4,
	X_MADGWICK_BETA = 1 << 5,
	X_ALPHA = 1 << 6,
	X_MOUSE_K = 1 << 7,
	X_GYRO_SCALE = 1 << 8,
};

static unsigned explicit_mask;

/* ------------------------------------------------------------------ */
/* Key table                                                           */
/* ------------------------------------------------------------------ */

static int key_bit(const char *key)
{
	if (strcmp(key, "imu_device") == 0)
		return X_IMU_DEVICE;
	if (strcmp(key, "hidraw_device") == 0)
		return X_HIDRAW_DEVICE;
	if (strcmp(key, "default_calib") == 0)
		return X_DEFAULT_CALIB;
	if (strcmp(key, "lpf_alpha") == 0)
		return X_LPF_ALPHA;
	if (strcmp(key, "mouse_scale") == 0)
		return X_MOUSE_SCALE;
	if (strcmp(key, "madgwick_beta") == 0)
		return X_MADGWICK_BETA;
	if (strcmp(key, "alpha") == 0)
		return X_ALPHA;
	if (strcmp(key, "mouse_k") == 0)
		return X_MOUSE_K;
	if (strcmp(key, "gyro_scale_default") == 0)
		return X_GYRO_SCALE;
	return 0;
}

static void set_from_value(struct config *cfg, const char *key,
			   struct json_value *v)
{
	char *s;
	int bit = key_bit(key);

	if (!bit || !v)
		return;
	switch (bit) {
	case X_IMU_DEVICE:
	case X_HIDRAW_DEVICE:
	case X_DEFAULT_CALIB:
		if (v->type != JSON_STR)
			return;
		/* An empty string means "auto" - normalize to NULL. */
		if (v->str[0] == '\0')
			s = NULL;
		else {
			s = strdup(v->str);
			if (!s)
				return;
		}
		if (bit == X_IMU_DEVICE) {
			free(cfg->imu_device);
			cfg->imu_device = s;
		} else if (bit == X_HIDRAW_DEVICE) {
			free(cfg->hidraw_device);
			cfg->hidraw_device = s;
		} else {
			free(cfg->default_calib);
			cfg->default_calib = s;
		}
		break;
	case X_LPF_ALPHA:
		if (v->type != JSON_NUM)
			return;
		cfg->lpf_alpha = v->num;
		break;
	case X_MOUSE_SCALE:
		if (v->type != JSON_NUM)
			return;
		cfg->mouse_scale = v->num;
		break;
	case X_MADGWICK_BETA:
		if (v->type != JSON_NUM)
			return;
		cfg->madgwick_beta = v->num;
		break;
	case X_ALPHA:
		if (v->type != JSON_NUM)
			return;
		cfg->alpha = v->num;
		break;
	case X_MOUSE_K:
		if (v->type != JSON_NUM)
			return;
		cfg->mouse_k = v->num;
		break;
	case X_GYRO_SCALE:
		if (v->type != JSON_NUM)
			return;
		cfg->gyro_scale_default = v->num;
		break;
	}
	explicit_mask |= (unsigned)bit;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

static void merge_file(struct config *cfg, const char *path)
{
	const char *err = NULL;
	size_t eoff = 0;
	struct json_value *root;
	static const char *keys[] = {
		"imu_device", "hidraw_device", "default_calib",
		"lpf_alpha", "mouse_scale", "madgwick_beta",
		"alpha", "mouse_k", "gyro_scale_default",
	};
	size_t i;
	struct stat st;

	/* Missing config files are the normal case - skip silently; warn
	 * only when a file exists but cannot be parsed. */
	if (stat(path, &st) != 0)
		return;

	root = json_load_file(path, &err, &eoff);
	if (!root) {
		fprintf(stderr, "warning: ignoring config file %s: %s "
			"(byte %zu)\n", path, err, eoff);
		return;
	}
	if (root->type != JSON_OBJ) {
		fprintf(stderr, "warning: ignoring config file %s: "
			"root is not an object\n", path);
		json_free(root);
		return;
	}
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		set_from_value(cfg, keys[i], json_obj_get(root, keys[i]));
	json_free(root);
}

static void set_defaults(struct config *cfg)
{
	cfg->lpf_alpha = 0.2;
	cfg->mouse_scale = 30.0;
	cfg->madgwick_beta = 0.1;
	cfg->alpha = 0.2;
	cfg->mouse_k = 0.5;
	cfg->gyro_scale_default = 0.07;
}

struct config *config_load(const char *extra_path)
{
	struct config *cfg = calloc(1, sizeof(*cfg));
	const char *home;

	if (!cfg)
		return NULL;
	explicit_mask = 0;
	set_defaults(cfg);
	merge_file(cfg, CFG_SYSTEM_PATH);
	home = getenv("HOME");
	if (home) {
		char path[4096];

		snprintf(path, sizeof(path), "%s%s/config.json", home,
			 CFG_USER_DIR);
		merge_file(cfg, path);
	}
	if (extra_path)
		merge_file(cfg, extra_path);
	return cfg;
}

void config_free(struct config *cfg)
{
	if (!cfg)
		return;
	free(cfg->imu_device);
	free(cfg->hidraw_device);
	free(cfg->default_calib);
	free(cfg);
}

/* ------------------------------------------------------------------ */
/* Save / set / query                                                  */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *dir)
{
	char tmp[4096];
	char *p;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", dir);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

int config_save_user(struct config *cfg, char *err, size_t errsz)
{
	const char *home = getenv("HOME");
	char dir[4096], path[sizeof(dir) + sizeof("/config.json")];
	struct json_value *root;
	char *text;
	FILE *f;

	if (!home) {
		snprintf(err, errsz, "HOME is not set; cannot save user config");
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s%s", home, CFG_USER_DIR);
	snprintf(path, sizeof(path), "%s/config.json", dir);
	if (mkdir_p(dir) < 0) {
		snprintf(err, errsz, "cannot create %s: %s", dir,
			 strerror(errno));
		return -1;
	}

	root = json_new(JSON_OBJ);
	if (!root)
		goto oom;
	{
		/* fixed key order, string keys first then numbers */
		struct {
			const char *key;
			int is_str;
			const void *val;
		} kvs[] = {
			{ "imu_device", 1, cfg->imu_device },
			{ "hidraw_device", 1, cfg->hidraw_device },
			{ "default_calib", 1, cfg->default_calib },
			{ "lpf_alpha", 0, &cfg->lpf_alpha },
			{ "mouse_scale", 0, &cfg->mouse_scale },
			{ "madgwick_beta", 0, &cfg->madgwick_beta },
			{ "alpha", 0, &cfg->alpha },
			{ "mouse_k", 0, &cfg->mouse_k },
			{ "gyro_scale_default", 0, &cfg->gyro_scale_default },
		};
		size_t i;

		for (i = 0; i < sizeof(kvs) / sizeof(kvs[0]); i++) {
			struct json_value *v;

			if (kvs[i].is_str) {
				const char *s = kvs[i].val;

				v = json_new_str(s ? s : "");
				if (!v)
					goto oom_free_root;
			} else {
				v = json_new_num(*(const double *)kvs[i].val);
				if (!v)
					goto oom_free_root;
			}
			if (json_obj_add(root, kvs[i].key, v) < 0) {
				json_free(v);
				goto oom_free_root;
			}
		}
	}
	text = json_dumps(root);
	json_free(root);
	if (!text)
		goto oom;
	f = fopen(path, "w");
	if (!f) {
		snprintf(err, errsz, "cannot open %s: %s", path,
			 strerror(errno));
		free(text);
		return -1;
	}
	if (fputs(text, f) < 0 || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %s", path);
		free(text);
		return -1;
	}
	free(text);
	return 0;

oom_free_root:
	json_free(root);
oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

int config_set_key(struct config *cfg, const char *key, const char *value,
		   char *err, size_t errsz)
{
	char *end;
	double d;

	switch (key_bit(key)) {
	case X_IMU_DEVICE:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->imu_device);
			cfg->imu_device = s;
			break;
		}
	case X_HIDRAW_DEVICE:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->hidraw_device);
			cfg->hidraw_device = s;
			break;
		}
	case X_DEFAULT_CALIB:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->default_calib);
			cfg->default_calib = s;
			break;
		}
	case X_LPF_ALPHA:
	case X_MOUSE_SCALE:
	case X_MADGWICK_BETA:
	case X_ALPHA:
	case X_MOUSE_K:
	case X_GYRO_SCALE:
		errno = 0;
		d = strtod(value, &end);
		if (errno != 0 || end == value || *end != '\0') {
			snprintf(err, errsz, "invalid value for %s: '%s'",
				 key, value);
			return -1;
		}
		switch (key_bit(key)) {
		case X_LPF_ALPHA: cfg->lpf_alpha = d; break;
		case X_MOUSE_SCALE: cfg->mouse_scale = d; break;
		case X_MADGWICK_BETA: cfg->madgwick_beta = d; break;
		case X_ALPHA: cfg->alpha = d; break;
		case X_MOUSE_K: cfg->mouse_k = d; break;
		default: cfg->gyro_scale_default = d; break;
		}
		break;
	default:
		snprintf(err, errsz, "unknown config key '%s'", key);
		return -1;
	}
	explicit_mask |= (unsigned)key_bit(key);
	return 0;

oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

int config_is_explicit(const char *key)
{
	int bit = key_bit(key);

	return bit ? (int)(explicit_mask & (unsigned)bit) : 0;
}

/* ------------------------------------------------------------------ */
/* Print                                                               */
/* ------------------------------------------------------------------ */

static void print_num(const char *key, double v)
{
	printf("%-20s = %g%s\n", key, v,
	       config_is_explicit(key) ? " (from config)" : " (default)");
}

static void print_str(const char *key, const char *v)
{
	printf("%-20s = %s%s\n", key, v ? v : "(auto)",
	       config_is_explicit(key) ? " (from config)" : " (default)");
}

void config_print(const struct config *cfg)
{
	print_str("imu_device", cfg->imu_device);
	print_str("hidraw_device", cfg->hidraw_device);
	print_str("default_calib", cfg->default_calib);
	print_num("lpf_alpha", cfg->lpf_alpha);
	print_num("mouse_scale", cfg->mouse_scale);
	print_num("madgwick_beta", cfg->madgwick_beta);
	print_num("alpha", cfg->alpha);
	print_num("mouse_k", cfg->mouse_k);
	print_num("gyro_scale_default", cfg->gyro_scale_default);
}

void config_print_paths(void)
{
	const char *home = getenv("HOME");

	printf("system: %s\n", CFG_SYSTEM_PATH);
	printf("user:   %s%s/config.json\n", home ? home : "$HOME",
	       CFG_USER_DIR);
}

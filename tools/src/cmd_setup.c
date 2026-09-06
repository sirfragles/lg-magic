/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_setup.c - `lg-magic setup`, the interactive configuration and
 * calibration wizard. After it finishes, the remote works end to end.
 *
 * Steps:  1. environment (root, module, devices, uinput)
 *         2. module parameters (/etc/modprobe.d/lg-magic.conf + reload)
 *         3. accelerometer calibration (recording + Levenberg-Marquardt)
 *         4. gyroscope calibration (recording + mean bias)
 *         5. calibration JSON (gyro scale / alpha / mouse_k questions)
 *         6. firmware blob to /lib/firmware (per-MAC + generic fallback)
 *         7. module reload + dmesg verification
 *         8. airmouse test (uinput virtual mouse)
 *         9. user config (SUDO_USER/HOME aware)
 *        10. summary
 *
 * `--non-interactive` accepts every default (scripting). Run as root.
 *
 * Note on ordering: the IMU evdev node only exists while lg_magic runs
 * with imu_evdev=1 (the module default is 0), so the parameters are
 * written and the module is (re)loaded before the device is reopened
 * for the calibration recordings.
 */
#include "calib.h"
#include "config.h"
#include "csv.h"
#include "evdev.h"
#include "hidraw.h"
#include "lm.h"
#include "modprobe_helpers.h"
#include "uinput.h"

#include <errno.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MODULE_NAME "lg_magic"
#define CALIB_DIR "/etc/lg-magic"
#define CALIB_JSON "/etc/lg-magic/calib.json"
#define FW_DIR "/lib/firmware"
#define FW_GENERIC "lg_magic_calib.bin"
#define ACCEL_SECONDS 20.0
#define GYRO_SECONDS 10.0
#define MOUSE_TEST_SECONDS 8.0
#define LM_G 9.80665

/* Quality gates for the accelerometer fit. The residual r is
 * ||M(a - b)|| - g, so the rms is in m/s^2 regardless of the raw
 * counts scale; the spread is scale-free (max axis range / max norm). */
#define GOOD_RMS 0.2		/* m/s^2 (2% of g) */
#define GOOD_SPREAD 0.15
#define GYRO_STD_WARN 100.0	/* counts; above this the remote moved */

static int non_interactive;
static int stdin_closed;	/* set when ask() hits EOF on stdin */

/* ------------------------------------------------------------------ */
/* Interactive helpers                                                 */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void sigsetup(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: a pending read returns EINTR so loops can exit. */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void trimnl(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

/* Prompt with a default; empty answer (or non-interactive mode, or EOF
 * on stdin) keeps the default. */
static void ask(const char *prompt, const char *def, char *buf, size_t bufsz)
{
	printf("%s [%s]: ", prompt, def);
	fflush(stdout);
	if (non_interactive || stdin_closed) {
		printf("%s (default)\n", def);
		snprintf(buf, bufsz, "%s", def);
		return;
	}
	if (!fgets(buf, (int)bufsz, stdin)) {
		stdin_closed = 1;
		snprintf(buf, bufsz, "%s", def);
		return;
	}
	trimnl(buf);
	if (!buf[0])
		snprintf(buf, bufsz, "%s", def);
}

static int ask_yn(const char *prompt, int def)
{
	char buf[64];

	ask(prompt, def ? "y" : "n", buf, sizeof(buf));
	return buf[0] == 'y' || buf[0] == 'Y';
}

static double ask_number(const char *prompt, double def, double lo, double hi)
{
	char buf[64], defbuf[64];
	double v;

	snprintf(defbuf, sizeof(defbuf), "%.4g", def);
	ask(prompt, defbuf, buf, sizeof(buf));
	v = strtod(buf, NULL);
	if (v < lo || v > hi) {
		if (!non_interactive && !stdin_closed)
			printf("Value out of range (%.4g..%.4g) - keeping the "
			       "default %.4g\n", lo, hi, def);
		return def;
	}
	return v;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

/* Record `seconds` of IMU frames into a growing sample array.
 * Returns the number of samples recorded, or -1 on error. */
static long record_samples(struct evdev_imu *dev, double seconds,
			   struct imu_sample **out, char *err, size_t errsz)
{
	struct imu_sample *s;
	size_t cap = 1024, n = 0;
	struct timespec t0, now;
	int have_t0 = 0;
	long i;

	s = malloc(cap * sizeof(*s));
	if (!s) {
		fprintf(stderr, "lg-magic: out of memory\n");
		return -1;
	}
	for (;;) {
		unsigned counter;
		double dt;
		int a[3], g[3], rv;

		rv = evdev_read_frame(dev, &counter, &dt, a, g, err, errsz);
		if (rv < 0) {
			if (g_stop)
				break;
			fprintf(stderr, "lg-magic: %s\n", err);
			free(s);
			return -1;
		}
		if (rv == 0)
			break;	/* EOF */
		if (!have_t0) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			have_t0 = 1;
		}
		if (n == cap) {
			cap *= 2;
			s = realloc(s, cap * sizeof(*s));
			if (!s) {
				fprintf(stderr, "lg-magic: out of memory\n");
				return -1;
			}
		}
		s[n].counter = counter;
		s[n].dt = dt;
		for (i = 0; i < 3; i++) {
			s[n].accel[i] = a[i];
			s[n].gyro[i] = g[i];
		}
		n++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		printf("\rRecording... %.1f s (%zu frames) ",
		       (now.tv_sec - t0.tv_sec) +
		       (now.tv_nsec - t0.tv_nsec) * 1e-9, n);
		fflush(stdout);
		{
			double elapsed = (now.tv_sec - t0.tv_sec) +
				(now.tv_nsec - t0.tv_nsec) * 1e-9;

			if (elapsed >= seconds)
				break;
		}
	}
	printf("\n");
	*out = s;
	return (long)n;
}

/* ------------------------------------------------------------------ */
/* Step 1: environment                                                 */
/* ------------------------------------------------------------------ */

static int step_environment(struct evdev_imu *dev, char *hidraw_path,
			    size_t hidraw_sz)
{
	char err[256];

	printf("\n=== Step 1: environment ===\n");
	if (geteuid() != 0) {
		fprintf(stderr, "lg-magic: setup needs root - the module "
			"parameters, /etc/modprobe.d and /lib/firmware are "
			"system-wide.\nRun it as: sudo lg-magic setup\n");
		return -1;
	}
	if (!module_is_loaded(MODULE_NAME)) {
		printf("Loading the lg_magic module...\n");
		if (module_load(MODULE_NAME, err, sizeof(err)) < 0) {
			fprintf(stderr, "Warning: %s\n", err);
			fprintf(stderr, "Check the DKMS installation and Secure "
				"Boot (unsigned modules must be enrolled). The "
				"wizard continues, but the kernel part will not "
				"work until this is fixed.\n");
		}
	} else {
		printf("Kernel module lg_magic: loaded.\n");
	}

	/* The IMU evdev node only exists with imu_evdev=1 (the default is
	 * 0); step 2 fixes the parameters if it is missing. */
	if (evdev_find_imu(dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		fprintf(stderr, "The lg_magic module must be loaded with "
			"imu_evdev=1 for the IMU to be exposed. After this "
			"wizard has written the parameter file:\n"
			"  unplug the receiver, run 'sudo rmmod lg_magic', "
			"plug it back in.\n");
		return -1;
	}
	printf("IMU device: %s (%s)\n", dev->name, dev->path);
	evdev_close(dev);	/* re-opened after the module reload */

	if (g_cfg->hidraw_device) {
		snprintf(hidraw_path, hidraw_sz, "%s", g_cfg->hidraw_device);
	} else if (hidraw_find_remote(hidraw_path, hidraw_sz, err,
				      sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		fprintf(stderr, "Is the remote paired and switched on?\n");
		return -1;
	}
	printf("Remote: %s\n", hidraw_path);

	if (access("/dev/uinput", W_OK) == 0 ||
	    access("/dev/input/uinput", W_OK) == 0) {
		printf("uinput: available.\n");
	} else {
		printf("Warning: /dev/uinput is not writable. Install the udev "
		       "rule (51-lgimu.rules) and add your user to the "
		       "'input' group, or run the airmouse as root.\n");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 2: module parameters                                           */
/* ------------------------------------------------------------------ */

static void step_module_params(void)
{
	char err[256];

	printf("\n=== Step 2: module parameters ===\n");
	printf("Writing /etc/modprobe.d/lg-magic.conf (imu_evdev=1 "
	       "airmouse=1)...\n");
	if (module_write_conf(MODULE_NAME, "imu_evdev=1 airmouse=1", err,
			      sizeof(err)) < 0) {
		fprintf(stderr, "Warning: %s\n", err);
		return;
	}
	printf("Reloading the module to apply the parameters...\n");
	if (module_reload(MODULE_NAME, err, sizeof(err)) < 0) {
		fprintf(stderr, "Note: %s\n", err);
		fprintf(stderr, "The module is in use by the connected remote. "
			"The new parameters will apply after:\n"
			"  unplug the receiver, run 'sudo rmmod lg_magic', "
			"plug it back in (or reboot).\n");
	} else {
		printf("Module reloaded with the new parameters.\n");
	}
}

/* ------------------------------------------------------------------ */
/* Steps 3-4: accelerometer / gyroscope calibration                    */
/* ------------------------------------------------------------------ */

/* Scale-free orientation spread: max axis range / max sample norm. */
static double accel_spread(const struct imu_sample *s, long n)
{
	double mn[3], mx[3], maxnorm = 0.0, best = 0.0;
	long i;
	int j;

	for (j = 0; j < 3; j++) {
		mn[j] = 1e30;
		mx[j] = -1e30;
	}
	for (i = 0; i < n; i++) {
		double nrm = 0.0;

		for (j = 0; j < 3; j++) {
			double v = s[i].accel[j];

			if (v < mn[j])
				mn[j] = v;
			if (v > mx[j])
				mx[j] = v;
			nrm += v * v;
		}
		nrm = sqrt(nrm);
		if (nrm > maxnorm)
			maxnorm = nrm;
	}
	for (j = 0; j < 3; j++)
		if (mx[j] - mn[j] > best)
			best = mx[j] - mn[j];
	return maxnorm > 1.0 ? best / maxnorm : 0.0;
}

static int step_accel_calib(struct evdev_imu *dev, struct calib *c)
{
	char err[256];
	int attempt;

	printf("\n=== Step 3: accelerometer calibration ===\n");
	printf("Slowly rotate the remote so that each axis points up and down\n"
	       "in turn. The fit needs at least 6 well-spread orientations.\n");
	for (attempt = 1; attempt <= 3; attempt++) {
		struct imu_sample *s = NULL;
		double (*a)[3];
		double cost, rms, spread;
		long n, i;

		ask_yn("Start the 20 s recording", 1);
		n = record_samples(dev, ACCEL_SECONDS, &s, err, sizeof(err));
		if (n < 0)
			return -1;
		if (g_stop) {
			free(s);
			fprintf(stderr, "Interrupted.\n");
			return -1;
		}
		if (n < 6) {
			printf("Only %ld samples recorded - need at least 6 in "
			       "different orientations.\n", n);
			free(s);
			if (stdin_closed || !ask_yn("Record again", 1))
				return -1;
			continue;
		}
		if (csv_write("/etc/lg-magic/calib_accel.csv", s, (size_t)n,
			      err, sizeof(err)) == 0)
			printf("Saved the recording to "
			       "/etc/lg-magic/calib_accel.csv\n");
		a = malloc((size_t)n * sizeof(*a));
		if (!a) {
			free(s);
			fprintf(stderr, "lg-magic: out of memory\n");
			return -1;
		}
		for (i = 0; i < n; i++)
			memcpy(a[i], s[i].accel, sizeof(a[i]));
		lm_fit_accel(a, (size_t)n, c->accel_bias, c->accel_matrix,
			     &cost);
		free(a);
		rms = sqrt(cost);
		spread = accel_spread(s, n);
		free(s);
		printf("mean squared residual: %.6g (rms %.4g m/s^2, %.2f%% of "
		       "g)\n", cost, rms, rms / LM_G * 100.0);
		printf("orientation spread: %.2f of the signal\n", spread);
		if (rms <= GOOD_RMS && spread >= GOOD_SPREAD)
			return 0;
		printf("Poor fit - rotate the remote more, in all axes.\n");
		if (stdin_closed || !ask_yn("Record again", 1))
			break;
	}
	fprintf(stderr, "Warning: accepting a poor accelerometer calibration; "
		"re-run 'sudo lg-magic setup' to retry.\n");
	return 0;
}

static int step_gyro_calib(struct evdev_imu *dev, struct calib *c)
{
	struct imu_sample *s = NULL;
	char err[256];
	double std[3];
	long n, i;
	int j;

	printf("\n=== Step 4: gyroscope calibration ===\n");
	printf("Lay the remote down on a flat surface and do not touch it.\n");
	ask_yn("Start the 10 s recording", 1);
	n = record_samples(dev, GYRO_SECONDS, &s, err, sizeof(err));
	if (n < 0)
		return -1;
	if (g_stop) {
		free(s);
		fprintf(stderr, "Interrupted.\n");
		return -1;
	}
	if (n == 0) {
		fprintf(stderr, "lg-magic: no samples recorded\n");
		free(s);
		return -1;
	}
	if (csv_write("/etc/lg-magic/calib_gyro.csv", s, (size_t)n, err,
		      sizeof(err)) == 0)
		printf("Saved the recording to /etc/lg-magic/calib_gyro.csv\n");
	for (j = 0; j < 3; j++) {
		c->gyro_bias[j] = 0.0;
		for (i = 0; i < n; i++)
			c->gyro_bias[j] += s[i].gyro[j];
		c->gyro_bias[j] /= (double)n;
		std[j] = 0.0;
		for (i = 0; i < n; i++)
			std[j] += (s[i].gyro[j] - c->gyro_bias[j]) *
				  (s[i].gyro[j] - c->gyro_bias[j]);
		std[j] = sqrt(std[j] / (double)n);
	}
	free(s);
	printf("gyro bias: [%.6g %.6g %.6g]\n", c->gyro_bias[0],
	       c->gyro_bias[1], c->gyro_bias[2]);
	printf("gyro noise (std): [%.3g %.3g %.3g]\n", std[0], std[1],
	       std[2]);
	for (j = 0; j < 3; j++)
		if (std[j] > GYRO_STD_WARN) {
			fprintf(stderr, "Warning: the gyro values were moving "
				"during the recording - repeat it if the "
				"airmouse drifts.\n");
			break;
		}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Steps 5-6: tuning questions, JSON, firmware blob                    */
/* ------------------------------------------------------------------ */

static void ask_tuning(struct calib *c, double *alpha, double *mouse_k)
{
	double s;

	printf("\n=== Step 5: calibration JSON ===\n");
	printf("The kernel multiplies (gyro - bias) by the gyro scale. The\n"
	       "README recommends about 0.07; raise it if the pointer feels\n"
	       "too slow.\n");
	s = ask_number("Gyroscope scale", g_cfg->gyro_scale_default,
		       0.001, 10.0);
	c->gyro_scale[0] = c->gyro_scale[1] = c->gyro_scale[2] = s;
	printf("Kernel low-pass alpha (0..1, smaller = smoother pointer):\n");
	*alpha = ask_number("alpha", *alpha, 0.0, 1.0);
	printf("Kernel airmouse sensitivity mouse_k (0..1):\n");
	*mouse_k = ask_number("mouse_k", *mouse_k, 0.0, 1.0);
}

static int write_blob_file(const char *name,
			   const struct lg_magic_airmouse_calib *blob,
			   char *err, size_t errsz)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", FW_DIR, name);
	f = fopen(path, "wb");
	if (!f) {
		snprintf(err, errsz, "cannot write %s: %s", path,
			 strerror(errno));
		return -1;
	}
	if (fwrite(blob, sizeof(*blob), 1, f) != 1 || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %s", path);
		return -1;
	}
	printf("Wrote %s\n", path);
	return 0;
}

static int step_blob(const struct calib *c, double alpha, double mouse_k,
		     const char *uniq)
{
	struct lg_magic_airmouse_calib blob;
	char fname[128], mac[18];
	char err[256];
	int i;

	printf("\n=== Step 6: firmware blob ===\n");
	if (mkdir(FW_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lg-magic: cannot create %s: %s\n", FW_DIR,
			strerror(errno));
		return -1;
	}
	calib_to_blob(c, (float)alpha, (float)mouse_k, &blob);
	if (calib_validate_blob(&blob) < 0)
		fprintf(stderr, "Warning: the blob fails the kernel validation "
			"ranges; writing it anyway (the kernel disables the "
			"airmouse if it rejects it).\n");

	if (uniq[0] && strlen(uniq) == 17) {
		/* Same sanitisation as lgmagic_sanitize_mac(): ':' -> '_'. */
		for (i = 0; i < 17; i++)
			mac[i] = uniq[i] == ':' ? '_' : uniq[i];
		mac[17] = '\0';
		snprintf(fname, sizeof(fname), "lg_magic_calib_%s.bin", mac);
		if (write_blob_file(fname, &blob, err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return -1;
		}
	} else if (uniq[0]) {
		printf("The hidraw uniq string ('%s') is not a MAC - writing "
		       "only the generic blob.\n", uniq);
	} else {
		printf("No uniq string available - writing only the generic "
		       "blob.\n");
	}
	if (write_blob_file(FW_GENERIC, &blob, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 7: reload + dmesg verification                                 */
/* ------------------------------------------------------------------ */

static void step_reload_verify(void)
{
	char err[256];
	int i, rc = -2;

	printf("\n=== Step 7: loading the calibration into the kernel ===\n");
	if (module_reload(MODULE_NAME, err, sizeof(err)) < 0) {
		fprintf(stderr, "Note: %s\n", err);
		fprintf(stderr, "The module is in use by the connected remote. "
			"The calibration blob will be picked up after:\n"
			"  unplug the receiver, run 'sudo rmmod lg_magic', "
			"plug it back in (or reboot).\n");
		return;
	}
	/* request_firmware runs at probe time; give it a moment. */
	for (i = 0; i < 20; i++) {
		usleep(50000);
		rc = dmesg_contains("Loading LG Magic calibration");
		if (rc == 1) {
			printf("The kernel loaded the calibration blob (found "
			       "in dmesg).\n");
			return;
		}
	}
	if (rc == -1)
		fprintf(stderr, "Cannot read dmesg - check manually: "
			"dmesg | grep 'LG Magic'\n");
	else
		fprintf(stderr, "Warning: no 'Loading LG Magic calibration' "
			"in dmesg; the blob may not have been loaded.\n");
}

/* ------------------------------------------------------------------ */
/* Step 8: airmouse test                                               */
/* ------------------------------------------------------------------ */

struct mouse_lpf {
	double alpha;
	double prev[3];
};

static void mouse_lpf_filter(struct mouse_lpf *f, const double g[3],
			     double out[3])
{
	out[0] = f->alpha * g[0] + (1.0 - f->alpha) * f->prev[0];
	out[1] = f->alpha * g[1] + (1.0 - f->alpha) * f->prev[1];
	out[2] = f->alpha * g[2] + (1.0 - f->alpha) * f->prev[2];
	f->prev[0] = out[0];
	f->prev[1] = out[1];
	f->prev[2] = out[2];
}

static void step_mouse_test(struct evdev_imu *dev, const struct calib *c)
{
	struct mouse_lpf filt;
	struct timespec t0, now;
	char err[256];
	int ufd, have_t0 = 0;

	printf("\n=== Step 8: airmouse test ===\n");
	if (non_interactive) {
		printf("Skipped (--non-interactive). Try it later with "
		       "'lg-magic imu --mouse'.\n");
		return;
	}
	if (!ask_yn("Run a quick airmouse test", 1))
		return;
	/* The reload in step 2 may have recreated the evdev node. */
	evdev_close(dev);
	if (evdev_find_imu(dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		return;
	}
	ufd = uinput_open(err, sizeof(err));
	if (ufd < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		evdev_close(dev);
		return;
	}
	filt.alpha = g_cfg->lpf_alpha;
	filt.prev[0] = filt.prev[1] = filt.prev[2] = 0.0;
	g_stop = 0;
	printf("Move the remote - the pointer should follow. Ctrl+C ends the "
	       "test.\n");
	for (;;) {
		unsigned counter;
		double dt, a_corr[3], g_corr[3], filt_out[3];
		int accel[3], gyro[3], dx, dy, rv;

		rv = evdev_read_frame(dev, &counter, &dt, accel, gyro, err,
				      sizeof(err));
		if (rv < 0) {
			if (g_stop)
				break;
			fprintf(stderr, "lg-magic: %s\n", err);
			break;
		}
		if (rv == 0)
			break;
		if (!have_t0) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			have_t0 = 1;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - t0.tv_sec) +
		    (now.tv_nsec - t0.tv_nsec) * 1e-9 >=
		    MOUSE_TEST_SECONDS)
			break;
		calib_apply(c, (double[3]){ accel[0], accel[1], accel[2] },
			    (double[3]){ gyro[0], gyro[1], gyro[2] },
			    a_corr, g_corr);
		mouse_lpf_filter(&filt, g_corr, filt_out);
		dx = (int)(-filt_out[2] * g_cfg->mouse_scale);
		dy = (int)(-filt_out[1] * g_cfg->mouse_scale);
		if (uinput_move(ufd, dx, dy) < 0)
			fprintf(stderr, "lg-magic: uinput write failed\n");
		printf("REL_X : %d REL_Y: %d\n", dx, dy);
	}
	uinput_close(ufd);
	printf("Airmouse test finished.\n");
}

/* ------------------------------------------------------------------ */
/* Step 9: user configuration                                          */
/* ------------------------------------------------------------------ */

static void step_save_user_config(double alpha, double mouse_k)
{
	const char *sudo_user = getenv("SUDO_USER");
	const char *saved_home = getenv("HOME");
	struct passwd *pw = NULL;
	char path[4096] = { 0 };
	char err[256], buf[64];

	printf("\n=== Step 9: user configuration ===\n");
	if (config_set_key(g_cfg, "default_calib", CALIB_JSON, err,
			   sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);
	snprintf(buf, sizeof(buf), "%.4g", alpha);
	if (config_set_key(g_cfg, "alpha", buf, err, sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);
	snprintf(buf, sizeof(buf), "%.4g", mouse_k);
	if (config_set_key(g_cfg, "mouse_k", buf, err, sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);

	/* Under sudo, HOME belongs to root - save into the invoking
	 * user's home and fix the ownership. */
	if (sudo_user)
		pw = getpwnam(sudo_user);
	if (pw) {
		setenv("HOME", pw->pw_dir, 1);
		snprintf(path, sizeof(path), "%s/.config/lg-magic/config.json",
			 pw->pw_dir);
	}
	if (config_save_user(g_cfg, err, sizeof(err)) < 0) {
		fprintf(stderr, "Warning: %s\n", err);
	} else {
		printf("Saved %s\n", path[0] ? path :
		       "~/.config/lg-magic/config.json");
		if (pw) {
			char dir[4096];

			snprintf(dir, sizeof(dir), "%s/.config", pw->pw_dir);
			chown(dir, pw->pw_uid, pw->pw_gid);
			snprintf(dir, sizeof(dir), "%s/.config/lg-magic",
				 pw->pw_dir);
			chown(dir, pw->pw_uid, pw->pw_gid);
			chown(path, pw->pw_uid, pw->pw_gid);
		}
	}
	if (saved_home)
		setenv("HOME", saved_home, 1);
	else
		unsetenv("HOME");
}

/* ------------------------------------------------------------------ */
/* Step 10: summary                                                    */
/* ------------------------------------------------------------------ */

static void step_summary(void)
{
	printf("\n=== Step 10: summary ===\n");
	printf("What was done:\n");
	printf("  - /etc/modprobe.d/lg-magic.conf: imu_evdev=1 airmouse=1\n");
	printf("  - %s (accel + gyro calibration)\n", CALIB_JSON);
	printf("  - %s/calib_accel.csv, calib_gyro.csv (recordings, for "
	       "'lg-magic calibrate')\n", CALIB_DIR);
	printf("  - %s/%s* (per-device + generic kernel blob)\n", FW_DIR,
	       "lg_magic_calib");
	printf("  - user configuration (~/.config/lg-magic/config.json)\n");
	printf("\nUsage:\n");
	printf("  lg-magic imu --mouse     airmouse (virtual mouse)\n");
	printf("  lg-magic imu --ahrs      orientation angles\n");
	printf("  lg-magic imu --cube      terminal cube\n");
	printf("  lg-magic analyze         HID report decoder\n");
	printf("  lg-magic config          show / edit the configuration\n");
	printf("\nRe-run this wizard any time: sudo lg-magic setup\n");
	printf("If the module parameters or the blob have not been applied yet,\n"
	       "unplug the receiver, run 'sudo rmmod lg_magic', and plug it\n"
	       "back in (or reboot).\n");
}

/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
	fputs("Usage: lg-magic setup [--non-interactive]\n"
	      "\n"
	      "Interactive wizard: module parameters, accelerometer and\n"
	      "gyroscope calibration, firmware blob installation and user\n"
	      "configuration. Run as root: sudo lg-magic setup\n"
	      "\n"
	      "  --non-interactive   accept the defaults everywhere\n", out);
}

int cmd_setup(int argc, char **argv)
{
	struct evdev_imu dev;
	struct calib c;
	double alpha, mouse_k;
	char hidraw_path[256], uniq[64], err[256];
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--non-interactive") == 0)
			non_interactive = 1;
		else if (strcmp(argv[i], "--help") == 0 ||
			 strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "lg-magic setup: unexpected argument "
				"'%s'\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}

	printf("LG Magic Remote setup wizard\n");
	sigsetup();

	if (step_environment(&dev, hidraw_path, sizeof(hidraw_path)) < 0)
		return 1;
	if (g_stop) {
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	step_module_params();
	if (g_stop) {
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	if (mkdir(CALIB_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lg-magic: cannot create %s: %s\n", CALIB_DIR,
			strerror(errno));
		return 1;
	}

	/* The reload in step 2 may have recreated the evdev nodes. */
	if (evdev_find_imu(&dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		fprintf(stderr, "The module is not running with imu_evdev=1 "
			"yet - apply the parameters from step 2 and re-run.\n");
		return 1;
	}
	uniq[0] = '\0';
	if (hidraw_get_uniq(hidraw_path, uniq, sizeof(uniq)) == 0 && uniq[0])
		printf("Device MAC: %s\n", uniq);

	calib_init_identity(&c);
	if (step_accel_calib(&dev, &c) < 0)
		return 1;
	if (g_stop) {
		evdev_close(&dev);
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	if (step_gyro_calib(&dev, &c) < 0) {
		evdev_close(&dev);
		return 1;
	}
	if (g_stop) {
		evdev_close(&dev);
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}

	alpha = g_cfg->alpha;
	mouse_k = g_cfg->mouse_k;
	ask_tuning(&c, &alpha, &mouse_k);

	if (calib_save_json(&c, CALIB_JSON, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		evdev_close(&dev);
		return 1;
	}
	printf("Calibration saved to %s\n", CALIB_JSON);

	if (step_blob(&c, alpha, mouse_k, uniq) < 0) {
		evdev_close(&dev);
		return 1;
	}
	step_reload_verify();
	step_mouse_test(&dev, &c);
	step_save_user_config(alpha, mouse_k);
	step_summary();
	evdev_close(&dev);
	return 0;
}

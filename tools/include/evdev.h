/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * evdev.h - Linux evdev access for the IMU device (Linux only).
 */
#ifndef LG_TOOLS_EVDEV_H
#define LG_TOOLS_EVDEV_H

#include <signal.h>
#include <stddef.h>

struct evdev_imu {
	int fd;
	char name[256];
	char path[256];
	unsigned last_counter;
	int have_frame;		/* seen at least one EV_SYN (for the dt quirk) */
	int accel[3];		/* ABS_X/Y/Z */
	int gyro[3];		/* ABS_RX/RY/RZ */
};

/* Set by the SIGINT/SIGTERM handler in cmd_imu.c; evdev_read_frame()
 * returns -1 (errno EINTR) instead of retrying once this is set. */
extern volatile sig_atomic_t g_stop;

/* Find the first /dev/input/event* whose EVIOCGNAME contains "IMU"
 * (same predicate as display_imu.py). wanted_path (non-NULL) overrides
 * detection. Returns 0 (device open, fields filled) or -1 with err. */
int evdev_find_imu(struct evdev_imu *dev, const char *wanted_path,
		   char *err, size_t errsz);

void evdev_close(struct evdev_imu *dev);

/* Read events until the next EV_SYN/SYN_REPORT. Returns 1 with a frame
 * (counter/dt/accel/gyro filled), 0 on EOF, -1 on error.
 * dt = ((counter - last) % 65536) / 256 * 0.02, NAN on the first frame. */
int evdev_read_frame(struct evdev_imu *dev, unsigned *counter, double *dt,
		     int accel[3], int gyro[3], char *err, size_t errsz);

#endif /* LG_TOOLS_EVDEV_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * evdev.c - Linux evdev access for the IMU device.
 *
 * Mirrors scripts/display_imu.py: the device is the first
 * /dev/input/event* whose EVIOCGNAME contains "IMU"; a frame ends on
 * EV_SYN; dt = ((counter - last) % 65536) / 256 * 0.02 (NAN on the first
 * frame). The initial ABS state comes from EVIOCGABS.
 */
#include "evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>

volatile sig_atomic_t g_stop;

#define COUNTS_PER_0P02 256.0
#define T_UNIT 0.02

static int abs_value(int fd, unsigned code)
{
	struct input_absinfo info;

	if (ioctl(fd, EVIOCGABS(code), &info) < 0)
		return 0;
	return info.value;
}

/* Open one path, fill name/path/initial state. Returns 0 or -1. */
static int open_imu(const char *path, struct evdev_imu *dev)
{
	char name[256];

	dev->fd = open(path, O_RDONLY);
	if (dev->fd < 0)
		return -1;
	if (ioctl(dev->fd, EVIOCGNAME(sizeof(name)), name) < 0)
		name[0] = '\0';
	name[sizeof(name) - 1] = '\0';
	snprintf(dev->name, sizeof(dev->name), "%s", name);
	snprintf(dev->path, sizeof(dev->path), "%s", path);

	/* "Initial state" in display_imu.py comes from the absinfo values. */
	dev->accel[0] = abs_value(dev->fd, ABS_X);
	dev->accel[1] = abs_value(dev->fd, ABS_Y);
	dev->accel[2] = abs_value(dev->fd, ABS_Z);
	dev->gyro[0] = abs_value(dev->fd, ABS_RX);
	dev->gyro[1] = abs_value(dev->fd, ABS_RY);
	dev->gyro[2] = abs_value(dev->fd, ABS_RZ);
	dev->last_counter = 0;
	dev->have_frame = 0;
	return 0;
}

int evdev_find_imu(struct evdev_imu *dev, const char *wanted_path,
		   char *err, size_t errsz)
{
	struct dirent *de;
	DIR *dir;

	if (wanted_path) {
		if (open_imu(wanted_path, dev) < 0) {
			snprintf(err, errsz, "cannot open %s: %s", wanted_path,
				 strerror(errno));
			return -1;
		}
		return 0;
	}

	dir = opendir("/dev/input");
	if (!dir) {
		snprintf(err, errsz, "cannot scan /dev/input: %s",
			 strerror(errno));
		return -1;
	}
	while ((de = readdir(dir)) != NULL) {
		char path[sizeof(de->d_name) + 16];

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		if (open_imu(path, dev) < 0)
			continue;
		/* Same predicate as Python: "IMU" in dev.name. */
		if (strstr(dev->name, "IMU")) {
			closedir(dir);
			return 0;
		}
		evdev_close(dev);
	}
	closedir(dir);
	snprintf(err, errsz, "no IMU evdev device found in /dev/input/");
	return -1;
}

void evdev_close(struct evdev_imu *dev)
{
	if (dev->fd >= 0) {
		close(dev->fd);
		dev->fd = -1;
	}
}

int evdev_read_frame(struct evdev_imu *dev, unsigned *counter, double *dt,
		     int accel[3], int gyro[3], char *err, size_t errsz)
{
	struct input_event ev;
	ssize_t rv;

	for (;;) {
		rv = read(dev->fd, &ev, sizeof(ev));
		if (rv < 0) {
			if (errno == EINTR) {
				if (g_stop)
					return -1;
				continue;
			}
			snprintf(err, errsz, "read error on %s: %s", dev->path,
				 strerror(errno));
			return -1;
		}
		if (rv == 0)
			return 0;	/* EOF */
		if (rv != sizeof(ev))
			continue;	/* partial read - wait for the rest */

		switch (ev.type) {
		case EV_ABS:
			switch (ev.code) {
			case ABS_X: dev->accel[0] = ev.value; break;
			case ABS_Y: dev->accel[1] = ev.value; break;
			case ABS_Z: dev->accel[2] = ev.value; break;
			case ABS_RX: dev->gyro[0] = ev.value; break;
			case ABS_RY: dev->gyro[1] = ev.value; break;
			case ABS_RZ: dev->gyro[2] = ev.value; break;
			}
			break;
		case EV_MSC:
			if (ev.code == MSC_SERIAL)
				*counter = (unsigned)ev.value;
			break;
		case EV_SYN:
			if (dev->have_frame) {
				unsigned delta = (*counter -
						  dev->last_counter) % 65536;

				*dt = (delta / COUNTS_PER_0P02) * T_UNIT;
			} else {
				*dt = NAN;	/* Python: None on frame 1 */
				dev->have_frame = 1;
			}
			dev->last_counter = *counter;
			accel[0] = dev->accel[0];
			accel[1] = dev->accel[1];
			accel[2] = dev->accel[2];
			gyro[0] = dev->gyro[0];
			gyro[1] = dev->gyro[1];
			gyro[2] = dev->gyro[2];
			return 1;
		}
	}
}

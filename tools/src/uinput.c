/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uinput.c - virtual mouse via /dev/uinput (scripts/uinput_mouse.py).
 *
 * Creates a device with REL_X/REL_Y + BTN_LEFT/BTN_RIGHT (the same event
 * set as the Python script) and emits relative moves + SYN_REPORT.
 */
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/uinput.h>

int uinput_open(char *err, size_t errsz)
{
	static const char *paths[] = { "/dev/uinput", "/dev/input/uinput" };
	int fd = -1;
	int last_errno = 0;
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fd = open(paths[i], O_WRONLY | O_NONBLOCK);
		if (fd >= 0)
			break;
		last_errno = errno;
	}
	if (fd < 0) {
		if (last_errno == ENOENT)
			snprintf(err, errsz, "no /dev/uinput device - "
				 "run 'modprobe uinput' first");
		else if (last_errno == EACCES)
			snprintf(err, errsz, "%s: permission denied - "
				 "run as root or join the input group",
				 paths[0]);
		else
			snprintf(err, errsz, "cannot open %s: %s", paths[0],
				 strerror(last_errno));
		return -1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
	    ioctl(fd, UI_SET_RELBIT, REL_Y) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) {
		snprintf(err, errsz, "uinput setup failed: %s",
			 strerror(errno));
		close(fd);
		return -1;
	}
	{
		struct uinput_setup usetup;

		memset(&usetup, 0, sizeof(usetup));
		snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "lg-magic mouse");
		usetup.id.bustype = BUS_USB;
		usetup.id.vendor = 0x1;
		usetup.id.product = 0x1;
		if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
		    ioctl(fd, UI_DEV_CREATE) < 0) {
			snprintf(err, errsz, "uinput create failed: %s",
				 strerror(errno));
			close(fd);
			return -1;
		}
	}
	return fd;
}

void uinput_close(int fd)
{
	if (fd < 0)
		return;
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
}

int uinput_move(int fd, int dx, int dy)
{
	struct input_event ev[3];

	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_REL;
	ev[0].code = REL_X;
	ev[0].value = dx;
	ev[1].type = EV_REL;
	ev[1].code = REL_Y;
	ev[1].value = dy;
	ev[2].type = EV_SYN;
	ev[2].code = SYN_REPORT;
	ev[2].value = 0;
	return write(fd, ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_devices.h - lg-magicd device discovery and hotplug (Linux only).
 *
 * Scans /dev/input (name + id + sysfs uniq), pairs each "LG Magic
 * Remote" keyboard with its IMU (pairing_match), opens the keyboard
 * with EVIOCGRAB (best effort) and the IMU without a grab (so
 * `lg-magic imu --csv/--mouse` keeps working in parallel).
 *
 * Hotplug: inotify on /dev/input with a 200 ms debounce, plus a slow
 * polling fallback rescan for environments without inotify/udev.
 * The grab is released whenever the fd closes - a crashed daemon
 * returns the remote to raw kernel events automatically.
 */
#ifndef LG_TOOLS_DAEMON_DEVICES_H
#define LG_TOOLS_DAEMON_DEVICES_H

#include <poll.h>
#include <stddef.h>

#include "daemon_config.h"
#include "daemon_pipeline.h"
#include "evdev.h"

/* One discovered remote. */
struct daemon_remote {
	char identity[64];	/* BT MAC or "unknown" (no uniq) */
	char kbd_path[256];
	char imu_path[256];	/* "" when the IMU is absent */
	struct evdev_imu kbd;	/* open, grabbed (best effort) */
	struct evdev_imu imu;	/* open, NOT grabbed; fd -1 when absent */
	struct device_config dc;	/* resolved per-remote config */
	struct pipeline pl;
};

struct daemon_devices {
	struct daemon_remote *remotes;
	size_t nremotes;
	struct daemon_config *config;
	const char *kbd_override;	/* --keyboard PATH (test mode) */
	int inotify_fd;
	int inotify_wd;			/* -1 when not watching */
	int have_rescan;		/* a debounced rescan is scheduled */
	long long rescan_due_ms;	/* monotonic deadline */
	long long last_rescan_ms;
	int debug;
};

/* Set up device discovery (inotify best effort). Returns 0 / -1. */
int daemon_devices_init(struct daemon_devices *dd, struct daemon_config *config,
			const char *kbd_override, int debug,
			char *err, size_t errsz);

/* Full rescan: probe /dev/input, re-pair, open/grab what appeared or
 * reconnected, close what disappeared.  On pairing errors (ambiguous
 * set) the existing remotes are KEPT and -1 is returned with err.
 * Returns 0 / -1. */
int daemon_devices_rescan(struct daemon_devices *dd, char *err, size_t errsz);

/* Fill pollfds (inotify + one entry per open device). Returns the
 * count; the layout matches daemon_devices_handle(). */
int daemon_devices_pollfds(struct daemon_devices *dd, struct pollfd *fds,
			   size_t nfds);

/* Handle one ready pollfd entry (idx from 0, same layout as
 * daemon_devices_pollfds): a device frame is pushed through the
 * pipeline and emitted on the uinput fds.  Returns 1 when a frame was
 * handled, 0 when nothing was (inotify drained, rescan scheduled), -1
 * when a device was lost (a rescan is already scheduled). */
int daemon_devices_handle(struct daemon_devices *dd, size_t idx,
			  int kbd_uinput, int mouse_uinput);

/* Milliseconds to sleep before the next scheduled rescan (<= 0 = now).
 * Combines the debounced inotify rescan with the polling fallback, so
 * the caller polls with this timeout instead of blocking forever. */
long long daemon_devices_rescan_delay_ms(const struct daemon_devices *dd,
					 long long now_ms);

/* SIGHUP: reload the global config and every remote's config +
 * pipeline.  Returns 0 / -1 (OOM only - per-remote failures are logged
 * and keep the previous state). */
int daemon_devices_reload(struct daemon_devices *dd, char *err, size_t errsz);

void daemon_devices_free(struct daemon_devices *dd);

#endif /* LG_TOOLS_DAEMON_DEVICES_H */

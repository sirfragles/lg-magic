/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_main.c - lg-magicd, the system daemon.
 *
 * Startup order (safety requirement from the plan): create BOTH uinput
 * devices, then scan and EVIOCGRAB the keyboard, then the poll loop.
 * On any exit - including SIGKILL - the grab dies with the fd and the
 * remote falls back to raw kernel events.
 *
 * The sd-bus interface (org.lgmagic, see daemon_bus.c) is optional:
 * without a system bus the daemon logs and keeps running busless (the
 * busless e2e mode).  The bus fd is appended to the device pollfds and
 * driven manually from the same poll() loop.
 *
 * Test flags: --keyboard PATH (pin the keyboard), --config-root DIR,
 * --state-dir DIR, --no-uinput (skip the virtual devices - the bus
 * smoke runs without /dev/uinput), --debug.
 */
#include "daemon_bus.h"
#include "daemon_config.h"
#include "daemon_devices.h"
#include "evdev.h"
#include "uinput.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_CONFIG_ROOT "/etc/lg-magic"
#define DEFAULT_STATE_DIR "/var/lib/lg-magic"

static volatile sig_atomic_t g_reload;

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		g_reload = 1;
	else
		g_stop = 1;
}

static void usage(FILE *out)
{
	fputs("Usage: lg-magicd [--keyboard PATH] [--config-root DIR]\n"
	      "                  [--state-dir DIR] [--no-uinput] [--debug]\n", out);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The two virtual devices: the keyboard carries the full EV_KEY set
 * (mapped keys + mouse buttons are all keycodes), the mouse carries
 * the relative axes. */
static int create_uinput_devices(int *kbd_fd, int *mouse_fd,
				 char *err, size_t errsz)
{
	struct uinput_spec kbd, mouse;
	int i;

	uinput_spec_init(&kbd, "lg-magicd keyboard");
	for (i = 0; i < KEY_CNT; i++)
		uinput_spec_key(&kbd, (unsigned)i);

	uinput_spec_init(&mouse, "lg-magicd mouse");
	uinput_spec_rel(&mouse, REL_X);
	uinput_spec_rel(&mouse, REL_Y);
	uinput_spec_rel(&mouse, REL_WHEEL);
	uinput_spec_rel(&mouse, REL_WHEEL_HI_RES);
	uinput_spec_rel(&mouse, REL_HWHEEL);
	uinput_spec_key(&mouse, BTN_LEFT);
	uinput_spec_key(&mouse, BTN_RIGHT);
	uinput_spec_key(&mouse, BTN_MIDDLE);

	*kbd_fd = uinput_create(&kbd, err, errsz);
	if (*kbd_fd < 0)
		return -1;
	*mouse_fd = uinput_create(&mouse, err, errsz);
	if (*mouse_fd < 0) {
		uinput_close(*kbd_fd);
		*kbd_fd = -1;
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *config_root = DEFAULT_CONFIG_ROOT;
	const char *state_dir = DEFAULT_STATE_DIR;
	const char *kbd_override = NULL;
	int debug = 0;
	int no_uinput = 0;
	struct daemon_config dc;
	struct daemon_devices dd;
	struct sigaction sa;
	struct pollfd *fds = NULL;
	size_t nfds = 0;
	char err[256];
	sd_bus *bus = NULL;
	int kbd_uinput = -1, mouse_uinput = -1;
	int ndev;
	int i, ret = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--keyboard") == 0 && i + 1 < argc)
			kbd_override = argv[++i];
		else if (strncmp(argv[i], "--keyboard=", 11) == 0)
			kbd_override = argv[i] + 11;
		else if (strcmp(argv[i], "--config-root") == 0 && i + 1 < argc)
			config_root = argv[++i];
		else if (strncmp(argv[i], "--config-root=", 14) == 0)
			config_root = argv[i] + 14;
		else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc)
			state_dir = argv[++i];
		else if (strncmp(argv[i], "--state-dir=", 12) == 0)
			state_dir = argv[i] + 12;
		else if (strcmp(argv[i], "--debug") == 0)
			debug = 1;
		else if (strcmp(argv[i], "--no-uinput") == 0)
			no_uinput = 1;
		else if (strcmp(argv[i], "--help") == 0 ||
			 strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "lg-magicd: unexpected argument '%s'\n",
				argv[i]);
			usage(stderr);
			return 1;
		}
	}

	if (daemon_config_init(&dc, config_root, state_dir, err,
			       sizeof(err)) < 0) {
		fprintf(stderr, "lg-magicd: %s\n", err);
		return 1;
	}

	/* 1. The virtual devices BEFORE anything is grabbed (a daemon
	 * restart must never leave the remote without a target). */
	if (no_uinput) {
		fprintf(stderr, "lg-magicd: --no-uinput: skipping the virtual "
			"devices\n");
	} else if (create_uinput_devices(&kbd_uinput, &mouse_uinput, err,
					 sizeof(err)) < 0) {
		fprintf(stderr, "lg-magicd: %s\n", err);
		ret = 1;
		goto out;
	} else {
		fprintf(stderr, "lg-magicd: virtual devices ready\n");
	}

	/* 2. Discover + open + grab; uinput already exists. */
	if (daemon_devices_init(&dd, &dc, kbd_override, debug, err,
				sizeof(err)) < 0) {
		fprintf(stderr, "lg-magicd: %s\n", err);
		ret = 1;
		goto out;
	}
	/* A failed first scan is not fatal: the polling fallback retries. */
	if (daemon_devices_rescan(&dd, err, sizeof(err)) < 0 && debug)
		fprintf(stderr, "lg-magicd: %s\n", err);

	/* 2b. The system bus (optional - without one the daemon runs
	 * busless, e.g. in the busless e2e). */
	bus = daemon_bus_open(&dd, err, sizeof(err));
	if (!bus)
		fprintf(stderr, "lg-magicd: %s (running without the bus)\n", err);
	else
		fprintf(stderr, "lg-magicd: bus name %s acquired\n", LG_BUS_NAME);

	/* 3. Signals: SIGHUP reloads, SIGINT/SIGTERM shut down (the grab
	 * is released by the cleanup below). */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* 4. The main loop. */
	for (;;) {
		long long delay;
		int n, prc;

		if (g_stop)
			break;
		if (g_reload) {
			g_reload = 0;
			fprintf(stderr, "lg-magicd: reloading config\n");
			if (daemon_devices_reload(&dd, err, sizeof(err)) < 0)
				fprintf(stderr, "lg-magicd: %s\n", err);
		}

		{
			/* inotify + up to two fds per remote + the bus */
			size_t need = 1 + 2 * dd.nremotes + (bus ? 1 : 0);
			int bfd;

			if (need > nfds) {
				struct pollfd *p = realloc(fds, need * sizeof(*fds));

				if (!p) {
					fprintf(stderr, "lg-magicd: out of "
						"memory\n");
					ret = 1;
					break;
				}
				fds = p;
				nfds = need;
			}
			n = daemon_devices_pollfds(&dd, fds, nfds);
			ndev = n;	/* the bus entry, when appended, is at ndev */
			bfd = bus ? daemon_bus_fd(bus) : -1;
			if (bfd >= 0 && (size_t)n < nfds) {
				fds[n].fd = bfd;
				/* POLLOUT only while the queue is non-empty;
				 * unconditional level-triggered POLLOUT would make
				 * poll() return immediately forever (100% CPU) */
				fds[n].events = daemon_bus_events(bus);
				fds[n].revents = 0;
				n++;
			}
		}

		delay = daemon_devices_rescan_delay_ms(&dd, now_ms());
		if (delay <= 0) {
			if (daemon_devices_rescan(&dd, err, sizeof(err)) < 0 &&
			    debug)
				fprintf(stderr, "lg-magicd: %s\n", err);
			continue;
		}
		prc = poll(fds, n, (int)delay);
		if (prc < 0 && errno == EINTR)
			continue;
		if (prc <= 0)
			continue;	/* timeout: the fallback rescan fires */
		for (i = 0; i < n; i++) {
			if (bus && i == ndev && fds[i].revents) {
				if (daemon_bus_process(bus, fds[i].revents) < 0) {
					fprintf(stderr, "lg-magicd: fatal bus "
						"error, running busless\n");
					daemon_bus_close(bus);
					bus = NULL;
				}
				continue;
			}
			if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
				(void)daemon_devices_handle(&dd, (size_t)i,
							    kbd_uinput,
							    mouse_uinput);
		}
	}

	/* The fds (and with them the grab) are released here, then the
	 * virtual devices are destroyed. */
	daemon_bus_close(bus);
	daemon_devices_free(&dd);
out:
	uinput_close(kbd_uinput);
	uinput_close(mouse_uinput);
	daemon_config_free(&dc);
	free(fds);
	return ret;
}

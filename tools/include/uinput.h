/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uinput.h - virtual mouse via /dev/uinput (Linux only).
 */
#ifndef LG_TOOLS_UINPUT_H
#define LG_TOOLS_UINPUT_H

#include <stddef.h>

/* Create a virtual mouse device (REL_X/REL_Y + BTN_LEFT/BTN_RIGHT).
 * Tries /dev/uinput first, then /dev/input/uinput. Returns the fd or -1
 * with err (ENOENT -> "modprobe uinput", EACCES -> root/input group). */
int uinput_open(char *err, size_t errsz);
void uinput_close(int fd);

/* Emit a relative move + SYN_REPORT. Returns 0 or -1. */
int uinput_move(int fd, int dx, int dy);

#endif /* LG_TOOLS_UINPUT_H */

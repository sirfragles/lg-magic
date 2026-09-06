/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * commands.h - subcommand entry points of the multi-call lg-magic binary.
 * Each command returns its exit code (0 = success).
 */
#ifndef LG_TOOLS_COMMANDS_H
#define LG_TOOLS_COMMANDS_H

int cmd_imu(int argc, char **argv);
int cmd_analyze(int argc, char **argv);
int cmd_calibrate(int argc, char **argv);
int cmd_calib2bin(int argc, char **argv);
int cmd_config(int argc, char **argv);
int cmd_setup(int argc, char **argv);

#endif /* LG_TOOLS_COMMANDS_H */

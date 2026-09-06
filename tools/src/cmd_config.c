/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_config.c - `lg-magic config` subcommand.
 *
 *   lg-magic config                print the effective configuration
 *   lg-magic config set KEY VALUE  set one key, save to the user file
 *   lg-magic config path           print the config file paths
 *
 * Keys: imu_device, hidraw_device, default_calib (strings) and
 * lpf_alpha, mouse_scale, madgwick_beta, alpha, mouse_k,
 * gyro_scale_default (numbers).
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
	fputs("Usage: lg-magic config [show|set KEY VALUE|path]\n"
	      "\n"
	      "  (no argument)   print the effective configuration\n"
	      "  set KEY VALUE   set one key and save it to\n"
	      "                  ~/.config/lg-magic/config.json\n"
	      "  path            print the config file locations\n"
	      "\n"
	      "Keys (strings): imu_device, hidraw_device, default_calib\n"
	      "Keys (numbers): lpf_alpha, mouse_scale, madgwick_beta,\n"
	      "                alpha, mouse_k, gyro_scale_default\n", out);
}

int cmd_config(int argc, char **argv)
{
	char err[256];

	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}

	/* No arguments: show the effective configuration. */
	if (argc == 1) {
		config_print(g_cfg);
		return 0;
	}

	if (strcmp(argv[1], "show") == 0) {
		if (argc != 2) {
			usage(stderr);
			return 1;
		}
		config_print(g_cfg);
		return 0;
	}

	if (strcmp(argv[1], "path") == 0) {
		if (argc != 2) {
			usage(stderr);
			return 1;
		}
		config_print_paths();
		return 0;
	}

	if (strcmp(argv[1], "set") == 0) {
		if (argc != 4) {
			fprintf(stderr, "usage: lg-magic config set "
				"KEY VALUE\n");
			return 1;
		}
		if (config_set_key(g_cfg, argv[2], argv[3],
				   err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return 1;
		}
		if (config_save_user(g_cfg, err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return 1;
		}
		printf("%s = %s\n", argv[2], argv[3]);
		return 0;
	}

	fprintf(stderr, "lg-magic config: unknown argument '%s'\n\n", argv[1]);
	usage(stderr);
	return 1;
}

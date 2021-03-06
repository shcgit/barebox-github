// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: © 2012 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix

/* dirname.c - strip directory and suffix from filenames */

#include <common.h>
#include <command.h>
#include <libgen.h>
#include <environment.h>
#include <fs.h>
#include <getopt.h>

static int do_dirname(int argc, char *argv[])
{
	int opt;
	int path_fs = 0;
	int len = 0;

	while ((opt = getopt(argc, argv, "V")) > 0) {
		switch (opt) {
		case 'V':
			path_fs = 1;
			break;
		}
	}

	if (argc < optind + 2)
		return COMMAND_ERROR_USAGE;

	if (path_fs)
		len = strlen(get_mounted_path(argv[optind]));

	setenv(argv[optind + 1], dirname(argv[optind]) + len);

	return 0;
}

BAREBOX_CMD_HELP_START(dirname)
BAREBOX_CMD_HELP_TEXT("Strip last componext of NAME and store into $DIRNAME")
BAREBOX_CMD_HELP_TEXT("")
BAREBOX_CMD_HELP_TEXT("Options:")
BAREBOX_CMD_HELP_OPT("-V", "return the path relative to the mountpoint.")
BAREBOX_CMD_HELP_END

BAREBOX_CMD_START(dirname)
	.cmd		= do_dirname,
	BAREBOX_CMD_DESC("strip last component from a path")
	BAREBOX_CMD_OPTS("[-V] NAME DIRNAME")
	BAREBOX_CMD_GROUP(CMD_GRP_FILE)
	BAREBOX_CMD_HELP(cmd_dirname_help)
BAREBOX_CMD_END

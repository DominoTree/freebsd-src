/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nick@spun.io>
 */

#include <sys/auxv.h>

#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Report AT_EXECPATH, then block until killed.  Executed from a file mount by
 * the filemount_path tests.
 */
int
main(void)
{
	char path[PATH_MAX];
	int error;

	error = elf_aux_info(AT_EXECPATH, path, sizeof(path));
	if (error != 0)
		errc(1, error, "elf_aux_info");
	printf("%s\n", path);
	fflush(stdout);

	/* Give the parent a bounded window in case it dies without killing us. */
	alarm(60);
	pause();
	return (0);
}

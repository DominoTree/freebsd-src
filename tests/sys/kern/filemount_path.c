/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nick@spun.io>
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <mntopts.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#define	COVER		"worker"
#define	CANARY		0xaa
#define	PROBE_BUFSZ	(2 * PATH_MAX)

static void
mount_file(const struct atf_tc *tc, const char *target)
{
	struct iovec *iov;
	char errmsg[1024], from[PATH_MAX];
	int error, iovlen;

	snprintf(from, sizeof(from), "%s/filemount_helper",
	    atf_tc_get_config_var(tc, "srcdir"));

	iov = NULL;
	iovlen = 0;
	build_iovec(&iov, &iovlen, __DECONST(char *, "fstype"),
	    __DECONST(char *, "nullfs"), (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "fspath"),
	    __DECONST(char *, target), (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "from"), from, (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "errmsg"), errmsg,
	    sizeof(errmsg));

	errmsg[0] = '\0';
	error = nmount(iov, iovlen, 0);
	ATF_REQUIRE_MSG(error == 0, "nmount %s on %s: %s", from, target,
	    errmsg[0] != '\0' ? errmsg : strerror(errno));

	free_iovec(&iov, &iovlen);
}

static void
unmount_cover(void)
{
	if (unmount(COVER, 0) == 0)
		return;
	if (errno == EBUSY)
		(void)unmount(COVER, MNT_FORCE);
}

/*
 * Create the file to mount over and return its expected resolved path.  The
 * mount root is not a directory and its parent is vp_crossmp, so the kernel
 * must resolve it via the covered vnode, which is this file.
 */
static void
make_cover(char *expected, size_t expectedlen)
{
	char cwd[PATH_MAX];
	int fd;

	fd = open(COVER, O_CREAT | O_RDWR, 0755);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", COVER, strerror(errno));
	ATF_REQUIRE(close(fd) == 0);

	ATF_REQUIRE_MSG(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd: %s",
	    strerror(errno));
	snprintf(expected, expectedlen, "%s/%s", cwd, COVER);
}

/*
 * __realpathat(2) is not exposed by libc; realpath(3) always passes PATH_MAX
 * and so cannot exercise a caller-supplied size.
 */
static void
check_realpathat(const char *path, size_t size, int experr,
    const char *expected)
{
	char buf[PROBE_BUFSZ], canary[PROBE_BUFSZ];
	size_t len;
	int ret;

	memset(buf, CANARY, sizeof(buf));
	memset(canary, CANARY, sizeof(canary));

	ret = syscall(SYS___realpathat, AT_FDCWD, path, buf, size, 0);

	if (experr != 0) {
		ATF_REQUIRE_ERRNO(experr, ret == -1);
		ATF_REQUIRE_MSG(memcmp(buf, canary, sizeof(buf)) == 0,
		    "__realpathat(%s, size=%zu) failed but wrote to the buffer",
		    path, size);
		return;
	}

	ATF_REQUIRE_MSG(ret == 0, "__realpathat(%s, size=%zu): %s", path, size,
	    strerror(errno));
	ATF_REQUIRE_STREQ(expected, buf);

	len = strlen(buf) + 1;
	ATF_REQUIRE_MSG(memcmp(buf + len, canary, sizeof(buf) - len) == 0,
	    "__realpathat(%s, size=%zu) wrote past the path", path, size);
}

ATF_TC_WITH_CLEANUP(realpath_file_mount);
ATF_TC_HEAD(realpath_file_mount, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "realpath(3) resolves a file mount via the covered vnode");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(realpath_file_mount, tc)
{
	char expected[PATH_MAX], resolved[PATH_MAX];

	make_cover(expected, sizeof(expected));
	mount_file(tc, COVER);

	ATF_REQUIRE_MSG(realpath(COVER, resolved) != NULL, "realpath: %s",
	    strerror(errno));
	ATF_REQUIRE_STREQ(expected, resolved);
}
ATF_TC_CLEANUP(realpath_file_mount, tc)
{
	unmount_cover();
}

ATF_TC_WITH_CLEANUP(realpathat_file_mount_size);
ATF_TC_HEAD(realpathat_file_mount_size, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "__realpathat(2) honors the caller's buffer size for a file mount");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(realpathat_file_mount_size, tc)
{
	char expected[PATH_MAX];

	make_cover(expected, sizeof(expected));
	mount_file(tc, COVER);

	/* Otherwise the undersized cases below are not undersized. */
	ATF_REQUIRE(strlen(expected) > 16);

	check_realpathat(COVER, 0, ENAMETOOLONG, NULL);
	check_realpathat(COVER, 1, ENAMETOOLONG, NULL);
	check_realpathat(COVER, 16, ENAMETOOLONG, NULL);
	check_realpathat(COVER, strlen(expected), ENAMETOOLONG, NULL);
	check_realpathat(COVER, strlen(expected) + 1, 0, expected);
	check_realpathat(COVER, PATH_MAX, 0, expected);
}
ATF_TC_CLEANUP(realpathat_file_mount_size, tc)
{
	unmount_cover();
}

ATF_TC(realpathat_regular_size);
ATF_TC_HEAD(realpathat_regular_size, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "__realpathat(2) honors the caller's buffer size for a regular file");
}
ATF_TC_BODY(realpathat_regular_size, tc)
{
	char expected[PATH_MAX];

	make_cover(expected, sizeof(expected));

	ATF_REQUIRE(strlen(expected) > 16);

	check_realpathat(COVER, 0, EINVAL, NULL);
	check_realpathat(COVER, 1, EINVAL, NULL);
	check_realpathat(COVER, 16, ENOMEM, NULL);
	check_realpathat(COVER, strlen(expected) + 1, 0, expected);
	check_realpathat(COVER, PATH_MAX, 0, expected);
}

ATF_TC_WITH_CLEANUP(exec_file_mount);
ATF_TC_HEAD(exec_file_mount, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "AT_EXECPATH and KERN_PROC_PATHNAME resolve a binary executed from "
	    "a file mount; querying the latter panicked INVARIANTS kernels");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(exec_file_mount, tc)
{
	char execpath[PATH_MAX], expected[PATH_MAX], pathname[PATH_MAX];
	size_t len;
	FILE *fp;
	pid_t pid;
	int fds[2], mib[4], status;

	make_cover(expected, sizeof(expected));
	mount_file(tc, COVER);

	ATF_REQUIRE(pipe(fds) == 0);

	pid = fork();
	ATF_REQUIRE_MSG(pid >= 0, "fork: %s", strerror(errno));
	if (pid == 0) {
		(void)close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(126);
		/* Relative, so that exec resolves AT_EXECPATH itself. */
		execl("./" COVER, COVER, NULL);
		_exit(127);
	}
	ATF_REQUIRE(close(fds[1]) == 0);

	fp = fdopen(fds[0], "r");
	ATF_REQUIRE(fp != NULL);
	ATF_REQUIRE_MSG(fgets(execpath, sizeof(execpath), fp) != NULL,
	    "child did not report AT_EXECPATH");
	execpath[strcspn(execpath, "\n")] = '\0';
	ATF_CHECK_STREQ(expected, execpath);

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PATHNAME;
	mib[3] = pid;
	len = sizeof(pathname);
	ATF_CHECK_MSG(sysctl(mib, 4, pathname, &len, NULL, 0) == 0,
	    "KERN_PROC_PATHNAME: %s", strerror(errno));
	ATF_CHECK_STREQ(expected, pathname);

	ATF_REQUIRE(kill(pid, SIGKILL) == 0);
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_REQUIRE(fclose(fp) == 0);
}
ATF_TC_CLEANUP(exec_file_mount, tc)
{
	unmount_cover();
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, realpath_file_mount);
	ATF_TP_ADD_TC(tp, realpathat_file_mount_size);
	ATF_TP_ADD_TC(tp, realpathat_regular_size);
	ATF_TP_ADD_TC(tp, exec_file_mount);

	return (atf_no_error());
}

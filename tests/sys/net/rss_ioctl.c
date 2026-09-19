/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>
 */

#include <sys/param.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>

#include <sys/wait.h>

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static int
rss_socket(void)
{
	int s;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	ATF_REQUIRE_MSG(s >= 0, "socket: %s", strerror(errno));
	return (s);
}

static void
rss_key_init(struct ifrsskey *ifrk, const char *ifname)
{

	memset(ifrk, 0, sizeof(*ifrk));
	strlcpy(ifrk->ifrk_name, ifname, sizeof(ifrk->ifrk_name));
	ifrk->ifrk_func = RSS_FUNC_TOEPLITZ;
	ifrk->ifrk_keylen = 40;
}

static void
rss_table_init(struct ifrsstable *ifrt, const char *ifname)
{

	memset(ifrt, 0, sizeof(*ifrt));
	strlcpy(ifrt->ifrt_name, ifname, sizeof(ifrt->ifrt_name));
}

static void
rss_hash_init(struct ifrsshash *ifrh, const char *ifname)
{

	memset(ifrh, 0, sizeof(*ifrh));
	strlcpy(ifrh->ifrh_name, ifname, sizeof(ifrh->ifrh_name));
}

ATF_TC_WITHOUT_HEAD(rss_layout);
ATF_TC_BODY(rss_layout, tc)
{

	ATF_CHECK_EQ(148, sizeof(struct ifrsskey));
	ATF_CHECK_EQ(24, sizeof(struct ifrsshash));
	ATF_CHECK_EQ(4116, sizeof(struct ifrsstable));
	ATF_CHECK_EQ(2048, RSS_TABLELEN);
	ATF_CHECK_EQ(16, __offsetof(struct ifrsskey, ifrk_func));
	ATF_CHECK_EQ(20, __offsetof(struct ifrsstable, ifrt_table));

	ATF_CHECK_EQ(sizeof(struct ifrsskey), IOCPARM_LEN(SIOCGIFRSSKEY));
	ATF_CHECK_EQ(sizeof(struct ifrsskey), IOCPARM_LEN(SIOCSIFRSSKEY));
	ATF_CHECK_EQ(sizeof(struct ifrsshash), IOCPARM_LEN(SIOCGIFRSSHASH));
	ATF_CHECK_EQ(sizeof(struct ifrsstable), IOCPARM_LEN(SIOCGIFRSSTABLE));
	ATF_CHECK_EQ(sizeof(struct ifrsstable), IOCPARM_LEN(SIOCSIFRSSTABLE));
	ATF_CHECK(sizeof(struct ifrsstable) <= IOCPARM_MAX);
}

/* The queries need no privilege and fail on an interface without RSS. */
ATF_TC_WITHOUT_HEAD(rss_get_unsupported);
ATF_TC_BODY(rss_get_unsupported, tc)
{
	struct ifrsskey ifrk;
	struct ifrsshash ifrh;
	struct ifrsstable *ifrt;
	int s;

	s = rss_socket();
	ifrt = malloc(sizeof(*ifrt));
	ATF_REQUIRE(ifrt != NULL);

	rss_key_init(&ifrk, "lo0");
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSKEY, &ifrk) == -1);
	rss_hash_init(&ifrh, "lo0");
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSHASH, &ifrh) == -1);
	rss_table_init(ifrt, "lo0");
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSTABLE, ifrt) == -1);

	free(ifrt);
	close(s);
}

enum {
	RSS_PRIV_EPERM = 0,
	RSS_PRIV_SETEUID_FAIL = 1,
	RSS_PRIV_SOCKET_FAIL = 2,
	RSS_PRIV_NOT_EPERM = 3,
	RSS_PRIV_NOMEM = 4,
};

ATF_TC(rss_set_unprivileged);
ATF_TC_HEAD(rss_set_unprivileged, tc)
{

	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.config", "unprivileged_user");
}
ATF_TC_BODY(rss_set_unprivileged, tc)
{
	const char *user;
	pid_t child;
	int status;

	user = atf_tc_get_config_var(tc, "unprivileged_user");

	child = fork();
	ATF_REQUIRE(child != -1);
	if (child == 0) {
		struct ifrsskey ifrk;
		struct ifrsstable *ifrt;
		struct passwd *passwd;
		int s;

		passwd = getpwnam(user);
		if (passwd == NULL || seteuid(passwd->pw_uid) != 0)
			_exit(RSS_PRIV_SETEUID_FAIL);
		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s < 0)
			_exit(RSS_PRIV_SOCKET_FAIL);
		ifrt = malloc(sizeof(*ifrt));
		if (ifrt == NULL)
			_exit(RSS_PRIV_NOMEM);

		rss_key_init(&ifrk, "lo0");
		if (ioctl(s, SIOCSIFRSSKEY, &ifrk) != -1 || errno != EPERM)
			_exit(RSS_PRIV_NOT_EPERM);

		/* A malformed request is refused for the same reason. */
		rss_key_init(&ifrk, "lo0");
		ifrk.ifrk_func = RSS_FUNC_NONE;
		ifrk.ifrk_keylen = 0;
		if (ioctl(s, SIOCSIFRSSKEY, &ifrk) != -1 || errno != EPERM)
			_exit(RSS_PRIV_NOT_EPERM);

		rss_table_init(ifrt, "lo0");
		ifrt->ifrt_nentries = RSS_TABLELEN;
		if (ioctl(s, SIOCSIFRSSTABLE, ifrt) != -1 || errno != EPERM)
			_exit(RSS_PRIV_NOT_EPERM);

		_exit(RSS_PRIV_EPERM);
	}

	ATF_REQUIRE_EQ(child, waitpid(child, &status, 0));
	ATF_REQUIRE(WIFEXITED(status));
	ATF_CHECK_EQ(RSS_PRIV_EPERM, WEXITSTATUS(status));
}

/* With privilege the same requests reach the interface, which has no RSS. */
ATF_TC(rss_set_unsupported);
ATF_TC_HEAD(rss_set_unsupported, tc)
{

	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(rss_set_unsupported, tc)
{
	struct ifrsskey ifrk;
	struct ifrsstable *ifrt;
	int s;

	s = rss_socket();
	ifrt = malloc(sizeof(*ifrt));
	ATF_REQUIRE(ifrt != NULL);

	rss_key_init(&ifrk, "lo0");
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);
	rss_table_init(ifrt, "lo0");
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSTABLE, ifrt) == -1);

	free(ifrt);
	close(s);
}

/* An Ethernet interface without the methods answers from ether_ioctl(). */
ATF_TC(rss_ether_unsupported);
ATF_TC_HEAD(rss_ether_unsupported, tc)
{

	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.kmods", "if_epair");
}
ATF_TC_BODY(rss_ether_unsupported, tc)
{
	struct ifreq ifr;
	struct ifrsskey ifrk;
	struct ifrsshash ifrh;
	struct ifrsstable *ifrt;
	int s;

	s = rss_socket();
	ifrt = malloc(sizeof(*ifrt));
	ATF_REQUIRE(ifrt != NULL);

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_data = (caddr_t)-1;
	strlcpy(ifr.ifr_name, "epair", sizeof(ifr.ifr_name));
	ATF_REQUIRE_MSG(ioctl(s, SIOCIFCREATE2, &ifr) == 0,
	    "SIOCIFCREATE2: %s", strerror(errno));

	rss_key_init(&ifrk, ifr.ifr_name);
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSKEY, &ifrk) == -1);
	rss_hash_init(&ifrh, ifr.ifr_name);
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSHASH, &ifrh) == -1);
	rss_table_init(ifrt, ifr.ifr_name);
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCGIFRSSTABLE, ifrt) == -1);
	rss_key_init(&ifrk, ifr.ifr_name);
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);
	rss_table_init(ifrt, ifr.ifr_name);
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSTABLE, ifrt) == -1);

	ATF_CHECK_MSG(ioctl(s, SIOCIFDESTROY, &ifr) == 0,
	    "SIOCIFDESTROY: %s", strerror(errno));
	free(ifrt);
	close(s);
}

ATF_TC(rss_iflib);
ATF_TC_HEAD(rss_iflib, tc)
{

	atf_tc_set_md_var(tc, "descr", "Round trip through an iflib driver "
	    "named by the rss.ifname configuration variable");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "execenv", "host");
}
ATF_TC_BODY(rss_iflib, tc)
{
	struct ifrsskey ifrk, saved_key;
	struct ifrsshash ifrh;
	struct ifrsstable *ifrt, *saved;
	const char *ifname;
	uint16_t i;
	int s;

	ifname = atf_tc_get_config_var_wd(tc, "rss.ifname", "");
	if (ifname[0] == '\0')
		atf_tc_skip("rss.ifname is not set");

	s = rss_socket();
	ifrt = malloc(sizeof(*ifrt));
	saved = malloc(sizeof(*saved));
	ATF_REQUIRE(ifrt != NULL && saved != NULL);

	/* The queries must answer before anything is programmed. */
	rss_key_init(&saved_key, ifname);
	ATF_REQUIRE_MSG(ioctl(s, SIOCGIFRSSKEY, &saved_key) == 0,
	    "SIOCGIFRSSKEY: %s", strerror(errno));
	ATF_CHECK_EQ(RSS_FUNC_TOEPLITZ, saved_key.ifrk_func);
	ATF_CHECK(saved_key.ifrk_keylen > 0);
	ATF_CHECK(saved_key.ifrk_keylen <= sizeof(saved_key.ifrk_key));

	rss_hash_init(&ifrh, ifname);
	ATF_REQUIRE_MSG(ioctl(s, SIOCGIFRSSHASH, &ifrh) == 0,
	    "SIOCGIFRSSHASH: %s", strerror(errno));
	ATF_CHECK_EQ(RSS_FUNC_TOEPLITZ, ifrh.ifrh_func);
	ATF_CHECK(ifrh.ifrh_types != 0);

	rss_table_init(saved, ifname);
	ATF_REQUIRE_MSG(ioctl(s, SIOCGIFRSSTABLE, saved) == 0,
	    "SIOCGIFRSSTABLE: %s", strerror(errno));
	ATF_REQUIRE(saved->ifrt_nentries > 0);
	ATF_REQUIRE(saved->ifrt_nentries <= RSS_TABLELEN);
	ATF_REQUIRE(saved->ifrt_nqueues > 0);
	for (i = 0; i < saved->ifrt_nentries; i++)
		ATF_CHECK(saved->ifrt_table[i] < saved->ifrt_nqueues);

	/* A table of the wrong length or naming a missing queue is refused. */
	memcpy(ifrt, saved, sizeof(*ifrt));
	ifrt->ifrt_nentries = 0;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSTABLE, ifrt) == -1);

	memcpy(ifrt, saved, sizeof(*ifrt));
	ifrt->ifrt_nentries = saved->ifrt_nentries + 1;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSTABLE, ifrt) == -1);

	memcpy(ifrt, saved, sizeof(*ifrt));
	ifrt->ifrt_table[0] = saved->ifrt_nqueues;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSTABLE, ifrt) == -1);

	/* A key of the wrong function, length or reserved field is refused. */
	memcpy(&ifrk, &saved_key, sizeof(ifrk));
	ifrk.ifrk_func = RSS_FUNC_NONE;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);

	memcpy(&ifrk, &saved_key, sizeof(ifrk));
	ifrk.ifrk_keylen = 0;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);

	memcpy(&ifrk, &saved_key, sizeof(ifrk));
	ifrk.ifrk_keylen = sizeof(ifrk.ifrk_key) + 1;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);

	memcpy(&ifrk, &saved_key, sizeof(ifrk));
	ifrk.ifrk_spare0 = 1;
	ATF_CHECK_ERRNO(EINVAL, ioctl(s, SIOCSIFRSSKEY, &ifrk) == -1);

	/* Point every entry at the last queue, read it back, then restore. */
	memcpy(ifrt, saved, sizeof(*ifrt));
	for (i = 0; i < ifrt->ifrt_nentries; i++)
		ifrt->ifrt_table[i] = saved->ifrt_nqueues - 1;
	ATF_REQUIRE_MSG(ioctl(s, SIOCSIFRSSTABLE, ifrt) == 0,
	    "SIOCSIFRSSTABLE: %s", strerror(errno));

	/* Nothing may abort before the restore below. */
	rss_table_init(ifrt, ifname);
	if (ioctl(s, SIOCGIFRSSTABLE, ifrt) == 0) {
		ATF_CHECK_EQ(saved->ifrt_nentries, ifrt->ifrt_nentries);
		for (i = 0; i < ifrt->ifrt_nentries; i++)
			ATF_CHECK_EQ(saved->ifrt_nqueues - 1,
			    ifrt->ifrt_table[i]);
	} else
		atf_tc_fail_nonfatal("SIOCGIFRSSTABLE: %s", strerror(errno));

	memcpy(ifrt, saved, sizeof(*ifrt));
	ATF_REQUIRE_MSG(ioctl(s, SIOCSIFRSSTABLE, ifrt) == 0,
	    "restoring the table: %s", strerror(errno));

	/* Programming the reported key back changes nothing. */
	memcpy(&ifrk, &saved_key, sizeof(ifrk));
	ATF_REQUIRE_MSG(ioctl(s, SIOCSIFRSSKEY, &ifrk) == 0,
	    "SIOCSIFRSSKEY: %s", strerror(errno));
	rss_key_init(&ifrk, ifname);
	ATF_REQUIRE_MSG(ioctl(s, SIOCGIFRSSKEY, &ifrk) == 0,
	    "SIOCGIFRSSKEY: %s", strerror(errno));
	ATF_CHECK_EQ(saved_key.ifrk_keylen, ifrk.ifrk_keylen);
	ATF_CHECK(memcmp(saved_key.ifrk_key, ifrk.ifrk_key,
	    saved_key.ifrk_keylen) == 0);

	free(saved);
	free(ifrt);
	close(s);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, rss_layout);
	ATF_TP_ADD_TC(tp, rss_get_unsupported);
	ATF_TP_ADD_TC(tp, rss_set_unprivileged);
	ATF_TP_ADD_TC(tp, rss_set_unsupported);
	ATF_TP_ADD_TC(tp, rss_ether_unsupported);
	ATF_TP_ADD_TC(tp, rss_iflib);

	return (atf_no_error());
}

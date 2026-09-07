/* SPDX-License-Identifier: BSD-2-Clause */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_mib.h>
#include <net/rss_config.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool
scenario(const char *name)
{

	return (getenv("RSSMAP_TEST_CASE") != NULL &&
	    strcmp(getenv("RSSMAP_TEST_CASE"), name) == 0);
}

static unsigned int
queue_count(void)
{

	if (scenario("unused_queue"))
		return (3);
	if (scenario("queues10"))
		return (10);
	if (scenario("queues11"))
		return (11);
	if (scenario("queues12"))
		return (12);
	if (scenario("queues100"))
		return (100);
	if (scenario("queues101"))
		return (101);
	return (2);
}

static int
reply(const void *data, size_t len, void *oldp, size_t *oldlenp)
{

	if (oldlenp == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (oldp != NULL) {
		if (*oldlenp < len) {
			*oldlenp = len;
			errno = ENOMEM;
			return (-1);
		}
		memcpy(oldp, data, len);
	}
	*oldlenp = len;
	return (0);
}

static int
reply_int(int value, void *oldp, size_t *oldlenp)
{

	return (reply(&value, sizeof(value), oldp, oldlenp));
}

static int
reply_string(const char *value, void *oldp, size_t *oldlenp)
{

	return (reply(value, strlen(value) + 1, oldp, oldlenp));
}

static int
unavailable(void)
{

	errno = ENOENT;
	return (-1);
}

int
ioctl(int fd, unsigned long request, ...)
{
	static int (*next_ioctl)(int, unsigned long, ...);
	struct ifrsstable *table;
	struct ifrsshash *hash;
	struct ifrsskey *key;
	unsigned int i, count;
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (request == SIOCGIFRSSTABLE) {
		if (scenario("no_table")) {
			errno = EOPNOTSUPP;
			return (-1);
		}
		table = arg;
		if (strcmp(table->ifrt_name, "lo0") != 0)
			_exit(98);
		count = queue_count();
		table->ifrt_nentries = count > 16 ? 128 : count > 2 ? 16 : 4;
		if (scenario("unused_queue"))
			table->ifrt_nentries = 2;
		if (scenario("hardware_larger"))
			table->ifrt_nentries = 8;
		if (scenario("table_zero"))
			table->ifrt_nentries = 0;
		if (scenario("table_oversize"))
			table->ifrt_nentries = RSS_TABLELEN + 1;
		if (scenario("table_nonpower"))
			table->ifrt_nentries = 3;
		table->ifrt_nqueues = scenario("queues_zero") ? 0 : count;
		for (i = 0; i < MIN(table->ifrt_nentries, RSS_TABLELEN); i++)
			table->ifrt_table[i] = i % count;
		if (scenario("queue_out_of_range"))
			table->ifrt_table[1] = count;
		return (0);
	}
	if (request == SIOCGIFRSSHASH) {
		if (scenario("hash_unreadable"))
			return (unavailable());
		hash = arg;
		hash->ifrh_func = RSS_FUNC_TOEPLITZ;
		hash->ifrh_types = RSS_TYPE_IPV4 | RSS_TYPE_TCP_IPV4;
		return (0);
	}
	if (request == SIOCGIFRSSKEY) {
		if (scenario("key_unreadable")) {
			errno = EPERM;
			return (-1);
		}
		key = arg;
		key->ifrk_func = RSS_FUNC_TOEPLITZ;
		key->ifrk_keylen = RSS_KEYSIZE;
		memset(key->ifrk_key, 0x5a, RSS_KEYSIZE);
		return (0);
	}
	if ((request & IOC_DIRMASK) == IOC_IN)
		_exit(99);
	if (next_ioctl == NULL)
		next_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (next_ioctl == NULL)
		_exit(97);
	return (next_ioctl(fd, request, arg));
}

int
sysctl(const int *name, u_int namelen, void *oldp, size_t *oldlenp,
    const void *newp, size_t newlen)
{
	static int (*next_sysctl)(const int *, u_int, void *, size_t *,
	    const void *, size_t);

	if (newp != NULL || newlen != 0)
		_exit(99);
	if (namelen == 6 && name[0] == CTL_NET && name[1] == PF_LINK &&
	    name[2] == NETLINK_GENERIC && name[3] == IFMIB_IFDATA &&
	    name[5] == IFDATA_DRIVERNAME) {
		if (scenario("name_unreadable"))
			return (unavailable());
		return (reply_string("aq12", oldp, oldlenp));
	}
	if (next_sysctl == NULL)
		next_sysctl = dlsym(RTLD_NEXT, "sysctl");
	if (next_sysctl == NULL)
		_exit(97);
	return (next_sysctl(name, namelen, oldp, oldlenp, newp, newlen));
}

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    const void *newp, size_t newlen)
{
	static int (*next_sysctlbyname)(const char *, void *, size_t *,
	    const void *, size_t);
	static const char basic_map[] = "0:0 1:2 2:4 3:6";
	static const char larger_map[] = "0:0 1:2 2:4 3:6 4:8 5:10 6:12 7:14";
	unsigned char key[RSS_KEYSIZE];
	unsigned int count, i;
	char oid[128];

	if (newp != NULL || newlen != 0)
		_exit(99);
	if (strcmp(name, "kern.features.rss") == 0) {
		if (scenario("no_rss"))
			return (unavailable());
		if (scenario("feature_unreadable")) {
			errno = EPERM;
			return (-1);
		}
		return (reply_int(1, oldp, oldlenp));
	}
	if (strcmp(name, "net.inet.rss.buckets") == 0) {
		if (scenario("bucket_zero"))
			return (reply_int(0, oldp, oldlenp));
		if (scenario("bucket_nonpower"))
			return (reply_int(3, oldp, oldlenp));
		if (scenario("bucket_unreadable"))
			return (unavailable());
		return (reply_int(scenario("software_larger") ? 8 : 4,
		    oldp, oldlenp));
	}
	if (strcmp(name, "net.inet.rss.bucket_mapping") == 0) {
		if (scenario("cpu_ranges"))
			return (reply_string("3:8 1:8 0:3 2:2", oldp, oldlenp));
		if (scenario("map_unreadable"))
			return (unavailable());
		if (scenario("map_duplicate"))
			return (reply_string("0:0 1:2 1:4 3:6", oldp, oldlenp));
		if (scenario("map_missing"))
			return (reply_string("0:0 1:2 3:6", oldp, oldlenp));
		if (scenario("map_out_of_range"))
			return (reply_string("0:0 1:2 2:4 4:6", oldp, oldlenp));
		if (scenario("map_negative"))
			return (reply_string("0:0 1:-2 2:4 3:6", oldp, oldlenp));
		if (scenario("map_junk"))
			return (reply_string("0:0 1:2junk 2:4 3:6", oldp, oldlenp));
		return (reply_string(scenario("software_larger") ? larger_map :
		    basic_map, oldp, oldlenp));
	}
	if (strcmp(name, "net.inet.rss.hashalgo") == 0)
		return (reply_int(RSS_HASH_TOEPLITZ, oldp, oldlenp));
	if (strcmp(name, "net.inet.rss.key") == 0) {
		if (scenario("software_key_unreadable"))
			return (unavailable());
		memset(key, scenario("different_key") ? 0xa5 : 0x5a, sizeof(key));
		return (reply(key, sizeof(key), oldp, oldlenp));
	}
	if (scenario("netisr_unreadable") && strncmp(name, "net.isr.", 8) == 0)
		return (unavailable());
	if (strcmp(name, "net.isr.dispatch") == 0)
		return (reply_string(scenario("hybrid") ? "hybrid" :
		    scenario("deferred") ? "deferred" : "direct", oldp, oldlenp));
	if (strcmp(name, "net.isr.numthreads") == 0)
		return (reply_int(scenario("one_worker") ? 1 : 8, oldp, oldlenp));
	if (strcmp(name, "net.isr.bindthreads") == 0)
		return (reply_int(scenario("unbound") ? 0 : 1, oldp, oldlenp));
	count = queue_count();
	for (i = 0; i < count; i++) {
		snprintf(oid, sizeof(oid), "dev.aq.12.iflib.rxq%0*u.cpu",
		    count > 100 ? 3 : count > 10 ? 2 : 1, i);
		if (strcmp(name, oid) != 0)
			continue;
		if (scenario("cpu_unreadable") && i == 1)
			return (unavailable());
		return (reply_int(i * 2, oldp, oldlenp));
	}
	if (strncmp(name, "dev.", 4) == 0 ||
	    strncmp(name, "net.inet.rss.", 13) == 0)
		return (unavailable());
	if (next_sysctlbyname == NULL)
		next_sysctlbyname = dlsym(RTLD_NEXT, "sysctlbyname");
	if (next_sysctlbyname == NULL)
		_exit(97);
	return (next_sysctlbyname(name, oldp, oldlenp, newp, newlen));
}

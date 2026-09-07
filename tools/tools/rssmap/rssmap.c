/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_mib.h>
#include <net/rss_config.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct placement {
	struct ifrsstable table;
	int	*queue_cpus;
	int	bucket_cpus[RSS_TABLE_MAXLEN];
	u_int	buckets;
};

static bool
get_int(const char *name, int *value)
{
	size_t len;

	len = sizeof(*value);
	if (sysctlbyname(name, value, &len, NULL, 0) == -1)
		return (false);
	if (len != sizeof(*value)) {
		errno = EINVAL;
		return (false);
	}
	return (true);
}

static bool
get_string(const char *name, char *value, size_t size)
{
	size_t len;

	len = size;
	if (sysctlbyname(name, value, &len, NULL, 0) == -1)
		return (false);
	if (len == 0 || len > size || value[len - 1] != '\0') {
		errno = EINVAL;
		return (false);
	}
	return (true);
}

static void
get_queue_cpus(const char *ifname, struct placement *p)
{
	char device[128], name[256];
	size_t len, split;
	u_int index, q;
	int mib[6], cpu, width;

	for (q = 0; q < p->table.ifrt_nqueues; q++)
		p->queue_cpus[q] = -1;
	index = if_nametoindex(ifname);
	if (index == 0)
		return;
	mib[0] = CTL_NET;
	mib[1] = PF_LINK;
	mib[2] = NETLINK_GENERIC;
	mib[3] = IFMIB_IFDATA;
	mib[4] = index;
	mib[5] = IFDATA_DRIVERNAME;
	len = sizeof(device);
	if (sysctl(mib, nitems(mib), device, &len, NULL, 0) == -1 ||
	    len == 0 || len > sizeof(device) || device[len - 1] != '\0')
		return;
	split = strlen(device);
	while (split > 0 && isdigit((unsigned char)device[split - 1]))
		split--;
	if (split == 0 || device[split] == '\0')
		return;
	width = p->table.ifrt_nqueues > 100 ? 3 :
	    p->table.ifrt_nqueues > 10 ? 2 : 1;
	for (q = 0; q < p->table.ifrt_nqueues; q++) {
		snprintf(name, sizeof(name), "dev.%.*s.%s.iflib.rxq%0*u.cpu",
		    (int)split, device, device + split, width, q);
		if (get_int(name, &cpu) && cpu >= 0)
			p->queue_cpus[q] = cpu;
	}
}

static void
get_buckets(struct placement *p)
{
	char mapping[RSS_TABLE_MAXLEN * 24], *next, *token, *colon;
	const char *error;
	u_int bucket, count;
	int enabled, buckets, cpu;

	if (!get_int("kern.features.rss", &enabled)) {
		printf("software RSS: %s\n",
		    errno == ENOENT ? "disabled" : "unavailable");
		return;
	}
	if (enabled == 0) {
		printf("software RSS: disabled\n");
		return;
	}
	if (!get_int("net.inet.rss.buckets", &buckets)) {
		printf("software RSS: unavailable (%s)\n", strerror(errno));
		return;
	}
	if (buckets <= 0 || buckets > RSS_TABLE_MAXLEN || !powerof2(buckets))
		errx(1, "invalid RSS bucket count: %d", buckets);
	if (!get_string("net.inet.rss.bucket_mapping", mapping,
	    sizeof(mapping))) {
		printf("software RSS: unavailable (%s)\n", strerror(errno));
		return;
	}
	for (bucket = 0; bucket < (u_int)buckets; bucket++)
		p->bucket_cpus[bucket] = -1;
	next = mapping;
	count = 0;
	while ((token = strsep(&next, " ")) != NULL) {
		colon = strchr(token, ':');
		if (colon == NULL)
			errx(1, "invalid RSS bucket mapping");
		*colon++ = '\0';
		bucket = strtonum(token, 0, buckets - 1, &error);
		if (error != NULL || p->bucket_cpus[bucket] != -1)
			errx(1, "invalid RSS bucket mapping");
		cpu = strtonum(colon, 0, INT_MAX, &error);
		if (error != NULL)
			errx(1, "invalid RSS bucket mapping");
		p->bucket_cpus[bucket] = cpu;
		count++;
	}
	if (count != (u_int)buckets)
		errx(1, "invalid RSS bucket mapping");
	p->buckets = buckets;
	printf("software RSS: %u buckets\n", p->buckets);
}

static void
print_hash(int fd, const char *ifname)
{
	struct ifrsshash hash = { 0 };
	struct ifrsskey key = { 0 };
	uint8_t software_key[RSS_KEYLEN];
	const char *key_match, *func;
	size_t len;
	int algorithm;

	strlcpy(hash.ifrh_name, ifname, sizeof(hash.ifrh_name));
	strlcpy(key.ifrk_name, ifname, sizeof(key.ifrk_name));
	func = "unavailable";
	if (ioctl(fd, SIOCGIFRSSHASH, &hash) == 0) {
		switch (hash.ifrh_func) {
		case RSS_FUNC_NONE:
			func = "none";
			break;
		case RSS_FUNC_PRIVATE:
			func = "private";
			break;
		case RSS_FUNC_TOEPLITZ:
			func = "toeplitz";
			break;
		default:
			func = "unknown";
			break;
		}
		printf("hash: %s, types %#x", func, hash.ifrh_types);
	} else
		printf("hash: %s", func);
	if (get_int("net.inet.rss.hashalgo", &algorithm))
		printf("; software %s", algorithm == RSS_HASH_TOEPLITZ ?
		    "toeplitz" : algorithm == RSS_HASH_NAIVE ? "naive" :
		    "unknown");
	key_match = "unavailable";
	len = sizeof(software_key);
	if (ioctl(fd, SIOCGIFRSSKEY, &key) == 0 &&
	    key.ifrk_keylen > 0 && key.ifrk_keylen <= sizeof(key.ifrk_key) &&
	    sysctlbyname("net.inet.rss.key", software_key, &len, NULL, 0) == 0 &&
	    len > 0 && len <= sizeof(software_key)) {
		key_match = len == key.ifrk_keylen &&
		    memcmp(key.ifrk_key, software_key, len) == 0 ?
		    "same" : "different";
	}
	printf("; key comparison %s\n", key_match);
}

static void
print_netisr(void)
{
	char dispatch[32];
	int workers, bound;

	if (!get_string("net.isr.dispatch", dispatch, sizeof(dispatch)))
		strlcpy(dispatch, "unavailable", sizeof(dispatch));
	printf("netisr: %s", dispatch);
	if (get_int("net.isr.numthreads", &workers))
		printf(", %d worker%s", workers, workers == 1 ? "" : "s");
	if (get_int("net.isr.bindthreads", &bound))
		printf(", %s", bound ? "bound" : "unbound");
	printf("\n");
}

static const char *
cpu_string(int cpu, char *buf, size_t len)
{

	if (cpu < 0)
		return ("-");
	snprintf(buf, len, "%d", cpu);
	return (buf);
}

static void
print_buckets(const struct placement *p)
{
	char cpu[16];
	u_int slot, entry, queue, bucket, slots;
	int rx_cpu, rss_cpu;

	printf(" HASH ENTRY   RXQ RX-CPU");
	if (p->buckets != 0)
		printf(" RSS-BUCKET RSS-CPU PLACEMENT");
	printf("\n");
	slots = MAX(p->table.ifrt_nentries, p->buckets);
	for (slot = 0; slot < slots; slot++) {
		entry = slot & (p->table.ifrt_nentries - 1);
		queue = p->table.ifrt_table[entry];
		rx_cpu = p->queue_cpus[queue];
		printf("%5u %5u %5u %6s", slot, entry, queue,
		    cpu_string(rx_cpu, cpu, sizeof(cpu)));
		if (p->buckets != 0) {
			bucket = slot & (p->buckets - 1);
			rss_cpu = p->bucket_cpus[bucket];
			printf(" %10u %7d %s", bucket, rss_cpu,
			    rx_cpu < 0 ? "unknown" :
			    rx_cpu == rss_cpu ? "same" : "different");
		}
		printf("\n");
	}
}

static int
compare_cpus(const void *a, const void *b)
{
	int x, y;

	x = *(const int *)a;
	y = *(const int *)b;
	return ((x > y) - (x < y));
}

static void
print_cpus(int *cpus, u_int count)
{
	const char *sep;
	u_int i;
	int first, last;

	if (count == 0) {
		printf("-");
		return;
	}
	qsort(cpus, count, sizeof(*cpus), compare_cpus);
	sep = "";
	for (i = 0; i < count; i++) {
		first = last = cpus[i];
		while (i + 1 < count && (cpus[i + 1] == last ||
		    (last < INT_MAX && cpus[i + 1] == last + 1)))
			last = cpus[++i];
		printf("%s%d", sep, first);
		if (last != first)
			printf("-%d", last);
		sep = ",";
	}
}

static void
print_queues(const struct placement *p)
{
	int cpus[MAX(RSS_TABLELEN, RSS_TABLE_MAXLEN)];
	char cpu[16];
	u_int queue, entry, slot, slots, entries, count;
	u_int same, different, unknown;
	int rx_cpu, rss_cpu;

	printf("QUEUE RX-CPU ENTRIES");
	if (p->buckets != 0)
		printf("  SAME DIFFERENT UNKNOWN RSS-CPUS");
	printf("\n");
	slots = MAX(p->table.ifrt_nentries, p->buckets);
	for (queue = 0; queue < p->table.ifrt_nqueues; queue++) {
		entries = 0;
		for (entry = 0; entry < p->table.ifrt_nentries; entry++)
			if (p->table.ifrt_table[entry] == queue)
				entries++;
		rx_cpu = p->queue_cpus[queue];
		printf("%5u %6s %7u", queue,
		    cpu_string(rx_cpu, cpu, sizeof(cpu)), entries);
		if (p->buckets == 0) {
			printf("\n");
			continue;
		}
		count = same = different = unknown = 0;
		for (slot = 0; slot < slots; slot++) {
			entry = slot & (p->table.ifrt_nentries - 1);
			if (p->table.ifrt_table[entry] != queue)
				continue;
			rss_cpu = p->bucket_cpus[slot & (p->buckets - 1)];
			cpus[count++] = rss_cpu;
			if (rx_cpu < 0)
				unknown++;
			else if (rx_cpu == rss_cpu)
				same++;
			else
				different++;
		}
		printf(" %5u %9u %7u ", same, different, unknown);
		print_cpus(cpus, count);
		printf("\n");
	}
}

int
main(int argc, char **argv)
{
	struct placement p = { 0 };
	const char *ifname;
	u_int i;
	int ch, fd;
	bool detail;

	detail = false;
	while ((ch = getopt(argc, argv, "b")) != -1) {
		switch (ch) {
		case 'b':
			detail = true;
			break;
		default:
			errx(1, "usage: rssmap [-b] interface");
		}
	}
	if (argc - optind != 1)
		errx(1, "usage: rssmap [-b] interface");
	ifname = argv[optind];
	if (strlcpy(p.table.ifrt_name, ifname, sizeof(p.table.ifrt_name)) >=
	    sizeof(p.table.ifrt_name))
		errx(1, "interface name too long");
	fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (fd == -1)
		err(1, "socket");
	if (ioctl(fd, SIOCGIFRSSTABLE, &p.table) == -1)
		err(1, "%s: SIOCGIFRSSTABLE", ifname);
	if (p.table.ifrt_nentries == 0 ||
	    p.table.ifrt_nentries > nitems(p.table.ifrt_table) ||
	    !powerof2(p.table.ifrt_nentries) || p.table.ifrt_nqueues == 0)
		errx(1, "invalid RSS table");
	for (i = 0; i < p.table.ifrt_nentries; i++)
		if (p.table.ifrt_table[i] >= p.table.ifrt_nqueues)
			errx(1, "invalid RSS table");
	p.queue_cpus = calloc(p.table.ifrt_nqueues, sizeof(*p.queue_cpus));
	if (p.queue_cpus == NULL)
		err(1, "calloc");
	get_queue_cpus(ifname, &p);
	printf("%s: %u RSS table entries, %u RX queues\n", ifname,
	    p.table.ifrt_nentries, p.table.ifrt_nqueues);
	get_buckets(&p);
	print_hash(fd, ifname);
	print_netisr();
	printf("Configured CPU bindings for the same hash (- = unavailable).\n");
	if (detail)
		print_buckets(&p);
	else
		print_queues(&p);
	free(p.queue_cpus);
	close(fd);
	return (0);
}

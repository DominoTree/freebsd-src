#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>

#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifconfig.h"

static const struct {
	uint32_t	type;
	const char	*name;
} rss_type_names[] = {
	{ RSS_TYPE_IPV4,	"ipv4" },
	{ RSS_TYPE_TCP_IPV4,	"tcp4" },
	{ RSS_TYPE_UDP_IPV4,	"udp4" },
	{ RSS_TYPE_IPV6,	"ipv6" },
	{ RSS_TYPE_IPV6_EX,	"ipv6ex" },
	{ RSS_TYPE_TCP_IPV6,	"tcp6" },
	{ RSS_TYPE_TCP_IPV6_EX,	"tcp6ex" },
	{ RSS_TYPE_UDP_IPV6,	"udp6" },
	{ RSS_TYPE_UDP_IPV6_EX,	"udp6ex" },
};

static const char *
rss_func_name(uint8_t func)
{
	switch (func) {
	case RSS_FUNC_NONE:
		return ("none");
	case RSS_FUNC_PRIVATE:
		return ("private");
	case RSS_FUNC_TOEPLITZ:
		return ("toeplitz");
	default:
		return ("unknown");
	}
}

static void
rss_print_hash(if_ctx *ctx)
{
	struct ifrsshash ifrh = {};
	const char *sep;
	size_t i;

	strlcpy(ifrh.ifrh_name, ctx->ifname, sizeof(ifrh.ifrh_name));
	if (ioctl_ctx(ctx, SIOCGIFRSSHASH, (caddr_t)&ifrh) < 0)
		return;

	printf("\trss: %s", rss_func_name(ifrh.ifrh_func));
	sep = " types ";
	for (i = 0; i < nitems(rss_type_names); i++) {
		if ((ifrh.ifrh_types & rss_type_names[i].type) == 0)
			continue;
		printf("%s%s", sep, rss_type_names[i].name);
		sep = ",";
	}
	printf("\n");
}

static void
rss_print_key(if_ctx *ctx)
{
	struct ifrsskey ifrk = {};
	uint16_t i;

	strlcpy(ifrk.ifrk_name, ctx->ifname, sizeof(ifrk.ifrk_name));
	if (ioctl_ctx(ctx, SIOCGIFRSSKEY, (caddr_t)&ifrk) < 0)
		return;
	if (ifrk.ifrk_keylen > sizeof(ifrk.ifrk_key))
		return;

	printf("\trss key:");
	for (i = 0; i < ifrk.ifrk_keylen; i++) {
		if (i % 20 == 0)
			printf("\n\t  ");
		printf("%02x", ifrk.ifrk_key[i]);
	}
	printf("\n");
}

static void
rss_print_table(if_ctx *ctx)
{
	struct ifrsstable ifrt = {};
	uint16_t i;

	strlcpy(ifrt.ifrt_name, ctx->ifname, sizeof(ifrt.ifrt_name));
	if (ioctl_ctx(ctx, SIOCGIFRSSTABLE, (caddr_t)&ifrt) < 0)
		return;
	if (ifrt.ifrt_nentries > nitems(ifrt.ifrt_table))
		return;

	printf("\trss table: %u entries, %u queue%s", ifrt.ifrt_nentries,
	    ifrt.ifrt_nqueues, ifrt.ifrt_nqueues == 1 ? "" : "s");
	for (i = 0; i < ifrt.ifrt_nentries; i++) {
		if (i % 32 == 0)
			printf("\n\t  ");
		printf("%*u", ifrt.ifrt_nqueues > 10 ? 3 : 2,
		    ifrt.ifrt_table[i]);
	}
	printf("\n");
}

static void
rss_status(if_ctx *ctx)
{

	if (ctx->args->verbose == 0)
		return;

	rss_print_hash(ctx);
	rss_print_key(ctx);
	rss_print_table(ctx);
}

static u_int
rss_parse_queues(const char *spec, uint16_t *queues, u_int maxqueues,
    u_int nqueues)
{
	char *copy, *tok, *next;
	u_int count;

	copy = strdup(spec);
	if (copy == NULL)
		err(1, "strdup");

	count = 0;
	next = copy;
	while ((tok = strsep(&next, ",")) != NULL) {
		u_long first, last;
		char *ep;

		if (*tok == '\0')
			errx(1, "rsstable: empty queue in \"%s\"", spec);
		first = strtoul(tok, &ep, 10);
		if (ep == tok)
			errx(1, "rsstable: \"%s\" is not a queue number", tok);
		last = first;
		if (*ep == '-') {
			tok = ep + 1;
			last = strtoul(tok, &ep, 10);
			if (ep == tok)
				errx(1, "rsstable: bad range \"%s\"", spec);
		}
		if (*ep != '\0')
			errx(1, "rsstable: trailing junk in \"%s\"", tok);
		if (first > last)
			errx(1, "rsstable: range %lu-%lu is reversed", first,
			    last);
		if (last >= nqueues)
			errx(1, "rsstable: queue %lu out of range (0..%u)",
			    last, nqueues - 1);
		for (; first <= last; first++) {
			if (count >= maxqueues)
				errx(1, "rsstable: too many queues listed");
			queues[count++] = (uint16_t)first;
		}
	}
	free(copy);

	if (count == 0)
		errx(1, "rsstable: no queues given");
	return (count);
}

static void
setrsstable(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct ifrsstable ifrt = {};
	uint16_t queues[RSS_TABLELEN];
	u_int nqueues;
	uint16_t i;

	strlcpy(ifrt.ifrt_name, ctx->ifname, sizeof(ifrt.ifrt_name));
	if (ioctl_ctx(ctx, SIOCGIFRSSTABLE, (caddr_t)&ifrt) < 0)
		err(1, "ioctl (SIOCGIFRSSTABLE)");
	if (ifrt.ifrt_nqueues == 0)
		errx(1, "rsstable: %s has no receive queues", ctx->ifname);
	if (ifrt.ifrt_nentries > nitems(ifrt.ifrt_table))
		errx(1, "rsstable: kernel reported %u entries",
		    ifrt.ifrt_nentries);

	if (strcmp(val, "default") == 0) {
		nqueues = ifrt.ifrt_nqueues;
		if (nqueues > nitems(queues))
			nqueues = nitems(queues);
		for (i = 0; i < nqueues; i++)
			queues[i] = i;
	} else {
		nqueues = rss_parse_queues(val, queues, nitems(queues),
		    ifrt.ifrt_nqueues);
	}

	for (i = 0; i < ifrt.ifrt_nentries; i++)
		ifrt.ifrt_table[i] = queues[i % nqueues];

	if (ioctl_ctx(ctx, SIOCSIFRSSTABLE, (caddr_t)&ifrt) < 0)
		err(1, "ioctl (SIOCSIFRSSTABLE)");
}

static uint8_t
rss_hexval(char digit)
{

	if (!isxdigit((unsigned char)digit))
		errx(1, "rsskey: \"%c\" is not a hex digit", digit);
	return (digittoint((unsigned char)digit));
}

static void
setrsskey(if_ctx *ctx, const char *val, int dummy __unused)
{
	struct ifrsskey ifrk = {};
	size_t digits, i;

	digits = strlen(val);
	if (digits == 0 || (digits % 2) != 0)
		errx(1, "rsskey: expected an even number of hex digits");
	if (digits / 2 > sizeof(ifrk.ifrk_key))
		errx(1, "rsskey: key exceeds %zu bytes",
		    sizeof(ifrk.ifrk_key));

	for (i = 0; i < digits / 2; i++) {
		ifrk.ifrk_key[i] = (uint8_t)((rss_hexval(val[i * 2]) << 4) |
		    rss_hexval(val[i * 2 + 1]));
	}
	ifrk.ifrk_func = RSS_FUNC_TOEPLITZ;
	ifrk.ifrk_keylen = (uint16_t)(digits / 2);

	strlcpy(ifrk.ifrk_name, ctx->ifname, sizeof(ifrk.ifrk_name));
	if (ioctl_ctx(ctx, SIOCSIFRSSKEY, (caddr_t)&ifrk) < 0)
		err(1, "ioctl (SIOCSIFRSSKEY)");
}

static struct cmd rss_cmds[] = {
	DEF_CMD_ARG("rsstable", setrsstable),
	DEF_CMD_ARG("rsskey", setrsskey),
};

static struct afswtch af_rss = {
	.af_name	= "af_rss",
	.af_af		= AF_UNSPEC,
	.af_other_status = rss_status,
};

static __constructor void
rss_ctor(void)
{
	size_t i;

	for (i = 0; i < nitems(rss_cmds);  i++)
		cmd_register(&rss_cmds[i]);
	af_register(&af_rss);
}

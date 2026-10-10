/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>
 */

#include <sys/param.h>

#include <net/if.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#include "ifconfig.h"

static void
linkdiag_distance(uint32_t cm)
{
	printf("%" PRIu32, cm / 100);
	if (cm % 100 != 0) {
		if (cm % 10 == 0)
			printf(".%" PRIu32, cm % 100 / 10);
		else
			printf(".%02" PRIu32, cm % 100);
	}
	printf(" m");
}

static void
linkdiag_print(const struct iflinkdiag *diag, const char *indent)
{
	const struct iflinkdiag_pair *pair;
	uint32_t i;

	for (i = 0; i < diag->ifld_npairs; i++) {
		pair = &diag->ifld_pair[i];
		printf("%spair %c: %s", indent, 'A' + i,
		    ifconfig_linkdiag_result_string(pair->ldp_result));
		if ((pair->ldp_result == IFLD_R_CROSSED ||
			pair->ldp_result == IFLD_R_CROSSSHORT) &&
		    pair->ldp_peer < diag->ifld_npairs && pair->ldp_peer != i)
			printf(" with pair %c", 'A' + pair->ldp_peer);
		if ((pair->ldp_flags & IFLD_PF_DISTANCE) != 0) {
			printf(" at ");
			linkdiag_distance(pair->ldp_distance);
		}
		if ((pair->ldp_flags & IFLD_PF_FAR_DISTANCE) != 0) {
			printf(" (far end ");
			linkdiag_distance(pair->ldp_far_distance);
			printf(")");
		}
		printf("\n");
	}
}

static void
linkdiag_status(if_ctx *ctx)
{
	struct iflinkdiag diag;

	if (ctx->args->verbose == 0)
		return;
	if (ifconfig_linkdiag_get(lifh, ctx->ifname, &diag) != 0 ||
	    (diag.ifld_flags & IFLD_F_VALID) == 0 || diag.ifld_npairs == 0)
		return;
	printf("\tlinkdiag:\n");
	linkdiag_print(&diag, "\t  ");
}

static void
setlinkdiag(if_ctx *ctx, const char *cmd __unused, int arg __unused)
{
	struct iflinkdiag diag;
	bool module;
	int error;

	module = sfp_linkdiag(ctx);
	if (ifconfig_linkdiag_get(lifh, ctx->ifname, &diag) != 0) {
		error = ifconfig_err_errno(lifh);
		if (module &&
		    (error == EOPNOTSUPP || error == ENOTTY ||
			(error == EINVAL &&
			    ifconfig_err_errtype(lifh) == IOCTL)))
			return;
		errc(1, error, "%s: linkdiag", ctx->ifname);
	}
	if ((diag.ifld_flags & IFLD_F_DISRUPTIVE) != 0)
		warnx("%s: linkdiag interrupts traffic while the test runs",
		    ctx->ifname);
	if (ifconfig_linkdiag_run(lifh, ctx->ifname, &diag) != 0)
		errc(1, ifconfig_err_errno(lifh), "%s: linkdiag", ctx->ifname);
	if ((diag.ifld_flags & (IFLD_F_VALID | IFLD_F_RUNNING)) !=
		IFLD_F_VALID ||
	    diag.ifld_npairs == 0)
		errx(1, "%s: linkdiag: no test result", ctx->ifname);
	linkdiag_print(&diag, "");
}

static struct cmd linkdiag_cmds[] = {
	DEF_CMD("linkdiag", 0, setlinkdiag),
};

static struct afswtch af_linkdiag = {
	.af_name = "af_linkdiag",
	.af_af = AF_UNSPEC,
	.af_other_status = linkdiag_status,
};

static __constructor void
linkdiag_ctor(void)
{
	cmd_register(&linkdiag_cmds[0]);
	af_register(&af_linkdiag);
}

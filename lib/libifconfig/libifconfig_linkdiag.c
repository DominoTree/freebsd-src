/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>
 */

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>

#include <errno.h>
#include <string.h>

#include "libifconfig.h"
#include "libifconfig_internal.h"

static int
linkdiag_request(ifconfig_handle_t *h, const char *name,
    struct iflinkdiag *diag, unsigned long request)
{
	ifconfig_error_clear(h);
	if (name == NULL || diag == NULL || strlen(name) >= IFNAMSIZ) {
		ifconfig_error(h, OTHER, EINVAL);
		return (-1);
	}
	memset(diag, 0, sizeof(*diag));
	strlcpy(diag->ifld_name, name, sizeof(diag->ifld_name));
	diag->ifld_version = IFLD_VERSION;
	if (ifconfig_ioctlwrap(h, AF_LOCAL, request, diag) != 0)
		return (-1);
	if (diag->ifld_version != IFLD_VERSION ||
	    diag->ifld_npairs > IFLD_MAXPAIRS ||
	    ((diag->ifld_flags & IFLD_F_VALID) != 0 &&
		diag->ifld_npairs == 0)) {
		ifconfig_error(h, OTHER, EPROTO);
		return (-1);
	}
	return (0);
}

int
ifconfig_linkdiag_get(ifconfig_handle_t *h, const char *name,
    struct iflinkdiag *diag)
{
	return (linkdiag_request(h, name, diag, SIOCGIFLINKDIAG));
}

int
ifconfig_linkdiag_run(ifconfig_handle_t *h, const char *name,
    struct iflinkdiag *diag)
{
	return (linkdiag_request(h, name, diag, SIOCSIFLINKDIAG));
}

const char *
ifconfig_linkdiag_result_string(uint8_t result)
{
	switch (result) {
	case IFLD_R_OK:
		return ("OK");
	case IFLD_R_OPEN:
		return ("open");
	case IFLD_R_SHORT:
		return ("short");
	case IFLD_R_CROSSED:
		return ("crossed");
	case IFLD_R_LOWMISMATCH:
		return ("low impedance mismatch");
	case IFLD_R_HIGHMISMATCH:
		return ("high impedance mismatch");
	case IFLD_R_BUSY:
		return ("busy");
	case IFLD_R_FAILED:
		return ("failed");
	case IFLD_R_CROSSSHORT:
		return ("cross-pair short");
	default:
		return ("unknown");
	}
}

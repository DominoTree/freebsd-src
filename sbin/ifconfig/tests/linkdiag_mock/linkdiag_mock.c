/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price <nprice@FreeBSD.org>
 */

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/sff8472.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
mock_i2c(struct ifreq *ifr, const char *mode)
{
	struct ifi2creq *req;
	uint8_t base[256] = {}, diag[256] = {};
	const uint8_t *data;

	if (strcmp(mode, "sfp") != 0 && strcmp(mode, "sfp-legacy") != 0 &&
	    strcmp(mode, "empty-sfp") != 0) {
		errno = EIO;
		return (-1);
	}
	req = (struct ifi2creq *)ifr->ifr_data;
	if (strcmp(mode, "empty-sfp") == 0) {
		memset(req->data, 0, sizeof(req->data));
		return (0);
	}
	if (req->offset == 0 && req->len > 1)
		_exit(93);
	base[SFF_8472_ID] = 3;
	base[SFF_8472_CONNECTOR] = 7;
	base[SFF_8472_TRANS_START] = 0x10;
	base[SFF_8472_DIAG_TYPE] = SFF_8472_DDM_DONE | SFF_8472_DDM_INTERNAL;
	memcpy(base + SFF_8472_VENDOR_START, "Test optics     ", 16);
	memcpy(base + SFF_8472_PN_START, "Optical module  ", 16);
	memcpy(base + SFF_8472_SN_START, "12345678        ", 16);
	memcpy(base + SFF_8472_DATE_START, "260811  ", 8);
	diag[SFF_8472_TEMP] = 0x19;
	diag[SFF_8472_VCC] = 0x80;
	diag[SFF_8472_VCC + 1] = 0xe8;
	diag[SFF_8472_TX_BIAS] = 0x09;
	diag[SFF_8472_TX_BIAS + 1] = 0xc4;
	diag[SFF_8472_RX_POWER] = 0x27;
	diag[SFF_8472_RX_POWER + 1] = 0x10;
	data = req->dev_addr == SFF_8472_BASE ? base : diag;
	if (req->len > sizeof(req->data) ||
	    req->offset + req->len > sizeof(base))
		_exit(94);
	memcpy(req->data, data + req->offset, req->len);
	return (0);
}

static int
mock_linkdiag(unsigned long request, struct iflinkdiag *diag, const char *mode)
{
	struct iflinkdiag expected = {};
	uint32_t i;

	strlcpy(expected.ifld_name, "lo0", sizeof(expected.ifld_name));
	expected.ifld_version = IFLD_VERSION;
	if (memcmp(diag, &expected, sizeof(expected)) != 0)
		_exit(90);
	if (request == SIOCSIFLINKDIAG &&
	    (strcmp(mode, "cached") == 0 || strcmp(mode, "uncached") == 0 ||
		strcmp(mode, "sfp") == 0 || strcmp(mode, "sfp-legacy") == 0 ||
		strcmp(mode, "unsupported") == 0))
		_exit(91);
	if (strcmp(mode, "unsupported") == 0 || strcmp(mode, "sfp") == 0 ||
	    strcmp(mode, "sfp-legacy") == 0) {
		errno = strcmp(mode, "sfp-legacy") == 0 ? EINVAL : EOPNOTSUPP;
		return (-1);
	}
	if (strcmp(mode, "denied") == 0 && request == SIOCSIFLINKDIAG) {
		errno = EPERM;
		return (-1);
	}
	diag->ifld_flags = IFLD_F_VALID;
	diag->ifld_npairs = 4;
	for (i = 0; i < 4; i++) {
		diag->ifld_pair[i].ldp_result = IFLD_R_OPEN;
		diag->ifld_pair[i].ldp_flags = IFLD_PF_DISTANCE |
		    IFLD_PF_FAR_DISTANCE;
		diag->ifld_pair[i].ldp_peer = IFLD_PEER_NONE;
		diag->ifld_pair[i].ldp_distance = 200;
		diag->ifld_pair[i].ldp_far_distance = 200;
	}
	diag->ifld_pair[2].ldp_result = IFLD_R_SHORT;
	diag->ifld_pair[2].ldp_distance = 100;
	diag->ifld_pair[3].ldp_result = IFLD_R_CROSSED;
	diag->ifld_pair[3].ldp_distance = 100;
	if (strcmp(mode, "fractional") == 0) {
		diag->ifld_pair[0].ldp_distance = 205;
		diag->ifld_pair[1].ldp_distance = 210;
		diag->ifld_pair[2].ldp_distance = 0;
		diag->ifld_pair[3].ldp_distance = UINT32_MAX;
	} else if (strcmp(mode, "statuses") == 0) {
		diag->ifld_npairs = IFLD_MAXPAIRS;
		memset(diag->ifld_pair, 0, sizeof(diag->ifld_pair));
		diag->ifld_pair[0].ldp_result = IFLD_R_OK;
		diag->ifld_pair[1].ldp_result = 255;
		diag->ifld_pair[2].ldp_result = IFLD_R_LOWMISMATCH;
		diag->ifld_pair[3].ldp_result = IFLD_R_HIGHMISMATCH;
	} else if (strcmp(mode, "peer-statuses") == 0) {
		memset(diag->ifld_pair, 0, sizeof(diag->ifld_pair));
		diag->ifld_pair[0].ldp_result = IFLD_R_BUSY;
		diag->ifld_pair[1].ldp_result = IFLD_R_FAILED;
		diag->ifld_pair[2].ldp_result = IFLD_R_CROSSSHORT;
		diag->ifld_pair[2].ldp_peer = 0;
		diag->ifld_pair[3].ldp_result = IFLD_R_CROSSED;
		diag->ifld_pair[3].ldp_peer = IFLD_MAXPAIRS;
	} else if (strcmp(mode, "bad-version") == 0 ||
	    (strcmp(mode, "bad-run-version") == 0 &&
		request == SIOCSIFLINKDIAG)) {
		diag->ifld_version++;
	} else if (strcmp(mode, "bad-count") == 0 ||
	    (strcmp(mode, "bad-run-count") == 0 &&
		request == SIOCSIFLINKDIAG)) {
		diag->ifld_npairs = IFLD_MAXPAIRS + 1;
	} else if (strcmp(mode, "zero-count") == 0 ||
	    (strcmp(mode, "zero-run-count") == 0 &&
		request == SIOCSIFLINKDIAG)) {
		diag->ifld_npairs = 0;
	} else if (strcmp(mode, "disruptive") == 0) {
		diag->ifld_flags |= IFLD_F_DISRUPTIVE;
	} else if (strcmp(mode, "uncached") == 0 ||
	    strcmp(mode, "incomplete") == 0) {
		diag->ifld_flags &= ~IFLD_F_VALID;
	}
	return (0);
}

int
ioctl(int fd, unsigned long request, ...)
{
	int (*real_ioctl)(int, unsigned long, ...);
	const char *mode;
	va_list ap;
	void *data;

	va_start(ap, request);
	data = va_arg(ap, void *);
	va_end(ap);
	mode = getenv("LINKDIAG_MOCK_MODE");
	if (mode != NULL) {
		if (request == SIOCGI2C)
			return (mock_i2c(data, mode));
		if (request == SIOCGIFLINKDIAG || request == SIOCSIFLINKDIAG)
			return (mock_linkdiag(request, data, mode));
	}
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (real_ioctl == NULL)
		_exit(92);
	return (real_ioctl(fd, request, data));
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Per-CPU precomputed L2 rewrite ("adjacency") cache.  A hit lets the output
 * node rewrite the egress ethernet header with a single memcpy and call
 * if_transmit() directly, avoiding ether_output()/arpresolve() per packet.
 * The cache reads an already-resolved lle; unresolved neighbours fall to the
 * output node's slow path (which resolves and queues).  A global generation,
 * bumped on any lle or ifnet-departure event, lazily invalidates entries.
 */

#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/if_llatbl.h>
#include <net/route.h>
#include <net/route/nhop.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#ifdef INET6
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#endif

#include <net/vpp/vpp.h>

static volatile u_int vpp_adj_gen = 1;
static eventhandler_tag vpp_adj_lle_tag;
static eventhandler_tag vpp_adj_ifdep_tag;

static void
vpp_adj_bump(void)
{

	atomic_add_int(&vpp_adj_gen, 1);
}

static void
vpp_adj_lle_event(void *arg __unused, struct llentry *lle __unused,
    int evt __unused)
{

	vpp_adj_bump();
}

static void
vpp_adj_ifdep_event(void *arg __unused, if_t ifp __unused)
{

	vpp_adj_bump();
}

struct vpp_adj *
vpp_adj_get(struct vpp_runtime *rt, struct nhop_object *nh, uint32_t dest)
{
	struct sockaddr_in sin;
	struct vpp_adj *a;
	struct llentry *la;
	uint32_t idx, gen;

	if (__predict_false(if_gettype(nh->nh_ifp) != IFT_ETHER))
		return (NULL);

	idx = nhop_get_idx(nh);
	gen = vpp_adj_gen;
	a = &rt->adj_cache[idx & VPP_ADJ_MASK];
	if (a->valid && a->nh_idx == idx && a->gen == gen && a->af == AF_INET)
		return (a);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_len = sizeof(sin);
	sin.sin_addr.s_addr = (nh->nh_flags & NHF_GATEWAY) ?
	    nh->gw4_sa.sin_addr.s_addr : dest;

	la = lla_lookup(LLTABLE(nh->nh_ifp), LLE_UNLOCKED,
	    (struct sockaddr *)&sin);
	if (la == NULL || (la->r_flags & RLLE_VALID) == 0 ||
	    la->r_hdrlen != ETHER_HDR_LEN)
		return (NULL);		/* slow path will resolve and queue */

	memcpy(a->eh, la->r_linkdata, ETHER_HDR_LEN);
	a->txifp = nh->nh_ifp;
	a->nh_idx = idx;
	a->gen = gen;
	a->af = AF_INET;
	a->valid = 1;
	return (a);
}

#ifdef INET6
struct vpp_adj *
vpp_adj_get6(struct vpp_runtime *rt, struct nhop_object *nh,
    const struct in6_addr *dst)
{
	struct vpp_adj *a;
	struct llentry *la;
	const struct in6_addr *nh6;
	uint32_t idx, gen;

	if (__predict_false(if_gettype(nh->nh_ifp) != IFT_ETHER))
		return (NULL);

	idx = nhop_get_idx(nh);
	gen = vpp_adj_gen;
	a = &rt->adj_cache[idx & VPP_ADJ_MASK];
	if (a->valid && a->nh_idx == idx && a->gen == gen && a->af == AF_INET6)
		return (a);

	nh6 = (nh->nh_flags & NHF_GATEWAY) ? &nh->gw6_sa.sin6_addr : dst;
	la = nd6_lookup(nh6, LLE_SF(AF_INET6, LLE_UNLOCKED), nh->nh_ifp);
	if (la == NULL || (la->r_flags & RLLE_VALID) == 0 ||
	    la->r_hdrlen != ETHER_HDR_LEN)
		return (NULL);		/* slow path will resolve and queue */

	memcpy(a->eh, la->r_linkdata, ETHER_HDR_LEN);
	a->txifp = nh->nh_ifp;
	a->nh_idx = idx;
	a->gen = gen;
	a->af = AF_INET6;
	a->valid = 1;
	return (a);
}
#endif /* INET6 */

void
vpp_adj_init(void)
{

	vpp_adj_lle_tag = EVENTHANDLER_REGISTER(lle_event, vpp_adj_lle_event,
	    NULL, EVENTHANDLER_PRI_ANY);
	vpp_adj_ifdep_tag = EVENTHANDLER_REGISTER(ifnet_departure_event,
	    vpp_adj_ifdep_event, NULL, EVENTHANDLER_PRI_ANY);
}

void
vpp_adj_fini(void)
{

	EVENTHANDLER_DEREGISTER(lle_event, vpp_adj_lle_tag);
	EVENTHANDLER_DEREGISTER(ifnet_departure_event, vpp_adj_ifdep_tag);
}

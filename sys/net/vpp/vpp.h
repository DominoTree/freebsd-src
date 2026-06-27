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

#ifndef _NET_VPP_VPP_H_
#define	_NET_VPP_VPP_H_

#ifdef _KERNEL

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/counter.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>

struct mbuf;
struct nhop_object;
struct in6_addr;

/* Vector width: one frame carries up to this many packets per node. */
#define	VPP_FRAME_SIZE	256

/*
 * Precomputed L2 rewrite ("adjacency") cache.  Per-CPU, direct-mapped by the
 * kernel nexthop index; an entry caches the egress ethernet header so the
 * output node can rewrite L2 with a single memcpy and if_transmit() directly,
 * instead of paying ether_output()/arpresolve() per packet.
 */
#define	VPP_ADJ_SIZE	1024u
#define	VPP_ADJ_MASK	(VPP_ADJ_SIZE - 1)

struct vpp_adj {
	uint32_t	nh_idx;			/* key: kernel nexthop index */
	uint32_t	gen;			/* generation when filled */
	if_t		txifp;			/* egress interface */
	uint8_t		valid;
	uint8_t		af;			/* key: address family */
	uint8_t		eh[ETHER_HDR_LEN];	/* precomputed egress L2 header */
};

/* Graph nodes, in topological (dispatch) order. */
enum {
	VPP_NODE_ETH_INPUT = 0,
	VPP_NODE_IP4_INPUT,
	VPP_NODE_IP4_LOOKUP,
	VPP_NODE_IP4_REWRITE,
	VPP_NODE_IP6_INPUT,
	VPP_NODE_IP6_LOOKUP,
	VPP_NODE_IP6_REWRITE,
	VPP_NODE_OUTPUT,
	VPP_NODE_PUNT,
	VPP_NODE_DROP,
	VPP_N_NODES
};

/* Per-error counters. */
enum {
	VPP_ERR_NONE = 0,
	VPP_ERR_NOT_IP4,
	VPP_ERR_IP4_HDR,
	VPP_ERR_IP4_OPTIONS,
	VPP_ERR_IP4_LENGTH,
	VPP_ERR_IP4_CSUM,
	VPP_ERR_TTL,
	VPP_ERR_LOCAL,
	VPP_ERR_NO_ROUTE,
	VPP_ERR_MTU,
	VPP_ERR_TX,
	VPP_N_ERRORS
};

/* Forwarding-path stats (which output path packets took). */
enum {
	VPP_STAT_FAST = 0,	/* adjacency fast path: cached L2 + if_transmit */
	VPP_STAT_SLOW,		/* slow path: if_output (resolve/queue) */
	VPP_N_STATS
};

struct vpp_pktmeta {
	struct nhop_object	*nh;	/* epoch-stable, this dispatch only */
	uint32_t		dest;	/* dst IPv4, network order */
	uint32_t		fib;
	uint16_t		l3_off;	/* IP header offset from m_data */
	uint16_t		error;
	uint8_t			is_v6;	/* IPv6 packet */
};

struct vpp_frame {
	uint32_t		n;
	struct mbuf		*bufs[VPP_FRAME_SIZE];
	struct vpp_pktmeta	meta[VPP_FRAME_SIZE];
};

struct vpp_runtime {
	struct mtx		lock;
	if_t			ifp;
	struct mbuf		*punt_head;
	struct mbuf		**punt_tail;
	struct vpp_frame	frames[VPP_N_NODES];
	struct vpp_adj		adj_cache[VPP_ADJ_SIZE];
};

typedef void vpp_node_fn_t(struct vpp_runtime *, struct vpp_frame *);

extern vpp_node_fn_t *const vpp_node_fns[VPP_N_NODES];
extern const char *const vpp_node_names[VPP_N_NODES];
extern const char *const vpp_err_names[VPP_N_ERRORS];
extern counter_u64_t vpp_node_pkts[VPP_N_NODES];
extern counter_u64_t vpp_err_cnt[VPP_N_ERRORS];
extern counter_u64_t vpp_stat[VPP_N_STATS];
extern const char *const vpp_stat_names[VPP_N_STATS];

/*
 * Prefetch the mbuf struct (far) and packet data (near) of upcoming vector
 * slots, so a node's per-packet work runs against warm cache lines.  Expanded
 * in the node sources (which have the full struct mbuf).
 */
#define	VPP_PREFETCH(f, i)	do {					\
	if ((i) + 8 < (f)->n)						\
		__builtin_prefetch((f)->bufs[(i) + 8], 0, 3);		\
	if ((i) + 4 < (f)->n)						\
		__builtin_prefetch((f)->bufs[(i) + 4]->m_data, 0, 3);	\
} while (0)

vpp_node_fn_t vpp_eth_input_node, vpp_ip4_input_node, vpp_ip4_lookup_node,
    vpp_ip4_rewrite_node, vpp_ip6_input_node, vpp_ip6_lookup_node,
    vpp_ip6_rewrite_node, vpp_output_node, vpp_punt_node, vpp_drop_node;

struct mbuf *vpp_engine_input(if_t, struct mbuf *);

int vpp_if_enable(const char *);
int vpp_if_disable(const char *);
void vpp_if_init(void);
void vpp_if_uninit(void);

/* Adjacency (L2 rewrite) cache (vpp_adj.c). */
struct vpp_adj *vpp_adj_get(struct vpp_runtime *, struct nhop_object *,
    uint32_t dest);
struct vpp_adj *vpp_adj_get6(struct vpp_runtime *, struct nhop_object *,
    const struct in6_addr *dst);
void vpp_adj_init(void);
void vpp_adj_fini(void);

MALLOC_DECLARE(M_VPP);

/* Append one packet (with its metadata) to a downstream node's frame. */
static __inline void
vpp_enq(struct vpp_runtime *rt, u_int dst, struct mbuf *m,
    const struct vpp_pktmeta *md)
{
	struct vpp_frame *df = &rt->frames[dst];
	uint32_t i = df->n;

	KASSERT(i < VPP_FRAME_SIZE, ("vpp: node %u frame overflow", dst));
	df->bufs[i] = m;
	df->meta[i] = *md;
	df->n = i + 1;
}

#endif /* _KERNEL */
#endif /* _NET_VPP_VPP_H_ */

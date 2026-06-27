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
 * VPP-style vectorized in-kernel IPv4 forwarding engine.  Runtime, graph
 * dispatcher, counters and control sysctls.
 */

#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/counter.h>
#include <sys/epoch.h>
#include <sys/pcpu.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>

#include <net/vpp/vpp.h>

MALLOC_DEFINE(M_VPP, "vpp", "VPP vectorized forwarding engine");

static struct vpp_runtime **vpp_rt;	/* indexed by cpuid */

vpp_node_fn_t *const vpp_node_fns[VPP_N_NODES] = {
	[VPP_NODE_ETH_INPUT] = vpp_eth_input_node,
	[VPP_NODE_IP4_INPUT] = vpp_ip4_input_node,
	[VPP_NODE_IP4_LOOKUP] = vpp_ip4_lookup_node,
	[VPP_NODE_IP4_REWRITE] = vpp_ip4_rewrite_node,
#ifdef INET6
	[VPP_NODE_IP6_INPUT] = vpp_ip6_input_node,
	[VPP_NODE_IP6_LOOKUP] = vpp_ip6_lookup_node,
	[VPP_NODE_IP6_REWRITE] = vpp_ip6_rewrite_node,
#endif
	[VPP_NODE_OUTPUT] = vpp_output_node,
	[VPP_NODE_PUNT] = vpp_punt_node,
	[VPP_NODE_DROP] = vpp_drop_node,
};

const char *const vpp_node_names[VPP_N_NODES] = {
	[VPP_NODE_ETH_INPUT] = "eth_input",
	[VPP_NODE_IP4_INPUT] = "ip4_input",
	[VPP_NODE_IP4_LOOKUP] = "ip4_lookup",
	[VPP_NODE_IP4_REWRITE] = "ip4_rewrite",
#ifdef INET6
	[VPP_NODE_IP6_INPUT] = "ip6_input",
	[VPP_NODE_IP6_LOOKUP] = "ip6_lookup",
	[VPP_NODE_IP6_REWRITE] = "ip6_rewrite",
#endif
	[VPP_NODE_OUTPUT] = "output",
	[VPP_NODE_PUNT] = "punt",
	[VPP_NODE_DROP] = "drop",
};

const char *const vpp_err_names[VPP_N_ERRORS] = {
	[VPP_ERR_NONE] = "none",
	[VPP_ERR_NOT_IP4] = "not_ip4",
	[VPP_ERR_IP4_HDR] = "ip4_hdr",
	[VPP_ERR_IP4_OPTIONS] = "ip4_options",
	[VPP_ERR_IP4_LENGTH] = "ip4_length",
	[VPP_ERR_IP4_CSUM] = "ip4_csum",
	[VPP_ERR_TTL] = "ttl_expired",
	[VPP_ERR_LOCAL] = "local",
	[VPP_ERR_NO_ROUTE] = "no_route",
	[VPP_ERR_MTU] = "mtu",
	[VPP_ERR_TX] = "tx_error",
};

counter_u64_t vpp_node_pkts[VPP_N_NODES];
counter_u64_t vpp_err_cnt[VPP_N_ERRORS];
counter_u64_t vpp_stat[VPP_N_STATS];

const char *const vpp_stat_names[VPP_N_STATS] = {
	[VPP_STAT_FAST] = "fast",
	[VPP_STAT_SLOW] = "slow",
};

static struct sysctl_ctx_list vpp_clist;

static void
vpp_dispatch(struct vpp_runtime *rt)
{
	struct vpp_frame *f;
	u_int ni;

	/*
	 * Nodes are numbered in topological order and every edge points to a
	 * higher-numbered node, so a single ascending pass runs the graph to
	 * completion.  Each node drains its own input frame.
	 */
	for (ni = 0; ni < VPP_N_NODES; ni++) {
		f = &rt->frames[ni];
		if (f->n == 0)
			continue;
		counter_u64_add(vpp_node_pkts[ni], f->n);
		vpp_node_fns[ni](rt, f);
	}
}

struct mbuf *
vpp_engine_input(if_t ifp, struct mbuf *mh)
{
	struct vpp_runtime *rt;
	struct vpp_frame *f;
	struct mbuf *m, *punt;
	uint32_t i;

	NET_EPOCH_ASSERT();
	rt = vpp_rt[curcpu];
	mtx_lock(&rt->lock);
	rt->ifp = ifp;
	rt->punt_head = NULL;
	rt->punt_tail = &rt->punt_head;

	while (mh != NULL) {
		f = &rt->frames[VPP_NODE_ETH_INPUT];
		f->n = 0;
		while (mh != NULL && f->n < VPP_FRAME_SIZE) {
			m = mh;
			mh = m->m_nextpkt;
			m->m_nextpkt = NULL;
			i = f->n++;
			f->bufs[i] = m;
			f->meta[i].nh = NULL;
			f->meta[i].dest = 0;
			f->meta[i].fib = M_GETFIB(m);
			f->meta[i].l3_off = 0;
			f->meta[i].error = VPP_ERR_NONE;
		}
		vpp_dispatch(rt);
	}

	punt = rt->punt_head;
	rt->ifp = NULL;
	mtx_unlock(&rt->lock);
	return (punt);
}

static int
vpp_sysctl_enable(SYSCTL_HANDLER_ARGS)
{
	char name[IFNAMSIZ];
	int error;

	name[0] = '\0';
	error = sysctl_handle_string(oidp, name, sizeof(name), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	return (vpp_if_enable(name));
}

static int
vpp_sysctl_disable(SYSCTL_HANDLER_ARGS)
{
	char name[IFNAMSIZ];
	int error;

	name[0] = '\0';
	error = sysctl_handle_string(oidp, name, sizeof(name), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	return (vpp_if_disable(name));
}

static void
vpp_sysctl_init(void)
{
	struct sysctl_oid *root, *nodes, *errs, *stats;
	int i;

	sysctl_ctx_init(&vpp_clist);
	root = SYSCTL_ADD_NODE(&vpp_clist, SYSCTL_STATIC_CHILDREN(_net),
	    OID_AUTO, "vpp", CTLFLAG_RW | CTLFLAG_MPSAFE, NULL,
	    "VPP vectorized forwarding");
	SYSCTL_ADD_PROC(&vpp_clist, SYSCTL_CHILDREN(root), OID_AUTO, "enable",
	    CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0,
	    vpp_sysctl_enable, "A", "enable forwarding on the named interface");
	SYSCTL_ADD_PROC(&vpp_clist, SYSCTL_CHILDREN(root), OID_AUTO, "disable",
	    CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0,
	    vpp_sysctl_disable, "A", "disable forwarding on the named interface");

	nodes = SYSCTL_ADD_NODE(&vpp_clist, SYSCTL_CHILDREN(root), OID_AUTO,
	    "node", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "per-node packet counts");
	for (i = 0; i < VPP_N_NODES; i++)
		if (vpp_node_names[i] != NULL)
			SYSCTL_ADD_COUNTER_U64(&vpp_clist,
			    SYSCTL_CHILDREN(nodes), OID_AUTO, vpp_node_names[i],
			    CTLFLAG_RD, &vpp_node_pkts[i], "packets");

	errs = SYSCTL_ADD_NODE(&vpp_clist, SYSCTL_CHILDREN(root), OID_AUTO,
	    "error", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "per-error counts");
	for (i = 0; i < VPP_N_ERRORS; i++)
		SYSCTL_ADD_COUNTER_U64(&vpp_clist, SYSCTL_CHILDREN(errs),
		    OID_AUTO, vpp_err_names[i], CTLFLAG_RD, &vpp_err_cnt[i],
		    "count");

	stats = SYSCTL_ADD_NODE(&vpp_clist, SYSCTL_CHILDREN(root), OID_AUTO,
	    "stat", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "forwarding-path stats");
	for (i = 0; i < VPP_N_STATS; i++)
		SYSCTL_ADD_COUNTER_U64(&vpp_clist, SYSCTL_CHILDREN(stats),
		    OID_AUTO, vpp_stat_names[i], CTLFLAG_RD, &vpp_stat[i],
		    "count");
}

static int
vpp_init(void)
{
	int cpu, i;

	vpp_rt = malloc(sizeof(*vpp_rt) * (mp_maxid + 1), M_VPP,
	    M_WAITOK | M_ZERO);
	CPU_FOREACH(cpu) {
		vpp_rt[cpu] = malloc(sizeof(struct vpp_runtime), M_VPP,
		    M_WAITOK | M_ZERO);
		mtx_init(&vpp_rt[cpu]->lock, "vpp rt", NULL, MTX_DEF);
	}
	for (i = 0; i < VPP_N_NODES; i++)
		vpp_node_pkts[i] = counter_u64_alloc(M_WAITOK);
	for (i = 0; i < VPP_N_ERRORS; i++)
		vpp_err_cnt[i] = counter_u64_alloc(M_WAITOK);
	for (i = 0; i < VPP_N_STATS; i++)
		vpp_stat[i] = counter_u64_alloc(M_WAITOK);

	vpp_if_init();
	vpp_adj_init();
	vpp_sysctl_init();
	return (0);
}

static void
vpp_uninit(void)
{
	int cpu, i;

	vpp_if_uninit();	/* restores if_input and drains the net epoch */
	vpp_adj_fini();
	sysctl_ctx_free(&vpp_clist);

	CPU_FOREACH(cpu) {
		mtx_destroy(&vpp_rt[cpu]->lock);
		free(vpp_rt[cpu], M_VPP);
	}
	free(vpp_rt, M_VPP);
	for (i = 0; i < VPP_N_NODES; i++)
		counter_u64_free(vpp_node_pkts[i]);
	for (i = 0; i < VPP_N_ERRORS; i++)
		counter_u64_free(vpp_err_cnt[i]);
	for (i = 0; i < VPP_N_STATS; i++)
		counter_u64_free(vpp_stat[i]);
}

static int
vpp_modevent(module_t mod __unused, int what, void *arg __unused)
{

	switch (what) {
	case MOD_LOAD:
		return (vpp_init());
	case MOD_UNLOAD:
		vpp_uninit();
		return (0);
	case MOD_SHUTDOWN:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t vpp_mod = {
	"vpp",
	vpp_modevent,
	NULL
};

DECLARE_MODULE(vpp, vpp_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(vpp, 1);

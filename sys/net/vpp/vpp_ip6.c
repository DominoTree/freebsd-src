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
 * IPv6 forwarding nodes: validate, FIB lookup, hop-limit rewrite.  Mirrors the
 * IPv4 nodes and ip6_tryforward()'s semantics; the egress (L2 rewrite + TX) is
 * shared with IPv4 in the output node.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/pfil.h>
#include <net/route.h>
#include <net/route/nhop.h>

#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet6/in6_var.h>
#include <netinet6/ip6_var.h>
#include <netinet6/in6_fib.h>

#include <net/vpp/vpp.h>

/* Validate a transit IPv6 unicast packet; punt everything else. */
void
vpp_ip6_input_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	/*
	 * A configured firewall must see transit traffic; if pfil is hooked,
	 * punt the whole frame to the normal stack rather than bypass it.
	 */
	if (__predict_false(PFIL_HOOKED_IN(V_inet6_pfil_head) ||
	    PFIL_HOOKED_OUT(V_inet6_pfil_head))) {
		for (i = 0; i < f->n; i++)
			vpp_enq(rt, VPP_NODE_PUNT, f->bufs[i], &f->meta[i]);
		f->n = 0;
		return;
	}

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ip6_hdr *ip6;
		int plen;

		VPP_PREFETCH(f, i);
		if (m->m_len < md.l3_off + (int)sizeof(struct ip6_hdr)) {
			m = m_pullup(m, md.l3_off + sizeof(struct ip6_hdr));
			if (m == NULL) {
				counter_u64_add(vpp_err_cnt[VPP_ERR_IP4_HDR], 1);
				continue;
			}
		}
		ip6 = (struct ip6_hdr *)(mtod(m, char *) + md.l3_off);

		if ((ip6->ip6_vfc & IPV6_VERSION_MASK) != IPV6_VERSION) {
			md.error = VPP_ERR_IP4_HDR;
			goto punt;
		}
		/* Only forward transit unicast; mirror ip6_tryforward(). */
		if ((m->m_flags & (M_BCAST | M_MCAST)) ||
		    ip6->ip6_nxt == IPPROTO_HOPOPTS ||
		    IN6_IS_ADDR_MULTICAST(&ip6->ip6_dst) ||
		    IN6_IS_ADDR_LINKLOCAL(&ip6->ip6_dst) ||
		    IN6_IS_ADDR_LINKLOCAL(&ip6->ip6_src) ||
		    IN6_IS_ADDR_UNSPECIFIED(&ip6->ip6_dst) ||
		    IN6_IS_ADDR_UNSPECIFIED(&ip6->ip6_src) ||
		    in6_localip_fib(&ip6->ip6_dst, md.fib)) {
			md.error = VPP_ERR_LOCAL;
			goto punt;
		}
		plen = ntohs(ip6->ip6_plen);
		if (plen == 0 ||	/* jumbogram: needs hop-by-hop, slow path */
		    m->m_pkthdr.len - md.l3_off - (int)sizeof(struct ip6_hdr) <
		    plen) {
			md.error = VPP_ERR_IP4_LENGTH;
			goto punt;
		}
		if (ip6->ip6_hlim <= IPV6_HLIMDEC) {
			md.error = VPP_ERR_TTL;
			goto punt;
		}
		vpp_enq(rt, VPP_NODE_IP6_LOOKUP, m, &md);
		continue;
punt:
		counter_u64_add(vpp_err_cnt[md.error], 1);
		vpp_enq(rt, VPP_NODE_PUNT, m, &md);
	}
	f->n = 0;
}

/* Resolve next hop from the kernel IPv6 FIB; punt non-forwardable results. */
void
vpp_ip6_lookup_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ip6_hdr *ip6 =
		    (struct ip6_hdr *)(mtod(m, char *) + md.l3_off);
		struct nhop_object *nh;

		VPP_PREFETCH(f, i);
		nh = fib6_lookup(md.fib, &ip6->ip6_dst, 0, NHR_NONE,
		    m->m_pkthdr.flowid);
		if (nh == NULL) {
			md.error = VPP_ERR_NO_ROUTE;
			goto punt;
		}
		if (nh->nh_flags &
		    (NHF_REJECT | NHF_BLACKHOLE | NHF_BROADCAST)) {
			md.error = VPP_ERR_NO_ROUTE;
			goto punt;
		}
		if (if_getflags(nh->nh_ifp) & IFF_LOOPBACK) {
			md.error = VPP_ERR_LOCAL;
			goto punt;
		}
		if ((uint32_t)ntohs(ip6->ip6_plen) + sizeof(struct ip6_hdr) >
		    nh->nh_mtu) {
			md.error = VPP_ERR_MTU;
			goto punt;
		}
		md.nh = nh;
		vpp_enq(rt, VPP_NODE_IP6_REWRITE, m, &md);
		continue;
punt:
		counter_u64_add(vpp_err_cnt[md.error], 1);
		vpp_enq(rt, VPP_NODE_PUNT, m, &md);
	}
	f->n = 0;
}

/* Decrement the hop limit (IPv6 has no header checksum to adjust). */
void
vpp_ip6_rewrite_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ip6_hdr *ip6 =
		    (struct ip6_hdr *)(mtod(m, char *) + md.l3_off);

		VPP_PREFETCH(f, i);
		ip6->ip6_hlim -= IPV6_HLIMDEC;
		vpp_enq(rt, VPP_NODE_OUTPUT, m, &md);
	}
	f->n = 0;
}

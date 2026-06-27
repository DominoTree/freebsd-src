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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/route.h>
#include <net/route/nhop.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>

#include <machine/in_cksum.h>

#include <net/vpp/vpp.h>

/* IPv4 slow path: strip L2, finish checksums, hand to egress if_output. */
static void
vpp_output_slow4(struct mbuf *m, struct nhop_object *nh, uint16_t l3_off,
    uint32_t dest)
{
	struct ifnet *ifp = nh->nh_ifp;
	struct route ro;
	struct sockaddr_in *dst;
	const struct sockaddr *gw;
	struct ip *ip;

	m_adj(m, l3_off);
	ip = mtod(m, struct ip *);
	if (__predict_false(m->m_pkthdr.csum_flags & CSUM_IP &
	    ~ifp->if_hwassist)) {
		ip->ip_sum = 0;
		ip->ip_sum = in_cksum(m, ip->ip_hl << 2);
		m->m_pkthdr.csum_flags &= ~CSUM_IP;
	}
	if (__predict_false(m->m_pkthdr.csum_flags & CSUM_DELAY_DATA &
	    ~ifp->if_hwassist)) {
		in_delayed_cksum(m);
		m->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
	}
	bzero(&ro, sizeof(ro));
	dst = (struct sockaddr_in *)&ro.ro_dst;
	dst->sin_family = AF_INET;
	dst->sin_len = sizeof(*dst);
	dst->sin_addr.s_addr = dest;
	if (nh->nh_flags & NHF_GATEWAY) {
		gw = &nh->gw_sa;
		ro.ro_flags |= RT_HAS_GW;
	} else
		gw = (const struct sockaddr *)dst;
	m_clrprotoflags(m);
	if ((*ifp->if_output)(ifp, m, gw, &ro) != 0)
		counter_u64_add(vpp_err_cnt[VPP_ERR_TX], 1);
}

/* IPv6 slow path: strip L2, finish checksums, hand to egress if_output. */
static void
vpp_output_slow6(struct mbuf *m, struct nhop_object *nh, uint16_t l3_off)
{
	struct ifnet *ifp = nh->nh_ifp;
	struct sockaddr_in6 dst;
	struct ip6_hdr *ip6;

	m_adj(m, l3_off);
	ip6 = mtod(m, struct ip6_hdr *);
	if (__predict_false(m->m_pkthdr.csum_flags & CSUM_DELAY_DATA_IPV6 &
	    ~ifp->if_hwassist)) {
		int off = ip6_lasthdr(m, 0, IPPROTO_IPV6, NULL);

		if (off < (int)sizeof(struct ip6_hdr) ||
		    off > m->m_pkthdr.len) {
			m_freem(m);
			counter_u64_add(vpp_err_cnt[VPP_ERR_TX], 1);
			return;
		}
		in6_delayed_cksum(m, m->m_pkthdr.len - off, off);
		m->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA_IPV6;
	}
	bzero(&dst, sizeof(dst));
	dst.sin6_family = AF_INET6;
	dst.sin6_len = sizeof(dst);
	dst.sin6_addr = (nh->nh_flags & NHF_GATEWAY) ?
	    nh->gw6_sa.sin6_addr : ip6->ip6_dst;
	m_clrprotoflags(m);
	if ((*ifp->if_output)(ifp, m, (struct sockaddr *)&dst, NULL) != 0)
		counter_u64_add(vpp_err_cnt[VPP_ERR_TX], 1);
}

/*
 * Output node.  Fast path: cached egress L2 header and no pending checksum
 * (the common transit case) -> rewrite L2 in place and if_transmit() directly.
 * Otherwise fall to the family slow path via if_output().
 */
void
vpp_output_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct nhop_object *nh = md.nh;
		struct vpp_adj *adj;
		struct ether_header *eh;
		bool fast;

		VPP_PREFETCH(f, i);
		if (md.is_v6) {
			struct ip6_hdr *ip6 =
			    (struct ip6_hdr *)(mtod(m, char *) + md.l3_off);

			adj = vpp_adj_get6(rt, nh, &ip6->ip6_dst);
			fast = (adj != NULL && (m->m_pkthdr.csum_flags &
			    CSUM_DELAY_DATA_IPV6) == 0);
		} else {
			adj = vpp_adj_get(rt, nh, md.dest);
			fast = (adj != NULL && (m->m_pkthdr.csum_flags &
			    (CSUM_IP | CSUM_DELAY_DATA)) == 0);
		}

		if (__predict_true(fast)) {
			eh = mtod(m, struct ether_header *);
			memcpy(eh, adj->eh, ETHER_HDR_LEN);
			counter_u64_add(vpp_stat[VPP_STAT_FAST], 1);
			m_clrprotoflags(m);
			if (if_transmit(adj->txifp, m) != 0)
				counter_u64_add(vpp_err_cnt[VPP_ERR_TX], 1);
			continue;
		}

		counter_u64_add(vpp_stat[VPP_STAT_SLOW], 1);
		if (md.is_v6)
			vpp_output_slow6(m, nh, md.l3_off);
		else
			vpp_output_slow4(m, nh, md.l3_off, md.dest);
	}
	f->n = 0;
}

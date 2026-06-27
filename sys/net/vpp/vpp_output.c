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

#include <machine/in_cksum.h>

#include <net/vpp/vpp.h>

/*
 * Strip the ingress L2 header, finish any deferred checksums the egress NIC
 * cannot offload, and hand the L3 packet to the egress if_output (which does
 * the L2 rewrite and neighbour resolution).  Mirrors ip_tryforward()'s send.
 */
void
vpp_output_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct nhop_object *nh = md.nh;
		struct ifnet *ifp = nh->nh_ifp;
		struct route ro;
		struct sockaddr_in *dst;
		const struct sockaddr *gw;
		struct ip *ip;
		int error;

		m_adj(m, md.l3_off);
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
		dst->sin_addr.s_addr = md.dest;
		if (nh->nh_flags & NHF_GATEWAY) {
			gw = &nh->gw_sa;
			ro.ro_flags |= RT_HAS_GW;
		} else
			gw = (const struct sockaddr *)dst;

		m_clrprotoflags(m);
		error = (*ifp->if_output)(ifp, m, gw, &ro);
		if (error != 0)
			counter_u64_add(vpp_err_cnt[VPP_ERR_TX], 1);
	}
	f->n = 0;
}

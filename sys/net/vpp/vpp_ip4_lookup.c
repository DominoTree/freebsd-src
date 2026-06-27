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
#include <net/route.h>
#include <net/route/nhop.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_fib.h>
#include <netinet/ip.h>

#include <net/vpp/vpp.h>

/* Resolve next hop from the kernel FIB; punt non-forwardable results. */
void
vpp_ip4_lookup_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ip *ip = (struct ip *)(mtod(m, char *) + md.l3_off);
		struct nhop_object *nh;
		struct in_addr dst;

		VPP_PREFETCH(f, i);
		dst.s_addr = md.dest;
		nh = fib4_lookup(md.fib, dst, 0, NHR_NONE, m->m_pkthdr.flowid);
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
		if (ntohs(ip->ip_len) > nh->nh_mtu) {
			md.error = VPP_ERR_MTU;
			goto punt;
		}
		md.nh = nh;
		vpp_enq(rt, VPP_NODE_IP4_REWRITE, m, &md);
		continue;
punt:
		counter_u64_add(vpp_err_cnt[md.error], 1);
		vpp_enq(rt, VPP_NODE_PUNT, m, &md);
	}
	f->n = 0;
}

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

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>

#include <machine/in_cksum.h>

#include <net/vpp/vpp.h>

/* Validate a transit IPv4 unicast packet; punt everything else. */
void
vpp_ip4_input_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ip *ip;
		int hlen;
		uint16_t ip_len;

		if (m->m_len < md.l3_off + (int)sizeof(struct ip)) {
			m = m_pullup(m, md.l3_off + sizeof(struct ip));
			if (m == NULL) {
				counter_u64_add(vpp_err_cnt[VPP_ERR_IP4_HDR], 1);
				continue;
			}
		}
		ip = (struct ip *)(mtod(m, char *) + md.l3_off);

		if (ip->ip_v != IPVERSION ||
		    ip->ip_hl < (sizeof(struct ip) >> 2)) {
			md.error = VPP_ERR_IP4_HDR;
			goto punt;
		}
		hlen = ip->ip_hl << 2;
		if (hlen != sizeof(struct ip)) {
			md.error = VPP_ERR_IP4_OPTIONS;
			goto punt;
		}
		ip_len = ntohs(ip->ip_len);
		if (ip_len < sizeof(struct ip) ||
		    m->m_pkthdr.len - md.l3_off < ip_len) {
			md.error = VPP_ERR_IP4_LENGTH;
			goto punt;
		}
		if ((m->m_pkthdr.csum_flags & (CSUM_IP_CHECKED | CSUM_IP_VALID)) !=
		    (CSUM_IP_CHECKED | CSUM_IP_VALID)) {
			if (in_cksum_hdr(ip) != 0) {
				md.error = VPP_ERR_IP4_CSUM;
				goto punt;
			}
		}
		if (ip->ip_ttl <= IPTTLDEC) {
			md.error = VPP_ERR_TTL;
			goto punt;
		}
		/* Only forward transit unicast; mirror ip_tryforward(). */
		if ((m->m_flags & (M_BCAST | M_MCAST)) ||
		    (if_getflags(m->m_pkthdr.rcvif) & IFF_LOOPBACK) ||
		    in_broadcast(ip->ip_src) || in_broadcast(ip->ip_dst) ||
		    IN_MULTICAST(ntohl(ip->ip_src.s_addr)) ||
		    IN_MULTICAST(ntohl(ip->ip_dst.s_addr)) ||
		    IN_LINKLOCAL(ntohl(ip->ip_src.s_addr)) ||
		    IN_LINKLOCAL(ntohl(ip->ip_dst.s_addr)) ||
		    in_localip_fib(ip->ip_dst, md.fib)) {
			md.error = VPP_ERR_LOCAL;
			goto punt;
		}
		md.dest = ip->ip_dst.s_addr;
		vpp_enq(rt, VPP_NODE_IP4_LOOKUP, m, &md);
		continue;
punt:
		counter_u64_add(vpp_err_cnt[md.error], 1);
		vpp_enq(rt, VPP_NODE_PUNT, m, &md);
	}
	f->n = 0;
}

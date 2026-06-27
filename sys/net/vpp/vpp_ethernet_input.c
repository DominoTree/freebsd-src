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

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>

#include <net/vpp/vpp.h>

void
vpp_eth_input_node(struct vpp_runtime *rt, struct vpp_frame *f)
{
	uint32_t i;

	for (i = 0; i < f->n; i++) {
		struct mbuf *m = f->bufs[i];
		struct vpp_pktmeta md = f->meta[i];
		struct ether_header *eh;

		if (m->m_len < ETHER_HDR_LEN) {
			m = m_pullup(m, ETHER_HDR_LEN);
			if (m == NULL) {
				counter_u64_add(vpp_err_cnt[VPP_ERR_IP4_HDR], 1);
				continue;
			}
		}
		eh = mtod(m, struct ether_header *);
		if (ntohs(eh->ether_type) == ETHERTYPE_IP) {
			md.l3_off = ETHER_HDR_LEN;
			vpp_enq(rt, VPP_NODE_IP4_INPUT, m, &md);
		} else {
			md.error = VPP_ERR_NOT_IP4;
			vpp_enq(rt, VPP_NODE_PUNT, m, &md);
		}
	}
	f->n = 0;
}

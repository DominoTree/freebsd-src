/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1990, 1993
 *	The Regents of the University of California.
 * Copyright (c) 2010-2011 Juniper Networks, Inc.
 * All rights reserved.
 *
 * Portions of this software were developed by Robert N. M. Watson under
 * contract to Juniper Networks, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _NETINET_IN_PCB_VAR_H_
#define _NETINET_IN_PCB_VAR_H_

/*
 * Definitions shared between netinet/in_pcb.c and netinet6/in6_pcb.c
 */

#define	INP_UNCONNECTED	0x04000000	/* Not inserted into hashes. */
#define	INP_INLBGROUP	0x01000000	/* Inserted into inpcblbgroup. */

/*
 * The inpcb database change context used by bind(2) and connect(2) operations.
 * Normally it is located on the stack and is initialized like this:
 *	struct inpcbinfo_ctx ipictx = {
 *		.pcbinfo = pcbinfo
 *	};
 */
struct inpcbinfo_ctx {
	struct inpcbinfo	*pcbinfo;
	struct inpbucket	*ebucket,	/* Target exact bucket */
				*wbucket,	/* Target(or old) wild bucket */
				*pbucket;	/* Port bucket */
	struct lbgroupbucket	*lbbucket;	/* Load balancer bucket */
};

#define	INPBUCKET_LOCK(bucket)		mtx_lock(&(bucket)->lock)
#define	INPBUCKET_UNLOCK(bucket)	mtx_unlock(&(bucket)->lock)
#define	INPBUCKET_LOCK_ASSERT(bucket)	mtx_assert(&(bucket)->lock, MA_OWNED)

#define	IPI_LOCK(ipi)	INPBUCKET_LOCK(&(ipi)->ipi_list_unconn)
#define	IPI_UNLOCK(ipi)	INPBUCKET_UNLOCK(&(ipi)->ipi_list_unconn)
#define	IPI_LOCK_ASSERT(ipi)	INPBUCKET_LOCK_ASSERT(&(ipi)->ipi_list_unconn)

static inline void
inpcbinfo_ctx_wildlock(struct inpcbinfo_ctx *ipictx, uint16_t lport)
{
	if (ipictx->wbucket == NULL) {
		ipictx->wbucket = &ipictx->pcbinfo->ipi_hash_wild[
		    INP_PCBHASH_WILD(lport, ipictx->pcbinfo->ipi_hashmask)];
		INPBUCKET_LOCK(ipictx->wbucket);
	} else {
		MPASS(ipictx->wbucket == &ipictx->pcbinfo->ipi_hash_wild[
		    INP_PCBHASH_WILD(lport, ipictx->pcbinfo->ipi_hashmask)]);
		INPBUCKET_LOCK_ASSERT(ipictx->wbucket);
	}
}

static inline void
inpcbinfo_ctx_portlock(struct inpcbinfo_ctx *ipictx, uint16_t lport)
{
	if (ipictx->pbucket == NULL) {
		ipictx->pbucket = &ipictx->pcbinfo->ipi_porthash[
		    INP_PCBPORTHASH(lport, ipictx->pcbinfo->ipi_porthashmask)];
		INPBUCKET_LOCK(ipictx->pbucket);
	} else {
		MPASS(ipictx->pbucket == &ipictx->pcbinfo->ipi_porthash[
		    INP_PCBPORTHASH(lport, ipictx->pcbinfo->ipi_porthashmask)]);
		INPBUCKET_LOCK_ASSERT(ipictx->pbucket);
	}
}

static inline void
inpcbinfo_ctx_release(struct inpcbinfo_ctx *ipictx)
{
	if (ipictx->ebucket != NULL)
		INPBUCKET_UNLOCK(ipictx->ebucket);
	if (ipictx->wbucket != NULL)
		INPBUCKET_UNLOCK(ipictx->wbucket);
	if (ipictx->pbucket != NULL)
		INPBUCKET_UNLOCK(ipictx->pbucket);
	if (ipictx->lbbucket != NULL)
		INPBUCKET_UNLOCK(ipictx->lbbucket);
}

void	inp_lock(struct inpcb *inp, const inp_lookup_t lock);
void	inp_unlock(struct inpcb *inp, const inp_lookup_t lock);
int	inp_trylock(struct inpcb *inp, const inp_lookup_t lock);
bool	inp_smr_lock(struct inpcb *, const inp_lookup_t);
int	in_pcb_lport(struct inpcbinfo_ctx *, struct inpcb *, struct in_addr *,
	    u_short *, struct ucred *, int);
int	in_pcb_lport_dest(struct inpcbinfo_ctx *ipictx,
	    struct inpcb *inp, const struct sockaddr *lsa,
	    const struct sockaddr *fsa, u_short fport, struct ucred *cred,
	    int lookupflags, u_short *lportp);
struct inpcb *in_pcblookup_local(struct inpcbinfo_ctx *, struct in_addr,
	    u_short, int, int, struct ucred *);
struct inpcb *in6_pcblookup_local(struct inpcbinfo_ctx *,
	    const struct in6_addr *, u_short, int, int, struct ucred *);
struct inpcb *in6_pcblookup_internal(struct inpcbinfo_ctx *ipictx,
	    const struct in6_addr *faddr, u_int fport_arg,
	    const struct in6_addr *laddr, u_int lport_arg,
	    int lookupflags, uint8_t numa_domain, int fib);
int     in_pcbinshash(struct inpcb *, struct inpcbinfo_ctx *);
void    in_pcbrehash(struct inpcb *, struct inpcbinfo_ctx *);
void    in_pcbremhash(struct inpcb *);
struct inpcblbgroup *in_pcblbgroup_find(struct inpcb *inp,
	    struct lbgroupbucket **bucket);

/*
 * How many map slots each member of a load balance group is dealt, which
 * sets how finely the load can be divided between them, and the point at
 * which a very large group stops being divided any more finely.
 */
#define	INPCBLB_SLOTS		16
#define	INPCBLB_SLOTS_MAX	4096

/*
 * A load balance group's selection map.  Each CPU that receives for the
 * group owns a run of slots, and each slot names the member that serves it,
 * so that selection is two loads and the members nearest a CPU are the ones
 * it hands connections to.  A map is immutable once published: it is built
 * under the group's bucket lock, swapped in, and freed once no lookup can
 * still be holding it.
 */
struct inpcblbmap {
	struct epoch_context lm_epoch_ctx;
	uint32_t	lm_nslots;
	uint32_t	*lm_cpu;	/* per CPU, offset << 16 | count */
	struct inpcb	**lm_slot;	/* lm_nslots members */
};

/*
 * Load balance groups used for the SO_REUSEPORT_LB socket option. Each group
 * (or unique address:port combination) can be re-used at most
 * INPCBLBGROUP_SIZMAX (256) times. The inpcbs are stored in il_inp which
 * is dynamically resized as processes bind/unbind to that specific group.
 */
struct inpcblbgroup {
	CK_LIST_ENTRY(inpcblbgroup) il_list;
	LIST_HEAD(, inpcb) il_pending;	/* PCBs waiting for listen() */
	struct epoch_context il_epoch_ctx;
	struct ucred	*il_cred;
	uint16_t	il_lport;			/* (c) */
	u_char		il_vflag;			/* (c) */
	uint8_t		il_numa_domain;
	int		il_fibnum;
	union in_dependaddr il_dependladdr;		/* (c) */
#define	il_laddr	il_dependladdr.id4_addr
#define	il6_laddr	il_dependladdr.id6_addr
	uint32_t	il_inpsiz; /* max count in il_inp[] (h) */
	uint32_t	il_inpcnt; /* cur count in il_inp[] (h) */
	uint32_t	il_pendcnt; /* cur count in il_pending (h) */
	struct inpcblbmap *il_map; /* selection map, or NULL (h) */
	struct inpcb	*il_inp[];			/* (h) */
};

/*
 * Select a group member for a packet the stack is processing on the current
 * CPU.  The map, when there is one, names the members nearest to this CPU;
 * everything else is distributed by the packet hash as it has always been.
 * Never returns NULL for a non-empty group, as the caller reads NULL as "no
 * group" and would fall through to the wildcard hash.
 */
static inline struct inpcb *
in_pcblbgroup_select(const struct inpcblbgroup *grp, uint32_t hash)
{
	const struct inpcblbmap *map;
	struct inpcb *inp;
	uint32_t count, entry, n;

	count = atomic_load_acq_int(&grp->il_inpcnt);
	if (count == 0)
		return (NULL);

	map = atomic_load_consume_ptr(&grp->il_map);
	if (map != NULL) {
		entry = map->lm_cpu[curcpu];
		n = entry & 0xffff;
		if (n != 0) {
			/* The ports are in the low bits; scaling uses the high. */
			hash ^= hash >> 16;
			hash *= 0x85ebca6b;
			hash ^= hash >> 13;
			hash *= 0xc2b2ae35;
			hash ^= hash >> 16;
			return (map->lm_slot[(entry >> 16) +
			    (uint32_t)(((uint64_t)hash * n) >> 32)]);
		}
	}

	inp = grp->il_inp[hash % count];
	KASSERT(inp != NULL, ("%s: inp == NULL", __func__));
	return (inp);
}

#endif /* !_NETINET_IN_PCB_VAR_H_ */

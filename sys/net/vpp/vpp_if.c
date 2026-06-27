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
 * Per-interface RX interception.  Enabling VPP on an interface swaps its
 * if_input to vpp_if_input (netmap's generic-adapter pattern); the saved
 * handler receives any packets the engine punts.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/ck.h>
#include <sys/epoch.h>
#include <sys/eventhandler.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/vnet.h>

#include <net/vpp/vpp.h>

struct vpp_ifsc {
	CK_LIST_ENTRY(vpp_ifsc)	link;
	if_t			ifp;
	if_input_fn_t		saved_input;
	bool			lro_was_on;
	struct epoch_context	ectx;
};

static CK_LIST_HEAD(, vpp_ifsc) vpp_iflist = CK_LIST_HEAD_INITIALIZER();
static struct sx vpp_if_sx;
static eventhandler_tag vpp_if_departtag;

static struct vpp_ifsc *
vpp_if_find(if_t ifp)
{
	struct vpp_ifsc *sc;

	CK_LIST_FOREACH(sc, &vpp_iflist, link)
		if (sc->ifp == ifp)
			return (sc);
	return (NULL);
}

static void
vpp_ifsc_free(epoch_context_t ctx)
{
	struct vpp_ifsc *sc;

	sc = __containerof(ctx, struct vpp_ifsc, ectx);
	free(sc, M_VPP);
}

/*
 * Look up an interface by name across all vnets (so a jailed interface can be
 * enabled from the host).  Returns a referenced ifp, or NULL.
 */
static if_t
vpp_ifunit_ref_anyvnet(const char *name)
{
	struct vnet *vnet_iter;
	if_t ifp = NULL;

	VNET_LIST_RLOCK();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		ifp = ifunit_ref(name);
		CURVNET_RESTORE();
		if (ifp != NULL)
			break;
	}
	VNET_LIST_RUNLOCK();
	return (ifp);
}

static void
vpp_if_input(if_t ifp, struct mbuf *mh)
{
	struct epoch_tracker et;
	struct vpp_ifsc *sc;
	struct mbuf *punt, *n;
	if_input_fn_t saved_input;
	bool needs_epoch;

	/*
	 * if_input may be entered without the net epoch held; mirror
	 * ether_input() and enter it when the driver requires it.
	 */
	needs_epoch = (if_getflags(ifp) & IFF_NEEDSEPOCH) != 0;
	if (__predict_false(needs_epoch))
		NET_EPOCH_ENTER(et);

	sc = vpp_if_find(ifp);
	if (__predict_false(sc == NULL)) {
		if (needs_epoch)
			NET_EPOCH_EXIT(et);
		while (mh != NULL) {
			n = mh->m_nextpkt;
			m_freem(mh);
			mh = n;
		}
		return;
	}

	/*
	 * Cache saved_input while still epoch-protected: once the epoch is
	 * exited, a concurrent disable may free sc.
	 */
	saved_input = sc->saved_input;
	CURVNET_SET_QUIET(if_getvnet(ifp));
	punt = vpp_engine_input(ifp, mh);
	CURVNET_RESTORE();

	if (needs_epoch)
		NET_EPOCH_EXIT(et);
	if (punt != NULL)
		saved_input(ifp, punt);
}

int
vpp_if_enable(const char *name)
{
	if_t ifp;
	struct vpp_ifsc *sc;

	ifp = vpp_ifunit_ref_anyvnet(name);
	if (ifp == NULL)
		return (ENXIO);
	if (if_gettype(ifp) != IFT_ETHER) {
		if_rele(ifp);
		return (EOPNOTSUPP);
	}

	sx_xlock(&vpp_if_sx);
	if (vpp_if_find(ifp) != NULL) {
		sx_xunlock(&vpp_if_sx);
		if_rele(ifp);
		return (EEXIST);
	}
	sc = malloc(sizeof(*sc), M_VPP, M_WAITOK | M_ZERO);
	sc->ifp = ifp;		/* keep the ifunit_ref for sc's lifetime */
	IFNET_RLOCK();
	sc->saved_input = if_getinputfn(ifp);
	CK_LIST_INSERT_HEAD(&vpp_iflist, sc, link);
	if_setinputfn(ifp, vpp_if_input);
	IFNET_RUNLOCK();
	/*
	 * LRO coalesces transit TCP before if_input, hiding it from the
	 * engine; disable it while VPP owns the interface.
	 */
	if (if_getcapenable(ifp) & IFCAP_LRO) {
		sc->lro_was_on = true;
		if_setcapenablebit(ifp, 0, IFCAP_LRO);
	}
	sx_xunlock(&vpp_if_sx);
	return (0);
}

int
vpp_if_disable(const char *name)
{
	if_t ifp;
	struct vpp_ifsc *sc;

	ifp = vpp_ifunit_ref_anyvnet(name);
	if (ifp == NULL)
		return (ENXIO);

	sx_xlock(&vpp_if_sx);
	sc = vpp_if_find(ifp);
	if (sc == NULL) {
		sx_xunlock(&vpp_if_sx);
		if_rele(ifp);
		return (ENOENT);
	}
	IFNET_RLOCK();
	if_setinputfn(ifp, sc->saved_input);
	CK_LIST_REMOVE(sc, link);
	IFNET_RUNLOCK();
	if (sc->lro_was_on)
		if_setcapenablebit(ifp, IFCAP_LRO, 0);
	sx_xunlock(&vpp_if_sx);

	NET_EPOCH_WAIT();		/* drain in-flight vpp_if_input */
	if_rele(sc->ifp);		/* release the enable-time reference */
	free(sc, M_VPP);
	if_rele(ifp);			/* release this call's reference */
	return (0);
}

static void
vpp_if_departure(void *arg __unused, if_t ifp)
{
	struct vpp_ifsc *sc;

	sx_xlock(&vpp_if_sx);
	sc = vpp_if_find(ifp);
	if (sc != NULL) {
		IFNET_RLOCK();
		if_setinputfn(ifp, sc->saved_input);
		CK_LIST_REMOVE(sc, link);
		IFNET_RUNLOCK();
		if_rele(sc->ifp);
		NET_EPOCH_CALL(vpp_ifsc_free, &sc->ectx);
	}
	sx_xunlock(&vpp_if_sx);
}

void
vpp_if_init(void)
{

	sx_init(&vpp_if_sx, "vpp if");
	vpp_if_departtag = EVENTHANDLER_REGISTER(ifnet_departure_event,
	    vpp_if_departure, NULL, EVENTHANDLER_PRI_ANY);
}

void
vpp_if_uninit(void)
{
	struct vpp_ifsc *sc;

	EVENTHANDLER_DEREGISTER(ifnet_departure_event, vpp_if_departtag);

	sx_xlock(&vpp_if_sx);
	while ((sc = CK_LIST_FIRST(&vpp_iflist)) != NULL) {
		IFNET_RLOCK();
		if_setinputfn(sc->ifp, sc->saved_input);
		CK_LIST_REMOVE(sc, link);
		IFNET_RUNLOCK();
		if (sc->lro_was_on)
			if_setcapenablebit(sc->ifp, IFCAP_LRO, 0);
		if_rele(sc->ifp);
		NET_EPOCH_CALL(vpp_ifsc_free, &sc->ectx);
	}
	sx_xunlock(&vpp_if_sx);

	NET_EPOCH_WAIT();
	NET_EPOCH_DRAIN_CALLBACKS();
	sx_destroy(&vpp_if_sx);
}

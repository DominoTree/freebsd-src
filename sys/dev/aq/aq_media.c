/*
 * aQuantia Corporation Network Driver
 * Copyright (C) 2014-2017 aQuantia Corporation. All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   (1) Redistributions of source code must retain the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer.
 *
 *   (2) Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   (3)The name of the author may not be used to endorse or promote
 *   products derived from this software without specific prior
 *   written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_var.h>
#include <net/iflib.h>
#include <net/sff8472.h>

#include "aq_device.h"

#include "aq_fw.h"
#include "aq_dbg.h"

/*
 * Transceiver compliance codes, SFF-8472 rev 12.5a table 5-3, and the two
 * SFF-8024 table 4-4 extended codes a BASE-T module identifies itself by.
 * <net/sff8472.h> carries the byte offsets but not the bit assignments.
 */
#define	AQ_SFF_TRANS_10G	3	/* 10G Ethernet compliance */
#define	 AQ_SFF_10G_SR		(1 << 4)
#define	 AQ_SFF_10G_LR		(1 << 5)
#define	 AQ_SFF_10G_LRM		(1 << 6)
#define	 AQ_SFF_10G_ER		(1 << 7)
#define	AQ_SFF_TRANS_1G		6	/* Ethernet compliance */
#define	 AQ_SFF_1G_SX		(1 << 0)
#define	 AQ_SFF_1G_LX		(1 << 1)
#define	 AQ_SFF_1G_CX		(1 << 2)
#define	 AQ_SFF_1G_T		(1 << 3)
#define	 AQ_SFF_100_FX		(1 << 5)
#define	AQ_SFF_TRANS_CABLE	8	/* SFP+ cable technology */
#define	 AQ_SFF_CABLE_PASSIVE	(1 << 2)
#define	 AQ_SFF_CABLE_ACTIVE	(1 << 3)
#define	AQ_SFF_EXT_10G_T_SFI	0x16	/* 10GBASE-T, SFI electrical interface */
#define	AQ_SFF_EXT_10G_T_SR	0x1c	/* 10GBASE-T short reach */

/*
 * Every pluggable name a speed can go by, most generic first, NULL-terminated.
 * The first entry is what the speed is called until a module has been read.
 */
static const int aq_sfp_100m[] = { IFM_100_SGMII, IFM_100_FX, 0 };
static const int aq_sfp_1g[] = {
	IFM_1000_SGMII, IFM_1000_SX, IFM_1000_LX, IFM_1000_CX, 0
};
static const int aq_sfp_2g5[] = { IFM_2500_X, 0 };
static const int aq_sfp_10g[] = {
	IFM_10G_SFI, IFM_10G_SR, IFM_10G_LR, IFM_10G_LRM, IFM_10G_ER,
	IFM_10G_TWINAX, 0
};

/*
 * Single source of truth for the supported link speeds.  A NULL ifm_sfp
 * means the speed has no standard pluggable equivalent and goes by its
 * twisted-pair name even in a cage.
 */
static const struct aq_media_map {
	uint32_t		link_bit;	/* AQ_LINK_* capability bit */
	enum aq_fw_link_speed	fw_rate;	/* aq_fw_* rate */
	int			ifm_tp;		/* IFM_* subtype, copper */
	const int		*ifm_sfp;	/* IFM_* subtypes, SFP cage */
	uint32_t		mbps;		/* link speed, Mbit/s */
} aq_media_types[] = {
	{ AQ_LINK_10M,  aq_fw_10M,  IFM_10_T,   NULL,        10 },
	{ AQ_LINK_100M, aq_fw_100M, IFM_100_TX, aq_sfp_100m, 100 },
	{ AQ_LINK_1G,   aq_fw_1G,   IFM_1000_T, aq_sfp_1g,   1000 },
	{ AQ_LINK_2G5,  aq_fw_2G5,  IFM_2500_T, aq_sfp_2g5,  2500 },
	{ AQ_LINK_5G,   aq_fw_5G,   IFM_5000_T, NULL,        5000 },
	{ AQ_LINK_10G,  aq_fw_10G,  IFM_10G_T,  aq_sfp_10g,  10000 },
};

/*
 * Compliance bits that name a medium, most specific first: the first one
 * a module asserts wins.  A cable technology bit is the fallback for both
 * speeds, since a passive or active copper cable states no optical code.
 */
static const struct aq_sff_map {
	uint8_t	offset;		/* page 00h byte */
	uint8_t	mask;		/* bits that name this medium */
	int	ifm;		/* IFM_* subtype they name */
} aq_sff_10g[] = {
	{ AQ_SFF_TRANS_10G,	AQ_SFF_10G_SR,	IFM_10G_SR },
	{ AQ_SFF_TRANS_10G,	AQ_SFF_10G_LR,	IFM_10G_LR },
	{ AQ_SFF_TRANS_10G,	AQ_SFF_10G_LRM,	IFM_10G_LRM },
	{ AQ_SFF_TRANS_10G,	AQ_SFF_10G_ER,	IFM_10G_ER },
	{ AQ_SFF_TRANS_CABLE,	AQ_SFF_CABLE_ACTIVE |
				AQ_SFF_CABLE_PASSIVE,	IFM_10G_TWINAX },
}, aq_sff_1g[] = {
	{ AQ_SFF_TRANS_1G,	AQ_SFF_1G_SX,		IFM_1000_SX },
	{ AQ_SFF_TRANS_1G,	AQ_SFF_1G_LX,		IFM_1000_LX },
	{ AQ_SFF_TRANS_1G,	AQ_SFF_1G_CX,		IFM_1000_CX },
	{ AQ_SFF_TRANS_1G,	AQ_SFF_1G_T,		IFM_1000_T },
	{ AQ_SFF_TRANS_CABLE,	AQ_SFF_CABLE_ACTIVE |
				AQ_SFF_CABLE_PASSIVE,	IFM_1000_CX },
}, aq_sff_100m[] = {
	{ AQ_SFF_TRANS_1G,	AQ_SFF_100_FX,		IFM_100_FX },
};

static int
aq_sff_ifm(const uint8_t *code, const struct aq_sff_map *map, u_int n)
{
	u_int i;

	for (i = 0; i < n; i++)
		if ((code[map[i].offset] & map[i].mask) != 0)
			return (map[i].ifm);

	return (0);
}

/* A module can be swapped whenever the port is down, so forget it then. */
void
aq_sfp_forget(struct aq_dev *aq_dev)
{
	aq_dev->sfp_ifm_100m = 0;
	aq_dev->sfp_ifm_1g = 0;
	aq_dev->sfp_ifm_10g = 0;
	aq_dev->sfp_known = false;
}

/* Names the medium in the cage, not what the cage could in principle hold. */
static void
aq_sfp_scan(struct aq_dev *aq_dev)
{
	struct aq_hw *hw = &aq_dev->hw;
	uint8_t code[SFF_8472_TRANS_END + 1];	/* page 00h from byte zero */
	uint8_t ext;
	int ifm_100m = 0, ifm_1g = 0, ifm_10g = 0;

	if (hw->fw_ops == NULL || hw->fw_ops->get_module_eeprom == NULL)
		goto commit;

	/* A failed read says nothing about the module: keep the last answer. */
	if (hw->fw_ops->get_module_eeprom(hw, SFF_8472_BASE, SFF_8472_ID,
	    sizeof code, code) != 0)
		return;

	/* Only these lay page 00h out the way the codes below assume. */
	if (code[SFF_8472_ID] != SFF_8472_ID_SFP &&
	    code[SFF_8472_ID] != SFF_8472_ID_SFF)
		goto commit;

	ifm_10g = aq_sff_ifm(code, aq_sff_10g, nitems(aq_sff_10g));
	ifm_1g = aq_sff_ifm(code, aq_sff_1g, nitems(aq_sff_1g));
	ifm_100m = aq_sff_ifm(code, aq_sff_100m, nitems(aq_sff_100m));

	/*
	 * A BASE-T module states so only in the extended code and claims no
	 * optical compliance, so only spend the round trip when the codes
	 * above found nothing optical.
	 */
	if ((ifm_10g == 0 || ifm_10g == IFM_10G_TWINAX) &&
	    hw->fw_ops->get_module_eeprom(hw, SFF_8472_BASE, SFF_8472_TRANS,
	    sizeof ext, &ext) == 0 &&
	    (ext == AQ_SFF_EXT_10G_T_SFI || ext == AQ_SFF_EXT_10G_T_SR))
		ifm_10g = IFM_10G_T;

commit:
	aq_dev->sfp_ifm_100m = ifm_100m;
	aq_dev->sfp_ifm_1g = ifm_1g;
	aq_dev->sfp_ifm_10g = ifm_10g;
	aq_dev->sfp_known = true;
}

/*
 * Fibre parts prefer what the module reported, then the generic pluggable
 * name.  A speed with no pluggable name still runs over the cage, so fall
 * back to the twisted-pair name rather than leave the speed unnameable.
 */
static int
aq_media_ifm(const struct aq_dev *aq_dev, u_int i)
{
	if (aq_dev->media_type == AQ_MEDIA_TYPE_FIBRE) {
		if (aq_media_types[i].link_bit == AQ_LINK_10G &&
		    aq_dev->sfp_ifm_10g != 0)
			return (aq_dev->sfp_ifm_10g);
		if (aq_media_types[i].link_bit == AQ_LINK_1G &&
		    aq_dev->sfp_ifm_1g != 0)
			return (aq_dev->sfp_ifm_1g);
		if (aq_media_types[i].link_bit == AQ_LINK_100M &&
		    aq_dev->sfp_ifm_100m != 0)
			return (aq_dev->sfp_ifm_100m);
		if (aq_media_types[i].ifm_sfp != NULL)
			return (aq_media_types[i].ifm_sfp[0]);
	}

	return (aq_media_types[i].ifm_tp);
}

/* Any name this speed goes by selects it, whatever module is in the cage. */
static bool
aq_media_matches(const struct aq_dev *aq_dev, u_int i, int user_media)
{
	const int *ifm;

	if (user_media == aq_media_types[i].ifm_tp)
		return (true);

	if (aq_dev->media_type != AQ_MEDIA_TYPE_FIBRE)
		return (false);

	for (ifm = aq_media_types[i].ifm_sfp; ifm != NULL && *ifm != 0; ifm++)
		if (*ifm == user_media)
			return (true);

	return (false);
}

void
aq_mediastatus_update(struct aq_dev *aq_dev, uint32_t link_speed,
const struct aq_hw_fc_info *fc_neg)
{
	struct aq_hw *hw = &aq_dev->hw;
	u_int i;
	int ifm;

	aq_dev->media_active = 0;
	if (fc_neg->fc_rx)
		aq_dev->media_active |= IFM_ETH_RXPAUSE;
	if (fc_neg->fc_tx)
		aq_dev->media_active |= IFM_ETH_TXPAUSE;

	/*
	 * Identify the module once per link session.  A retrain cannot have
	 * swapped it, and a swap can only happen while the port is down, so
	 * that is where the answer is discarded.
	 */
	if (aq_dev->media_type == AQ_MEDIA_TYPE_FIBRE) {
		if (link_speed == 0)
			aq_sfp_forget(aq_dev);
		else if (!aq_dev->sfp_known)
			aq_sfp_scan(aq_dev);
	}

	for (i = 0; i < nitems(aq_media_types); i++)
		if (link_speed == aq_media_types[i].mbps)
			break;

	if (i == nitems(aq_media_types)) {
		aq_dev->media_active |= IFM_NONE;
	} else {
		ifm = aq_media_ifm(aq_dev, i);
		aq_dev->media_active |= ifm | IFM_FDX;
	}

	if (hw->link_rate == aq_fw_speed_auto)
		aq_dev->media_active |= IFM_AUTO;
}

void
aq_mediastatus(if_t ifp, struct ifmediareq *ifmr)
{
	struct aq_dev *aq_dev = iflib_get_softc(if_getsoftc(ifp));

	ifmr->ifm_active = IFM_ETHER;
	ifmr->ifm_status = IFM_AVALID;

	if (aq_dev->linkup)
		ifmr->ifm_status |= IFM_ACTIVE;

	ifmr->ifm_active |= aq_dev->media_active;
}

int
aq_mediachange(if_t ifp)
{
	struct aq_dev          *aq_dev = iflib_get_softc(if_getsoftc(ifp));
	struct aq_hw      *hw = &aq_dev->hw;
	uint64_t           old_media_rate = if_getbaudrate(ifp);
	int                old_link_speed = hw->link_rate;
	struct ifmedia    *ifm = iflib_get_media(aq_dev->ctx);
	int                user_media = IFM_SUBTYPE(ifm->ifm_media);
	uint64_t           media_rate;
	u_int              i;

	AQ_DBG_ENTERA("media 0x%x", user_media);

	if (!(ifm->ifm_media & IFM_ETHER)) {
		device_printf(aq_dev->dev,
		    "%s(): aq_dev interface - bad media: 0x%X\n", __FUNCTION__,
		    ifm->ifm_media);
		return (0);    // should never happen
	}

	switch (user_media) {
	case IFM_AUTO: // auto-select media
		hw->link_rate = aq_fw_speed_auto;
		media_rate = -1;
	break;

	case IFM_NONE: // disable media
		media_rate = 0;
		hw->link_rate = 0;
		iflib_link_state_change(aq_dev->ctx, LINK_STATE_DOWN,  0);
	break;

	default:
		for (i = 0; i < nitems(aq_media_types); i++)
			if (aq_media_matches(aq_dev, i, user_media))
				break;
		if (i == nitems(aq_media_types)) {
			device_printf(hw->dev, "unknown media: 0x%X\n",
			    user_media);
			return (0);
		}
		hw->link_rate = aq_media_types[i].fw_rate;
		media_rate = ifmedia_baudrate(ifm->ifm_media);
	break;
	}
	hw->fc.fc_rx = (ifm->ifm_media & IFM_ETH_RXPAUSE) ? 1 : 0;
	hw->fc.fc_tx = (ifm->ifm_media & IFM_ETH_TXPAUSE) ? 1 : 0;

	/* In down state just remember new link speed */
	if (!(if_getflags(ifp) & IFF_UP))
		return (0);

	if ((media_rate != old_media_rate) ||
	    (hw->link_rate != old_link_speed)) {
		// re-initialize hardware with new parameters
		aq_hw_set_link_speed(hw, hw->link_rate);
	}

	AQ_DBG_EXIT(0);
	return (0);
}

static void
aq_add_media_types(struct aq_dev *aq_dev, int media_link_speed)
{
	ifmedia_add(aq_dev->media, IFM_ETHER | media_link_speed | IFM_FDX, 0,
	    NULL);
	ifmedia_add(aq_dev->media, IFM_ETHER | media_link_speed | IFM_FDX |
	    IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE, 0, NULL);
	ifmedia_add(aq_dev->media, IFM_ETHER | media_link_speed | IFM_FDX |
	    IFM_ETH_RXPAUSE, 0, NULL);
	ifmedia_add(aq_dev->media, IFM_ETHER | media_link_speed | IFM_FDX |
	    IFM_ETH_TXPAUSE, 0, NULL);
}

static void
aq_media_populate(struct aq_dev *aq_dev)
{
	const int *ifm;
	u_int i;

	// ifconfig eth0 none
	ifmedia_add(aq_dev->media, IFM_ETHER | IFM_NONE, 0, NULL);

	ifmedia_add(aq_dev->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	aq_add_media_types(aq_dev, IFM_AUTO);

	for (i = 0; i < nitems(aq_media_types); i++) {
		if ((aq_dev->link_speeds & aq_media_types[i].link_bit) == 0)
			continue;
		aq_add_media_types(aq_dev, aq_media_types[i].ifm_tp);
		if (aq_dev->media_type != AQ_MEDIA_TYPE_FIBRE)
			continue;
		for (ifm = aq_media_types[i].ifm_sfp;
		    ifm != NULL && *ifm != 0; ifm++)
			aq_add_media_types(aq_dev, *ifm);
	}
}

void
aq_initmedia(struct aq_dev *aq_dev)
{

	AQ_DBG_ENTER();

	aq_sfp_forget(aq_dev);

	aq_media_populate(aq_dev);

	// link is initially autoselect
	ifmedia_set(aq_dev->media,
	    IFM_ETHER | IFM_AUTO | IFM_FDX | IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE);

	AQ_DBG_EXIT(0);
}

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
 * Single source of truth for the supported link speeds.  A zero fibre
 * subtype means the speed has no standard pluggable equivalent.
 */
static const struct aq_media_map {
	uint32_t		link_bit;	/* AQ_LINK_* capability bit */
	enum aq_fw_link_speed	fw_rate;	/* aq_fw_* rate */
	int			ifm_tp;		/* IFM_* subtype, copper */
	int			ifm_fibre;	/* IFM_* subtype, SFP cage */
	uint32_t		mbps;		/* link speed, Mbit/s */
} aq_media_types[] = {
	{ AQ_LINK_10M,  aq_fw_10M,  IFM_10_T,   0,            10 },
	{ AQ_LINK_100M, aq_fw_100M, IFM_100_TX, IFM_100_FX,   100 },
	{ AQ_LINK_1G,   aq_fw_1G,   IFM_1000_T, IFM_1000_SX,  1000 },
	{ AQ_LINK_2G5,  aq_fw_2G5,  IFM_2500_T, IFM_2500_SX,  2500 },
	{ AQ_LINK_5G,   aq_fw_5G,   IFM_5000_T, 0,            5000 },
	{ AQ_LINK_10G,  aq_fw_10G,  IFM_10G_T,  IFM_10G_SFI,  10000 },
};

/* SFF-8472 table 3.5 compliance bits, relative to SFF_8472_TRANS_START. */
#define	AQ_SFF_TRANS_10G	0		/* byte 3 */
#define	  AQ_SFF_10G_SR		  0x10
#define	  AQ_SFF_10G_LR		  0x20
#define	  AQ_SFF_10G_LRM	  0x40
#define	  AQ_SFF_10G_ER		  0x80
#define	AQ_SFF_TRANS_1G		3		/* byte 6 */
#define	  AQ_SFF_1G_SX		  0x01
#define	  AQ_SFF_1G_LX		  0x02
#define	  AQ_SFF_1G_CX		  0x04
#define	  AQ_SFF_1G_T		  0x08
#define	AQ_SFF_TRANS_CABLE	5		/* byte 8 */
#define	  AQ_SFF_CABLE_ACTIVE	  0x04
#define	  AQ_SFF_CABLE_PASSIVE	  0x08

/* Names the medium in the cage, not what the cage could in principle hold. */
static void
aq_sfp_scan(struct aq_dev *aq_dev)
{
	struct aq_hw *hw = &aq_dev->hw;
	uint8_t code[SFF_8472_TRANS_END - SFF_8472_TRANS_START + 1];
	uint8_t cable;

	aq_dev->sfp_ifm_1g = 0;
	aq_dev->sfp_ifm_10g = 0;

	if (hw->fw_ops == NULL || hw->fw_ops->get_module_eeprom == NULL)
		return;

	if (hw->fw_ops->get_module_eeprom(hw, SFF_8472_BASE,
	    SFF_8472_TRANS_START, sizeof code, code) != 0)
		return;

	cable = code[AQ_SFF_TRANS_CABLE] &
	    (AQ_SFF_CABLE_ACTIVE | AQ_SFF_CABLE_PASSIVE);

	if (code[AQ_SFF_TRANS_10G] & AQ_SFF_10G_SR)
		aq_dev->sfp_ifm_10g = IFM_10G_SR;
	else if (code[AQ_SFF_TRANS_10G] & AQ_SFF_10G_LR)
		aq_dev->sfp_ifm_10g = IFM_10G_LR;
	else if (code[AQ_SFF_TRANS_10G] & AQ_SFF_10G_LRM)
		aq_dev->sfp_ifm_10g = IFM_10G_LRM;
	else if (code[AQ_SFF_TRANS_10G] & AQ_SFF_10G_ER)
		aq_dev->sfp_ifm_10g = IFM_10G_ER;
	else if (cable != 0)
		aq_dev->sfp_ifm_10g = IFM_10G_TWINAX;

	if (code[AQ_SFF_TRANS_1G] & AQ_SFF_1G_SX)
		aq_dev->sfp_ifm_1g = IFM_1000_SX;
	else if (code[AQ_SFF_TRANS_1G] & AQ_SFF_1G_LX)
		aq_dev->sfp_ifm_1g = IFM_1000_LX;
	else if (code[AQ_SFF_TRANS_1G] & AQ_SFF_1G_CX)
		aq_dev->sfp_ifm_1g = IFM_1000_CX;
	else if (code[AQ_SFF_TRANS_1G] & AQ_SFF_1G_T)
		aq_dev->sfp_ifm_1g = IFM_1000_T;
	else if (cable != 0)
		aq_dev->sfp_ifm_1g = IFM_1000_CX;
}

/* Fibre parts prefer what the module reported over the table default. */
static int
aq_media_ifm(const struct aq_dev *aq_dev, u_int i)
{
	if (aq_dev->media_type != AQ_MEDIA_TYPE_FIBRE)
		return (aq_media_types[i].ifm_tp);

	if (aq_media_types[i].link_bit == AQ_LINK_10G &&
	    aq_dev->sfp_ifm_10g != 0)
		return (aq_dev->sfp_ifm_10g);
	if (aq_media_types[i].link_bit == AQ_LINK_1G &&
	    aq_dev->sfp_ifm_1g != 0)
		return (aq_dev->sfp_ifm_1g);

	return (aq_media_types[i].ifm_fibre);
}

/* A module read after attach can rename an already advertised speed. */
static bool
aq_media_matches(const struct aq_dev *aq_dev, u_int i, int user_media)
{
	if (user_media == aq_media_ifm(aq_dev, i))
		return (true);

	return (aq_dev->media_type == AQ_MEDIA_TYPE_FIBRE &&
	    aq_media_types[i].ifm_fibre != 0 &&
	    user_media == aq_media_types[i].ifm_fibre);
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

	/* The module can be swapped while the port is down. */
	if (aq_dev->media_type == AQ_MEDIA_TYPE_FIBRE && link_speed != 0)
		aq_sfp_scan(aq_dev);

	for (i = 0; i < nitems(aq_media_types); i++)
		if (link_speed == aq_media_types[i].mbps)
			break;

	if (i == nitems(aq_media_types)) {
		aq_dev->media_active |= IFM_NONE;
	} else {
		ifm = aq_media_ifm(aq_dev, i);
		aq_dev->media_active |= (ifm != 0 ? ifm : IFM_OTHER) | IFM_FDX;
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
	int                old_media_rate = if_getbaudrate(ifp);
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
		media_rate = (uint64_t)aq_media_types[i].mbps * 1000;
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
void
aq_initmedia(struct aq_dev *aq_dev)
{
	u_int i;

	AQ_DBG_ENTER();

	aq_dev->sfp_ifm_1g = 0;
	aq_dev->sfp_ifm_10g = 0;

	// ifconfig eth0 none
	ifmedia_add(aq_dev->media, IFM_ETHER | IFM_NONE, 0, NULL);

	ifmedia_add(aq_dev->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	aq_add_media_types(aq_dev, IFM_AUTO);

	for (i = 0; i < nitems(aq_media_types); i++) {
		if ((aq_dev->link_speeds & aq_media_types[i].link_bit) == 0)
			continue;
		if (aq_media_ifm(aq_dev, i) == 0)
			continue;
		aq_add_media_types(aq_dev, aq_media_ifm(aq_dev, i));
	}

	// link is initially autoselect
	ifmedia_set(aq_dev->media,
	    IFM_ETHER | IFM_AUTO | IFM_FDX | IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE);

	AQ_DBG_EXIT(0);
}

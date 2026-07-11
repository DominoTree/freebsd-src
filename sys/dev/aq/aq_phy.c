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

/*
 * Clause 45 MDIO access to the Atlantic 1 on-board PHY, via the MIF MDIO
 * interface registers.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/errno.h>

#include "aq_common.h"
#include "aq_hw.h"
#include "aq_hw_llh.h"
#include "aq_hw_llh_internal.h"
#include "aq_phy.h"

/* MIF MDIO interface registers, numbered as in the register spec. */
#define AQ_MDIO_IFACE_CONTROL	2U
#define AQ_MDIO_IFACE_WDATA	3U
#define AQ_MDIO_IFACE_ADDRESS	4U
#define AQ_MDIO_IFACE_RDATA	5U

/* MDIO controller operation modes. */
#define AQ_MDIO_OP_READ		1U
#define AQ_MDIO_OP_WRITE	2U
#define AQ_MDIO_OP_ADDRESS	3U

/* Clause 45 MMD device addresses. */
#define AQ_MMD_PMAPMD		1U
#define AQ_MMD_VEND1		30U

/* PMA/PMD device identifier: 1.2 is the MSW, 1.3 the LSW. */
#define AQ_MMD_PMAPMD_DEVID_MSW	2U
#define AQ_MMD_PMAPMD_DEVID_LSW	3U

#define AQ_PHY_ID_MAX		32U

/* An idle MDIO bus floats high, so an absent PHY reads back all-ones. */
#define AQ_PHY_NO_RESPONSE	0xffffU

/* PTP block enable, in each of the vendor PTP control registers. */
#define AQ_PHY_PTP_EN		BIT(10)

static const uint16_t aq_phy_ptp_regs[] = { 0x031e, 0x031d, 0x031c, 0x031b };

/* MDIO transactions and the MDIO semaphore both poll for up to 100ms. */
#define AQ_PHY_WAIT_US		10U
#define AQ_PHY_WAIT_COUNT	10000U

static bool aq_mdio_wait(struct aq_hw *hw);
static bool aq_mdio_op(struct aq_hw *hw, uint16_t mmd, uint32_t op);
static bool aq_mdio_read_word(struct aq_hw *hw, uint16_t mmd, uint16_t addr,
    uint16_t *val);
static bool aq_mdio_write_word(struct aq_hw *hw, uint16_t mmd, uint16_t addr,
    uint16_t data);
static bool aq_phy_sem_acquire(struct aq_hw *hw);
static void aq_phy_sem_release(struct aq_hw *hw);
static bool aq_phy_read_reg(struct aq_hw *hw, uint16_t mmd, uint16_t addr,
    uint16_t *val);
static bool aq_phy_clear_bits(struct aq_hw *hw, uint16_t mmd, uint16_t addr,
    uint16_t mask);
static bool aq_phy_find(struct aq_hw *hw);

static bool
aq_mdio_wait(struct aq_hw *hw)
{
	return (AQ_HW_WAIT_FOR(mdio_busy_get(hw) == 0U, AQ_PHY_WAIT_US,
	    AQ_PHY_WAIT_COUNT) == 0);
}

/* Run one operation against the PHY selected by hw->phy_id. */
static bool
aq_mdio_op(struct aq_hw *hw, uint16_t mmd, uint32_t op)
{
	uint32_t phy_addr;

	KASSERT(hw->phy_id < AQ_PHY_ID_MAX,
	    ("%s: invalid phy_id %u", __func__, hw->phy_id));

	phy_addr = (hw->phy_id << 5) | mmd;
	reg_glb_mdio_iface_set(hw, AQ_MDIO_IFACE_CONTROL,
	    mdio_execute_operation_msk | (op << mdio_op_mode_shift) |
	    (phy_addr & mdio_phy_address_msk));

	return (aq_mdio_wait(hw));
}

/*
 * The word accessors report whether the controller completed the transaction,
 * not whether a PHY answered: an absent PHY completes and reads all-ones.
 * Both require the MDIO semaphore to be held.
 */
static bool
aq_mdio_read_word(struct aq_hw *hw, uint16_t mmd, uint16_t addr, uint16_t *val)
{
	reg_glb_mdio_iface_set(hw, AQ_MDIO_IFACE_ADDRESS,
	    addr & mdio_address_msk);

	if (!aq_mdio_op(hw, mmd, AQ_MDIO_OP_ADDRESS) ||
	    !aq_mdio_op(hw, mmd, AQ_MDIO_OP_READ))
		return (false);

	*val = (uint16_t)reg_glb_mdio_iface_get(hw, AQ_MDIO_IFACE_RDATA);

	return (true);
}

static bool
aq_mdio_write_word(struct aq_hw *hw, uint16_t mmd, uint16_t addr, uint16_t data)
{
	reg_glb_mdio_iface_set(hw, AQ_MDIO_IFACE_ADDRESS,
	    addr & mdio_address_msk);

	if (!aq_mdio_op(hw, mmd, AQ_MDIO_OP_ADDRESS))
		return (false);

	reg_glb_mdio_iface_set(hw, AQ_MDIO_IFACE_WDATA,
	    data & mdio_write_data_msk);

	return (aq_mdio_op(hw, mmd, AQ_MDIO_OP_WRITE));
}

/* Reading the semaphore acquires it; writing 1 releases it. */
static bool
aq_phy_sem_acquire(struct aq_hw *hw)
{
	return (AQ_HW_WAIT_FOR(reg_glb_cpu_sem_get(hw, AQ_HW_FW_SM_MDIO) == 1U,
	    AQ_PHY_WAIT_US, AQ_PHY_WAIT_COUNT) == 0);
}

static void
aq_phy_sem_release(struct aq_hw *hw)
{
	reg_glb_cpu_sem_set(hw, 1U, AQ_HW_FW_SM_MDIO);
}

static bool
aq_phy_read_reg(struct aq_hw *hw, uint16_t mmd, uint16_t addr, uint16_t *val)
{
	bool ok;

	if (!aq_phy_sem_acquire(hw))
		return (false);

	ok = aq_mdio_read_word(hw, mmd, addr, val);
	aq_phy_sem_release(hw);

	return (ok);
}

/* One semaphore hold for the read-modify-write, so firmware cannot race it. */
static bool
aq_phy_clear_bits(struct aq_hw *hw, uint16_t mmd, uint16_t addr, uint16_t mask)
{
	uint16_t val;
	bool ok;

	if (!aq_phy_sem_acquire(hw))
		return (false);

	ok = aq_mdio_read_word(hw, mmd, addr, &val) &&
	    aq_mdio_write_word(hw, mmd, addr, (uint16_t)(val & ~mask));
	aq_phy_sem_release(hw);

	return (ok);
}

/*
 * A failed transaction means the MDIO interface is unusable, so give up
 * rather than time out once per address.
 */
static bool
aq_phy_find(struct aq_hw *hw)
{
	uint16_t val;

	for (hw->phy_id = 0; hw->phy_id < AQ_PHY_ID_MAX; hw->phy_id++) {
		if (!aq_phy_read_reg(hw, AQ_MMD_PMAPMD,
		    AQ_MMD_PMAPMD_DEVID_LSW, &val))
			break;
		if (val != AQ_PHY_NO_RESPONSE)
			return (true);
	}

	hw->phy_id = AQ_PHY_ID_MAX;

	return (false);
}

bool
aq_phy_init(struct aq_hw *hw)
{
	uint16_t lsw, msw;

	if (!aq_phy_find(hw))
		return (false);

	if (aq_phy_read_reg(hw, AQ_MMD_PMAPMD, AQ_MMD_PMAPMD_DEVID_MSW, &msw) &&
	    aq_phy_read_reg(hw, AQ_MMD_PMAPMD, AQ_MMD_PMAPMD_DEVID_LSW, &lsw) &&
	    (msw != AQ_PHY_NO_RESPONSE || lsw != AQ_PHY_NO_RESPONSE))
		return (true);

	hw->phy_id = AQ_PHY_ID_MAX;

	return (false);
}

void
aq_phy_disable_ptp(struct aq_hw *hw)
{
	u_int i;

	for (i = 0; i < nitems(aq_phy_ptp_regs); i++) {
		if (!aq_phy_clear_bits(hw, AQ_MMD_VEND1, aq_phy_ptp_regs[i],
		    AQ_PHY_PTP_EN))
			device_printf(hw->dev,
			    "failed to disable PHY PTP block at 1e.%04x\n",
			    aq_phy_ptp_regs[i]);
	}
}

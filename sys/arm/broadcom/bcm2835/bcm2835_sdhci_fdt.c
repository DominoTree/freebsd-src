/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * All rights reserved.
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
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcreg.h>
#include <dev/mmc/mmc_fdt_helpers.h>

#include <dev/regulator/regulator.h>

#include <dev/sdhci/sdhci.h>

#include "mmcbr_if.h"
#include "sdhci_if.h"

#include "opt_mmccam.h"

#include <arm/broadcom/bcm2835/bcm2835_dma.h>
#include <arm/broadcom/bcm2835/bcm2835_mbox_prop.h>
#ifdef NOTYET
#include <arm/broadcom/bcm2835/bcm2835_clkman.h>
#endif
#include <arm/broadcom/bcm2835/bcm2835_sdhci.h>
#include <arm/broadcom/bcm2835/bcm2835_vcbus.h>

static struct ofw_compat_data compat_data[] = {
	{"broadcom,bcm2835-sdhci",	(uintptr_t)&bcm2835_sdhci_conf},
	{"brcm,bcm2835-sdhci",		(uintptr_t)&bcm2835_sdhci_conf},
	{"brcm,bcm2835-mmc",		(uintptr_t)&bcm2835_sdhci_conf},
	{"brcm,bcm2711-emmc2",		(uintptr_t)&bcm2838_emmc2_conf},
	{"brcm,bcm2838-emmc2",		(uintptr_t)&bcm2838_emmc2_conf},
	{NULL,				0}
};

static void bcm_sdhci_dma_intr(int ch, void *arg);
static void bcm_sdhci_start_dma(struct sdhci_slot *slot);

static void
bcm_sdhci_dmacb(void *arg, bus_dma_segment_t *segs, int nseg, int err)
{
	struct bcm_sdhci_softc *sc = arg;
	int i;

	/* Sanity check: we can only ever have one mapping at a time. */
	KASSERT(sc->dmamap_seg_count == 0, ("leaked DMA segment"));
	sc->dmamap_status = err;
	sc->dmamap_seg_count = nseg;

	/* Note nseg is guaranteed to be zero if err is non-zero. */
	for (i = 0; i < nseg; i++) {
		sc->dmamap_seg_addrs[i] = segs[i].ds_addr;
		sc->dmamap_seg_sizes[i] = segs[i].ds_len;
	}
}

static int
bcm_sdhci_fdt_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Broadcom 2708 SDHCI controller");

	return (BUS_PROBE_DEFAULT);
}

static int
bcm_sdhci_fdt_attach(device_t dev)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	int err;
	phandle_t node;
	pcell_t cell;
	u_int default_freq;

	sc->conf = (struct bcm_mmc_conf *)ofw_bus_search_compatible(dev,
	    compat_data)->ocd_data;
	if (sc->conf == 0)
	    return (ENXIO);

	err = bcm2835_mbox_set_power_state(BCM2835_MBOX_POWER_ID_EMMC, TRUE);
	if (err != 0) {
		if (bootverbose)
			device_printf(dev, "Unable to enable the power\n");
		return (err);
	}

	default_freq = 0;
	err = bcm2835_mbox_get_clock_rate(sc->conf->clock_id, &default_freq);
	if (err == 0) {
		/* Convert to MHz */
		default_freq /= 1000000;
	}
	if (default_freq == 0) {
		node = ofw_bus_get_node(dev);
		if ((OF_getencprop(node, "clock-frequency", &cell,
		    sizeof(cell))) > 0)
			default_freq = cell / 1000000;
	}

#ifdef NOTYET
	if (sc->conf->clock_src > 0) {
		uint32_t f;
		sc->clkman = devclass_get_device(
		    devclass_find("bcm2835_clkman"), 0);
		if (sc->clkman == NULL) {
			device_printf(dev, "cannot find Clock Manager\n");
			return (ENXIO);
		}

		f = bcm2835_clkman_set_frequency(sc->clkman,
		    sc->conf->clock_src, default_freq);
		if (f == 0)
			return (EINVAL);

		if (bootverbose)
			device_printf(dev, "Clock source frequency: %dMHz\n",
			    f);
	}
#endif

	sc->sc_platform_dma = true;

	err = bcm_sdhci_attach_slot(dev, default_freq);
	if (err != 0)
		return (err);

	mmc_fdt_parse(dev, 0, &sc->sc_mmc_helper, &sc->sc_slot.host);

	sc->sc_dma_ch = bcm_dma_allocate(BCM_DMA_CH_ANY);
	if (sc->sc_dma_ch == BCM_DMA_CH_INVALID) {
		err = ENXIO;
		goto fail;
	}

	err = bcm_dma_setup_intr(sc->sc_dma_ch, bcm_sdhci_dma_intr, sc);
	if (err != 0) {
		device_printf(dev,
		    "cannot setup dma interrupt handler\n");
		err = ENXIO;
		goto fail;
	}

	/* Allocate bus_dma resources. */
	err = bus_dma_tag_create(bus_get_dma_tag(dev),
	    1, 0, bcm283x_dmabus_peripheral_lowaddr(),
	    BUS_SPACE_MAXADDR, NULL, NULL,
	    BCM_DMA_MAXSIZE, ALLOCATED_DMA_SEGS, BCM_SDHCI_BUFFER_SIZE,
	    BUS_DMA_ALLOCNOW, NULL, NULL,
	    &sc->sc_dma_tag);

	if (err) {
		device_printf(dev, "failed allocate DMA tag");
		goto fail;
	}

	err = bus_dmamap_create(sc->sc_dma_tag, 0, &sc->sc_dma_map);
	if (err) {
		device_printf(dev, "bus_dmamap_create failed\n");
		goto fail;
	}

	bcm_sdhci_attach_done(dev);

	return (0);

fail:
	bcm_dma_free(sc->sc_dma_ch);
	bcm_sdhci_release_resources(dev);

	return (err);
}

static int
bcm_sdhci_fdt_update_ios(device_t bus, device_t child)
{
	struct bcm_sdhci_softc *sc;
	struct mmc_ios *ios;
	int rv;

	sc = device_get_softc(bus);
	ios = &sc->sc_slot.host.ios;

	if (ios->power_mode == power_up) {
		if (sc->sc_mmc_helper.vmmc_supply)
			regulator_enable(sc->sc_mmc_helper.vmmc_supply);
		if (sc->sc_mmc_helper.vqmmc_supply)
			regulator_enable(sc->sc_mmc_helper.vqmmc_supply);
	}

	rv = sdhci_generic_update_ios(bus, child);
	if (rv != 0)
		return (rv);

	if (ios->power_mode == power_off) {
		if (sc->sc_mmc_helper.vmmc_supply)
			regulator_disable(sc->sc_mmc_helper.vmmc_supply);
		if (sc->sc_mmc_helper.vqmmc_supply)
			regulator_disable(sc->sc_mmc_helper.vqmmc_supply);
	}

	return (0);
}

static void
bcm_sdhci_start_dma_seg(struct bcm_sdhci_softc *sc)
{
	struct sdhci_slot *slot;
	vm_paddr_t pdst, psrc;
	int err __diagused, idx, len, sync_op, width;

	slot = &sc->sc_slot;
	mtx_assert(&slot->mtx, MA_OWNED);
	idx = sc->dmamap_seg_index++;
	len = sc->dmamap_seg_sizes[idx];
	slot->offset += len;
	width = (len & 0xf ? BCM_DMA_32BIT : BCM_DMA_128BIT);

	if (slot->curcmd->data->flags & MMC_DATA_READ) {
		/*
		 * Peripherals on the AXI bus do not need DREQ pacing for reads
		 * from the ARM core, so we can safely set this to NONE.
		 */
		bcm_dma_setup_src(sc->sc_dma_ch, BCM_DMA_DREQ_NONE,
		    BCM_DMA_SAME_ADDR, BCM_DMA_32BIT);
		bcm_dma_setup_dst(sc->sc_dma_ch, BCM_DMA_DREQ_NONE,
		    BCM_DMA_INC_ADDR, width);
		psrc = sc->sc_sdhci_buffer_phys;
		pdst = sc->dmamap_seg_addrs[idx];
		sync_op = BUS_DMASYNC_PREREAD;
	} else {
		/*
		 * The ordering here is important, because the last write to
		 * dst/src in the dma control block writes the real dreq value.
		 */
		bcm_dma_setup_src(sc->sc_dma_ch, BCM_DMA_DREQ_NONE,
		    BCM_DMA_INC_ADDR, width);
		bcm_dma_setup_dst(sc->sc_dma_ch, sc->conf->emmc_dreq,
		    BCM_DMA_SAME_ADDR, BCM_DMA_32BIT);
		psrc = sc->dmamap_seg_addrs[idx];
		pdst = sc->sc_sdhci_buffer_phys;
		sync_op = BUS_DMASYNC_PREWRITE;
	}

	/*
	 * When starting a new DMA operation do the busdma sync operation, and
	 * disable SDCHI data interrrupts because we'll be driven by DMA
	 * interrupts (or SDHCI error interrupts) until the IO is done.
	 */
	if (idx == 0) {
		bus_dmamap_sync(sc->sc_dma_tag, sc->sc_dma_map, sync_op);

		slot->intmask &= ~DATA_XFER_MASK;
		bcm_sdhci_write_4(sc->sc_dev, slot, SDHCI_SIGNAL_ENABLE,
		    slot->intmask);
	}

	/*
	 * Start the DMA transfer.  Only programming errors (like failing to
	 * allocate a channel) cause a non-zero return from bcm_dma_start().
	 */
	err = bcm_dma_start(sc->sc_dma_ch, psrc, pdst, len);
	KASSERT((err == 0), ("bcm2835_sdhci: failed DMA start"));
}

static void
bcm_sdhci_dma_exit(struct bcm_sdhci_softc *sc)
{
	struct sdhci_slot *slot = &sc->sc_slot;

	mtx_assert(&slot->mtx, MA_OWNED);

	/* Re-enable interrupts */
	slot->intmask |= DATA_XFER_MASK;
	bcm_sdhci_write_4(slot->bus, slot, SDHCI_SIGNAL_ENABLE,
	    slot->intmask);
}

static void
bcm_sdhci_dma_unload(struct bcm_sdhci_softc *sc)
{
	struct sdhci_slot *slot = &sc->sc_slot;

	if (sc->dmamap_seg_count == 0)
		return;
	if ((slot->curcmd->data->flags & MMC_DATA_READ) != 0)
		bus_dmamap_sync(sc->sc_dma_tag, sc->sc_dma_map,
		    BUS_DMASYNC_POSTREAD);
	else
		bus_dmamap_sync(sc->sc_dma_tag, sc->sc_dma_map,
		    BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->sc_dma_tag, sc->sc_dma_map);

	sc->dmamap_seg_count = 0;
	sc->dmamap_seg_index = 0;
}

static void
bcm_sdhci_dma_intr(int ch, void *arg)
{
	struct bcm_sdhci_softc *sc = (struct bcm_sdhci_softc *)arg;
	struct sdhci_slot *slot = &sc->sc_slot;
	uint32_t reg;

	mtx_lock(&slot->mtx);
	if (slot->curcmd == NULL)
		goto out;
	/*
	 * If there are more segments for the current dma, start the next one.
	 * Otherwise unload the dma map and decide what to do next based on the
	 * status of the sdhci controller and whether there's more data left.
	 */
	if (sc->dmamap_seg_index < sc->dmamap_seg_count) {
		bcm_sdhci_start_dma_seg(sc);
		goto out;
	}

	bcm_sdhci_dma_unload(sc);

	/*
	 * If we had no further segments pending, we need to determine how to
	 * proceed next.  If the 'data/space pending' bit is already set and we
	 * can continue via DMA, do so.  Otherwise, re-enable interrupts and
	 * return.
	 */
	reg = bcm_sdhci_read_4(slot->bus, slot, SDHCI_INT_STATUS) &
	    DATA_XFER_MASK;
	if ((reg & DATA_PENDING_MASK) != 0 &&
	    BCM_SDHCI_SEGSZ_LEFT(slot) >= BCM_SDHCI_BUFFER_SIZE) {
		/* ACK any pending interrupts */
		bcm_sdhci_write_4(slot->bus, slot, SDHCI_INT_STATUS,
		    DATA_PENDING_MASK);

		bcm_sdhci_start_dma(slot);
		if (slot->curcmd->error != 0) {
			/* We won't recover from this error for this command. */
			bcm_sdhci_dma_unload(sc);
			bcm_sdhci_dma_exit(sc);
			sdhci_finish_data(slot);
		}
	} else if ((reg & SDHCI_INT_DATA_END) != 0) {
		bcm_sdhci_dma_exit(sc);
		bcm_sdhci_write_4(slot->bus, slot, SDHCI_INT_STATUS,
		    reg);
		slot->flags &= ~PLATFORM_DATA_STARTED;
		sdhci_finish_data(slot);
	} else {
		bcm_sdhci_dma_exit(sc);
	}
out:
	mtx_unlock(&slot->mtx);
}

static void
bcm_sdhci_start_dma(struct sdhci_slot *slot)
{
	struct bcm_sdhci_softc *sc = device_get_softc(slot->bus);
	uint8_t *buf;
	size_t left;

	mtx_assert(&slot->mtx, MA_OWNED);

	left = BCM_SDHCI_SEGSZ_LEFT(slot);
	buf = (uint8_t *)slot->curcmd->data->data + slot->offset;
	KASSERT(left != 0,
	    ("%s: DMA handling incorrectly indicated", __func__));

	/*
	 * No need to check segment count here; if we've not yet unloaded
	 * previous segments, we'll catch that in bcm_sdhci_dmacb.
	 */
	if (bus_dmamap_load(sc->sc_dma_tag, sc->sc_dma_map, buf, left,
	    bcm_sdhci_dmacb, sc, BUS_DMA_NOWAIT) != 0 ||
	    sc->dmamap_status != 0) {
		slot->curcmd->error = MMC_ERR_NO_MEMORY;
		return;
	}

	/* DMA start */
	bcm_sdhci_start_dma_seg(sc);
}

static int
bcm_sdhci_will_handle_transfer(device_t dev, struct sdhci_slot *slot)
{
#ifdef INVARIANTS
	struct bcm_sdhci_softc *sc = device_get_softc(slot->bus);
#endif

	/*
	 * We don't want to perform DMA in this context -- interrupts are
	 * disabled, and a transaction may already be in progress.
	 */
	if (dumping)
		return (0);

	/*
	 * This indicates that we somehow let a data interrupt slip by into the
	 * SDHCI framework, when it should not have.  This really needs to be
	 * caught and fixed ASAP, as it really shouldn't happen.
	 */
	KASSERT(sc->dmamap_seg_count == 0,
	    ("data pending interrupt pushed through SDHCI framework"));

	/*
	 * Do not use DMA for transfers less than our block size.  Checking
	 * alignment serves little benefit, as we round transfer sizes down to
	 * a multiple of the block size and push the transfer back to
	 * SDHCI-driven PIO once we're below the block size.
	 */
	if (BCM_SDHCI_SEGSZ_LEFT(slot) < BCM_DMA_BLOCK_SIZE)
		return (0);

	return (1);
}

static void
bcm_sdhci_start_transfer(device_t dev, struct sdhci_slot *slot,
    uint32_t *intmask)
{

	/* DMA transfer FIFO 1KB */
	bcm_sdhci_start_dma(slot);
}

static void
bcm_sdhci_finish_transfer(device_t dev, struct sdhci_slot *slot)
{
	struct bcm_sdhci_softc *sc = device_get_softc(slot->bus);

	/*
	 * Clean up.  Interrupts are clearly enabled, because we received an
	 * SDHCI_INT_DATA_END to get this far -- just make sure we don't leave
	 * anything laying around.
	 */
	if (sc->dmamap_seg_count != 0) {
		/*
		 * Our segment math should have worked out such that we would
		 * never finish the transfer without having used up all of the
		 * segments.  If we haven't, that means we must have erroneously
		 * regressed to SDHCI-driven PIO to finish the operation and
		 * this is certainly caused by developer-error.
		 */
		bcm_sdhci_dma_unload(sc);
	}

	sdhci_finish_data(slot);
}

static device_method_t bcm_sdhci_fdt_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		bcm_sdhci_fdt_probe),
	DEVMETHOD(device_attach,	bcm_sdhci_fdt_attach),

	/* MMC bridge interface */
	DEVMETHOD(mmcbr_update_ios,	bcm_sdhci_fdt_update_ios),

	/* Platform transfer methods */
	DEVMETHOD(sdhci_platform_will_handle,		bcm_sdhci_will_handle_transfer),
	DEVMETHOD(sdhci_platform_start_transfer,	bcm_sdhci_start_transfer),
	DEVMETHOD(sdhci_platform_finish_transfer,	bcm_sdhci_finish_transfer),

	DEVMETHOD_END
};

DEFINE_CLASS_1(sdhci_bcm, bcm_sdhci_fdt_driver, bcm_sdhci_fdt_methods,
    sizeof(struct bcm_sdhci_softc), bcm_sdhci_driver);

DRIVER_MODULE(sdhci_bcm, simplebus, bcm_sdhci_fdt_driver, NULL, NULL);
#ifdef NOTYET
MODULE_DEPEND(sdhci_bcm, bcm2835_clkman, 1, 1, 1);
#endif
#ifndef MMCCAM
MMC_DECLARE_BRIDGE(sdhci_bcm_fdt);
#endif

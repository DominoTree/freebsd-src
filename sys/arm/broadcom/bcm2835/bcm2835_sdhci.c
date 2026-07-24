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
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcreg.h>
#include <dev/mmc/mmc_helpers.h>

#include <dev/sdhci/sdhci.h>

#include "mmcbr_if.h"
#include "sdhci_if.h"

#include "opt_mmccam.h"

#include <arm/broadcom/bcm2835/bcm2835_dma.h>
#include <arm/broadcom/bcm2835/bcm2835_mbox_prop.h>
#include <arm/broadcom/bcm2835/bcm2835_sdhci.h>

#ifdef DEBUG
static int bcm2835_sdhci_debug = 0;

TUNABLE_INT("hw.bcm2835.sdhci.debug", &bcm2835_sdhci_debug);
SYSCTL_INT(_hw_sdhci, OID_AUTO, bcm2835_sdhci_debug, CTLFLAG_RWTUN,
    &bcm2835_sdhci_debug, 0, "bcm2835 SDHCI debug level");

#define	dprintf(fmt, args...)					\
	do {							\
		if (bcm2835_sdhci_debug)			\
			printf("%s: " fmt, __func__, ##args);	\
	}  while (0)
#else
#define dprintf(fmt, args...)
#endif

static int bcm2835_sdhci_hs = 1;
int bcm2835_sdhci_pio_mode = 0;

struct bcm_mmc_conf bcm2835_sdhci_conf = {
	.clock_id	= BCM2835_MBOX_CLOCK_ID_EMMC,
	.clock_src	= -1,
	.default_freq	= BCM2835_DEFAULT_SDHCI_FREQ,
	.quirks		= SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
	    SDHCI_QUIRK_BROKEN_TIMEOUT_VAL | SDHCI_QUIRK_DONT_SET_HISPD_BIT |
	    SDHCI_QUIRK_MISSING_CAPS,
	.emmc_dreq	= BCM_DMA_DREQ_EMMC,
};

struct bcm_mmc_conf bcm2838_emmc2_conf = {
	.clock_id	= BCM2838_MBOX_CLOCK_ID_EMMC2,
	.clock_src	= -1,
	.default_freq	= BCM2838_DEFAULT_SDHCI_FREQ,
	.quirks		= 0,
	.emmc_dreq	= BCM_DMA_DREQ_NONE,
};

TUNABLE_INT("hw.bcm2835.sdhci.hs", &bcm2835_sdhci_hs);
TUNABLE_INT("hw.bcm2835.sdhci.pio_mode", &bcm2835_sdhci_pio_mode);

static void bcm_sdhci_intr(void *);
static int bcm_sdhci_get_ro(device_t, device_t);

int
bcm_sdhci_attach_slot(device_t dev, u_int default_freq)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	int rid, err;

	sc->sc_dev = dev;
	sc->sc_req = NULL;

	if (default_freq == 0)
		default_freq = sc->conf->default_freq;

	if (bootverbose)
		device_printf(dev, "SDHCI frequency: %dMHz\n", default_freq);

	rid = 0;
	sc->sc_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (!sc->sc_mem_res) {
		device_printf(dev, "cannot allocate memory window\n");
		err = ENXIO;
		goto fail;
	}

	sc->sc_bst = rman_get_bustag(sc->sc_mem_res);
	sc->sc_bsh = rman_get_bushandle(sc->sc_mem_res);

	rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (!sc->sc_irq_res) {
		device_printf(dev, "cannot allocate interrupt\n");
		err = ENXIO;
		goto fail;
	}

	if (bus_setup_intr(dev, sc->sc_irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, bcm_sdhci_intr, sc, &sc->sc_intrhand)) {
		device_printf(dev, "cannot setup interrupt handler\n");
		err = ENXIO;
		goto fail;
	}

	if (sc->sc_platform_dma && !bcm2835_sdhci_pio_mode)
		sc->sc_slot.opt = SDHCI_PLATFORM_TRANSFER;

	sc->sc_slot.caps = SDHCI_CAN_VDD_330 | SDHCI_CAN_VDD_180;
	if (bcm2835_sdhci_hs)
		sc->sc_slot.caps |= SDHCI_CAN_DO_HISPD;
	sc->sc_slot.caps |= (default_freq << SDHCI_CLOCK_BASE_SHIFT);
	sc->sc_slot.quirks = sc->conf->quirks;

	sdhci_init_slot(dev, &sc->sc_slot, 0);

	/* FIXME: Fix along with other BUS_SPACE_PHYSADDR instances */
	sc->sc_sdhci_buffer_phys = rman_get_start(sc->sc_mem_res) +
	    SDHCI_BUFFER;

	return (0);

fail:
	bcm_sdhci_release_resources(dev);

	return (err);
}

void
bcm_sdhci_attach_done(device_t dev)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);

	bus_identify_children(dev);
	bus_attach_children(dev);

	sdhci_start_slot(&sc->sc_slot);

	/* Seed our copies. */
	sc->blksz_and_count = SDHCI_READ_4(dev, &sc->sc_slot, SDHCI_BLOCK_SIZE);
	sc->cmd_and_mode = SDHCI_READ_4(dev, &sc->sc_slot, SDHCI_TRANSFER_MODE);
}

void
bcm_sdhci_release_resources(device_t dev)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);

	if (sc->sc_intrhand != NULL) {
		bus_teardown_intr(dev, sc->sc_irq_res, sc->sc_intrhand);
		sc->sc_intrhand = NULL;
	}
	if (sc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->sc_irq_res);
		sc->sc_irq_res = NULL;
	}
	if (sc->sc_mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->sc_mem_res);
		sc->sc_mem_res = NULL;
	}
}

int
bcm_sdhci_detach(device_t dev)
{

	return (EBUSY);
}

static void
bcm_sdhci_intr(void *arg)
{
	struct bcm_sdhci_softc *sc = arg;

	sdhci_generic_intr(&sc->sc_slot);
}

static int
bcm_sdhci_get_ro(device_t bus, device_t child)
{

	return (0);
}

static inline uint32_t
RD4(struct bcm_sdhci_softc *sc, bus_size_t off)
{
	uint32_t val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, off);
	return val;
}

static inline void
WR4(struct bcm_sdhci_softc *sc, bus_size_t off, uint32_t val)
{

	bus_space_write_4(sc->sc_bst, sc->sc_bsh, off, val);
	/*
	 * The Arasan HC has a bug where it may lose the content of
	 * consecutive writes to registers that are within two SD-card
	 * clock cycles of each other (a clock domain crossing problem).
	 */
	if (sc->sc_slot.clock > 0)
		DELAY(((2 * 1000000) / sc->sc_slot.clock) + 1);
}

static uint8_t
bcm_sdhci_read_1(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	uint32_t val = RD4(sc, off & ~3);

	return ((val >> (off & 3)*8) & 0xff);
}

static uint16_t
bcm_sdhci_read_2(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	uint32_t val32;

	/*
	 * Standard 32-bit handling of command and transfer mode, as
	 * well as block size and count.
	 */
	if ((off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) &&
	    sc->need_update_blk)
		val32 = sc->blksz_and_count;
	else if (off == SDHCI_TRANSFER_MODE || off == SDHCI_COMMAND_FLAGS)
		val32 = sc->cmd_and_mode;
	else
		val32 = RD4(sc, off & ~3);

	return ((val32 >> (off & 3)*8) & 0xffff);
}

uint32_t
bcm_sdhci_read_4(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);

	return RD4(sc, off);
}

static void
bcm_sdhci_read_multi_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t *data, bus_size_t count)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);

	bus_space_read_multi_4(sc->sc_bst, sc->sc_bsh, off, data, count);
}

static void
bcm_sdhci_write_1(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint8_t val)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	uint32_t val32 = RD4(sc, off & ~3);
	val32 &= ~(0xff << (off & 3)*8);
	val32 |= (val << (off & 3)*8);
	WR4(sc, off & ~3, val32);
}

static void
bcm_sdhci_write_2(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint16_t val)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	uint32_t val32;

	/*
	 * If we have a queued up 16bit value for blk size or count, use and
	 * update the saved value rather than doing any real register access.
	 * If we did not touch either since the last write, then read from
	 * register as at least block count can change.
	 * Similarly, if we are about to issue a command, always use the saved
	 * value for transfer mode as we can never write that without issuing
	 * a command.
	 */
	if ((off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) &&
	    sc->need_update_blk)
		val32 = sc->blksz_and_count;
	else if (off == SDHCI_COMMAND_FLAGS)
		val32 = sc->cmd_and_mode;
	else
		val32 = RD4(sc, off & ~3);

	val32 &= ~(0xffff << (off & 3)*8);
	val32 |= (val << (off & 3)*8);

	if (off == SDHCI_TRANSFER_MODE)
		sc->cmd_and_mode = val32;
	else if (off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) {
		sc->blksz_and_count = val32;
		sc->need_update_blk = true;
	} else {
		if (off == SDHCI_COMMAND_FLAGS) {
			/* If we saved blk writes, do them now before cmd. */
			if (sc->need_update_blk) {
				WR4(sc, SDHCI_BLOCK_SIZE, sc->blksz_and_count);
				sc->need_update_blk = false;
			}
			/* Always save cmd and mode registers. */
			sc->cmd_and_mode = val32;
		}
		WR4(sc, off & ~3, val32);
	}
}

void
bcm_sdhci_write_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t val)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	WR4(sc, off, val);
}

static void
bcm_sdhci_write_multi_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t *data, bus_size_t count)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);

	bus_space_write_multi_4(sc->sc_bst, sc->sc_bsh, off, data, count);
}

static device_method_t bcm_sdhci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_detach,	bcm_sdhci_detach),

	/* Bus interface */
	DEVMETHOD(bus_read_ivar,	sdhci_generic_read_ivar),
	DEVMETHOD(bus_write_ivar,	sdhci_generic_write_ivar),
	DEVMETHOD(bus_add_child,	bus_generic_add_child),

	/* MMC bridge interface */
	DEVMETHOD(mmcbr_update_ios,	sdhci_generic_update_ios),
	DEVMETHOD(mmcbr_request,	sdhci_generic_request),
	DEVMETHOD(mmcbr_get_ro,		bcm_sdhci_get_ro),
	DEVMETHOD(mmcbr_acquire_host,	sdhci_generic_acquire_host),
	DEVMETHOD(mmcbr_release_host,	sdhci_generic_release_host),

	/* SDHCI registers accessors */
	DEVMETHOD(sdhci_read_1,		bcm_sdhci_read_1),
	DEVMETHOD(sdhci_read_2,		bcm_sdhci_read_2),
	DEVMETHOD(sdhci_read_4,		bcm_sdhci_read_4),
	DEVMETHOD(sdhci_read_multi_4,	bcm_sdhci_read_multi_4),
	DEVMETHOD(sdhci_write_1,	bcm_sdhci_write_1),
	DEVMETHOD(sdhci_write_2,	bcm_sdhci_write_2),
	DEVMETHOD(sdhci_write_4,	bcm_sdhci_write_4),
	DEVMETHOD(sdhci_write_multi_4,	bcm_sdhci_write_multi_4),

	DEVMETHOD_END
};

DEFINE_CLASS_0(sdhci_bcm, bcm_sdhci_driver, bcm_sdhci_methods,
    sizeof(struct bcm_sdhci_softc));

SDHCI_DEPEND(sdhci_bcm);

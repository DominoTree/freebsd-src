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
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcreg.h>
#include <dev/mmc/mmc_helpers.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <dev/acpica/acpivar.h>

#include <dev/sdhci/sdhci.h>

#include "mmcbr_if.h"
#include "sdhci_if.h"

#include "opt_mmccam.h"

#include <arm/broadcom/bcm2835/bcm2835_sdhci.h>

static char *bcm_sdhci_hids[] = {
	"BCM2847",
	NULL
};

static int
bcm_sdhci_acpi_probe(device_t dev)
{
	device_t bus;
	int err;

	bus = device_get_parent(dev);

	err = ACPI_ID_PROBE(bus, dev, bcm_sdhci_hids, NULL);
	if (err <= 0) {
		device_set_desc(dev, "Broadcom 2708 SDHCI controller");
		return (err);
	}

	return (ENXIO);
}

static int
bcm_sdhci_acpi_attach(device_t dev)
{
	struct bcm_sdhci_softc *sc = device_get_softc(dev);
	uint32_t freq;
	int err;

	sc->conf = &bcm2835_sdhci_conf;

	/*
	 * The VideoCore mailbox and the DMA controller are only reachable
	 * through the device tree, so neither the firmware clock query nor
	 * platform DMA is available here.
	 */
	sc->sc_platform_dma = false;

	if (device_get_property(dev, "clock-frequency", &freq, sizeof(freq),
	    DEVICE_PROP_UINT32) <= 0)
		freq = 0;
	else
		freq /= 1000000;

	err = bcm_sdhci_attach_slot(dev, freq);
	if (err != 0)
		return (err);

	if (mmc_parse(dev, &sc->sc_mmc_helper, &sc->sc_slot.host) != 0) {
		bcm_sdhci_release_resources(dev);
		return (ENXIO);
	}

	if (sc->sc_mmc_helper.props & MMC_PROP_NON_REMOVABLE) {
		sc->sc_slot.opt |= SDHCI_NON_REMOVABLE;
		if (bootverbose)
			device_printf(dev, "Non-removable media\n");
	}

	bcm_sdhci_attach_done(dev);

	return (0);
}

static device_method_t bcm_sdhci_acpi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		bcm_sdhci_acpi_probe),
	DEVMETHOD(device_attach,	bcm_sdhci_acpi_attach),

	DEVMETHOD(sdhci_get_card_present, sdhci_generic_get_card_present),

	DEVMETHOD_END
};

DEFINE_CLASS_1(sdhci_bcm, bcm_sdhci_acpi_driver, bcm_sdhci_acpi_methods,
    sizeof(struct bcm_sdhci_softc), bcm_sdhci_driver);

DRIVER_MODULE(sdhci_bcm, acpi, bcm_sdhci_acpi_driver, NULL, NULL);
#ifndef MMCCAM
MMC_DECLARE_BRIDGE(sdhci_bcm_acpi);
#endif

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Nick Price
 */

/*
 * MSI controller (MIP) for the Raspberry Pi 5 external PCIe slot under
 * EDK2 ACPI, which describes neither the MIP nor its inbound window.
 * Both are taken from the loader-supplied DTB.
 */

#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <machine/bus.h>
#include <machine/machdep.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "gic_if.h"
#include "msi_if.h"

#define	MIP_INT_CFGL_HOST	0x20
#define	MIP_INT_CFGH_HOST	0x30
#define	MIP_INT_MASKL_HOST	0x40
#define	MIP_INT_MASKH_HOST	0x50
#define	MIP_INT_MASKL_VPU	0x60
#define	MIP_INT_MASKH_VPU	0x70
#define	MIP_MAX_VECTORS		64

#define	RC_BAR4_CONFIG_LO	0x40d4
#define	RC_BAR4_CONFIG_HI	0x40d8
#define	UBUS_BAR4_REMAP_LO	0x410c
#define	UBUS_BAR4_REMAP_HI	0x4110
#define	RC_BAR_SIZE_MASK	0x1f
#define	RC_BAR_SIZE_4K		0x1c
#define	UBUS_REMAP_EN		0x1

#define	GIC_SPI_BASE		32

struct bcm2712_mip_softc {
	u_int			spi_start;	/* GIC INTID */
	u_int			spi_count;
	u_int			msi_offset;
	uint64_t		msi_addr;	/* PCI address of the MIP */
	struct intr_irqsrc	*isrcs[MIP_MAX_VECTORS];
};

static void
bcm2712_mip_identify(driver_t *driver, device_t parent)
{
	phandle_t root;

	if (arm64_bus_method != ARM64_BUS_ACPI)
		return;
	if (device_find_child(parent, "bcm2712_mip", DEVICE_UNIT_ANY) != NULL)
		return;
	root = OF_finddevice("/");
	if (root == -1 || !ofw_bus_node_is_compatible(root, "brcm,bcm2712"))
		return;

	BUS_ADD_CHILD(parent, 0, "bcm2712_mip", DEVICE_UNIT_ANY);
}

static int
bcm2712_mip_probe(device_t dev)
{
	device_set_desc(dev, "BCM2712 PCIe MSI controller");
	/* gic_acpi cannot print a child it has no devinfo for. */
	device_quiet(dev);
	return (BUS_PROBE_NOWILDCARD);
}

static int
bcm2712_mip_parse(device_t dev, phandle_t mip, uint64_t *mip_pa)
{
	struct bcm2712_mip_softc *sc;
	pcell_t ranges[5], reg[8];

	sc = device_get_softc(dev);

	/* <base size> <msi-addr size>, two cells each under /axi. */
	if (OF_getencprop(mip, "reg", reg, sizeof(reg)) != sizeof(reg))
		return (ENXIO);
	*mip_pa = (uint64_t)reg[0] << 32 | reg[1];
	sc->msi_addr = (uint64_t)reg[4] << 32 | reg[5];

	if (OF_getencprop(mip, "brcm,msi-offset", &sc->msi_offset,
	    sizeof(sc->msi_offset)) <= 0)
		sc->msi_offset = 0;

	/* <&gic GIC_SPI spi type count>; MIP index N raises SPI spi + N. */
	if (OF_getencprop(mip, "msi-ranges", ranges, sizeof(ranges)) !=
	    sizeof(ranges) || ranges[1] != 0)
		return (ENXIO);
	sc->spi_start = GIC_SPI_BASE + ranges[2] + sc->msi_offset;
	sc->spi_count = ranges[4];
	if (sc->spi_count == 0 || sc->spi_count > MIP_MAX_VECTORS)
		return (ENXIO);
	return (0);
}

static int
bcm2712_mip_attach(device_t dev)
{
	struct bcm2712_mip_softc *sc;
	bus_space_tag_t bst;
	bus_space_handle_t mip_bsh, rc_bsh;
	bus_size_t mip_size, rc_size;
	phandle_t mip, pcie;
	pcell_t xref;
	uint64_t mip_pa;
	device_t gic;
	uint32_t bar;
	u_int i;
	int error;

	sc = device_get_softc(dev);
	gic = device_get_parent(dev);

	pcie = OF_finddevice("/axi/pcie@1000110000");
	if (pcie == -1 || OF_getencprop(pcie, "msi-parent", &xref,
	    sizeof(xref)) != sizeof(xref))
		return (ENXIO);
	mip = OF_node_from_xref(xref);
	if (!ofw_bus_node_is_compatible(mip, "brcm,bcm2712-mip"))
		return (ENXIO);
	error = bcm2712_mip_parse(dev, mip, &mip_pa);
	if (error != 0) {
		device_printf(dev, "cannot parse the MIP node\n");
		return (error);
	}

	if (OF_decode_addr(mip, 0, &bst, &mip_bsh, &mip_size) != 0)
		return (ENXIO);
	if (OF_decode_addr(pcie, 0, &bst, &rc_bsh, &rc_size) != 0) {
		bus_space_unmap(bst, mip_bsh, mip_size);
		return (ENXIO);
	}

	bar = bus_space_read_4(bst, rc_bsh, RC_BAR4_CONFIG_LO);
	if ((bar & RC_BAR_SIZE_MASK) != 0) {
		device_printf(dev, "inbound window 4 in use: %#x\n", bar);
		error = EBUSY;
		goto out;
	}

	bus_space_write_4(bst, mip_bsh, MIP_INT_MASKL_HOST, 0);
	bus_space_write_4(bst, mip_bsh, MIP_INT_MASKH_HOST, 0);
	bus_space_write_4(bst, mip_bsh, MIP_INT_MASKL_VPU, ~0u);
	bus_space_write_4(bst, mip_bsh, MIP_INT_MASKH_VPU, ~0u);
	bus_space_write_4(bst, mip_bsh, MIP_INT_CFGL_HOST, ~0u);
	bus_space_write_4(bst, mip_bsh, MIP_INT_CFGH_HOST, ~0u);

	bus_space_write_4(bst, rc_bsh, RC_BAR4_CONFIG_HI, sc->msi_addr >> 32);
	bus_space_write_4(bst, rc_bsh, UBUS_BAR4_REMAP_HI, mip_pa >> 32);
	bus_space_write_4(bst, rc_bsh, UBUS_BAR4_REMAP_LO,
	    (uint32_t)mip_pa | UBUS_REMAP_EN);
	bus_space_write_4(bst, rc_bsh, RC_BAR4_CONFIG_LO,
	    (uint32_t)sc->msi_addr | RC_BAR_SIZE_4K);

	GIC_RESERVE_MSI_RANGE(gic, sc->spi_start, sc->spi_count);

	/* Learn each vector's source so map_msi can find its MIP index. */
	for (i = 0; i < sc->spi_count; i++) {
		error = GIC_ALLOC_MSIX(gic, sc->spi_start + i, 1,
		    &sc->isrcs[i]);
		if (error != 0)
			goto out;
		GIC_RELEASE_MSIX(gic, sc->isrcs[i]);
	}

	error = intr_msi_register(dev, ACPI_MSI_XREF);
	if (error == 0)
		device_printf(dev, "%u vectors on SPI %u-%u\n", sc->spi_count,
		    sc->spi_start - GIC_SPI_BASE,
		    sc->spi_start - GIC_SPI_BASE + sc->spi_count - 1);
out:
	bus_space_unmap(bst, rc_bsh, rc_size);
	bus_space_unmap(bst, mip_bsh, mip_size);
	return (error);
}

static int
bcm2712_mip_detach(device_t dev)
{
	/* An MSI controller cannot be deregistered. */
	return (EBUSY);
}

static int
bcm2712_mip_alloc_msi(device_t dev, device_t child, int count, int maxcount,
    device_t *pic, struct intr_irqsrc **srcs)
{
	struct bcm2712_mip_softc *sc;
	int error;

	sc = device_get_softc(dev);
	error = GIC_ALLOC_MSI(device_get_parent(dev), sc->spi_start,
	    sc->spi_count, count, maxcount, srcs);
	if (error != 0)
		return (error);
	*pic = dev;
	return (0);
}

static int
bcm2712_mip_release_msi(device_t dev, device_t child, int count,
    struct intr_irqsrc **isrc)
{
	return (GIC_RELEASE_MSI(device_get_parent(dev), count, isrc));
}

static int
bcm2712_mip_alloc_msix(device_t dev, device_t child, device_t *pic,
    struct intr_irqsrc **isrcp)
{
	struct bcm2712_mip_softc *sc;
	int error;

	sc = device_get_softc(dev);
	error = GIC_ALLOC_MSIX(device_get_parent(dev), sc->spi_start,
	    sc->spi_count, isrcp);
	if (error != 0)
		return (error);
	*pic = dev;
	return (0);
}

static int
bcm2712_mip_release_msix(device_t dev, device_t child,
    struct intr_irqsrc *isrc)
{
	return (GIC_RELEASE_MSIX(device_get_parent(dev), isrc));
}

static int
bcm2712_mip_map_msi(device_t dev, device_t child, struct intr_irqsrc *isrc,
    uint64_t *addr, uint32_t *data)
{
	struct bcm2712_mip_softc *sc;
	u_int i;

	sc = device_get_softc(dev);
	for (i = 0; i < sc->spi_count; i++) {
		if (sc->isrcs[i] == isrc) {
			*addr = sc->msi_addr;
			*data = sc->msi_offset + i;
			return (0);
		}
	}
	return (EINVAL);
}

static device_method_t bcm2712_mip_methods[] = {
	DEVMETHOD(device_identify,	bcm2712_mip_identify),
	DEVMETHOD(device_probe,		bcm2712_mip_probe),
	DEVMETHOD(device_attach,	bcm2712_mip_attach),
	DEVMETHOD(device_detach,	bcm2712_mip_detach),

	DEVMETHOD(msi_alloc_msi,	bcm2712_mip_alloc_msi),
	DEVMETHOD(msi_release_msi,	bcm2712_mip_release_msi),
	DEVMETHOD(msi_alloc_msix,	bcm2712_mip_alloc_msix),
	DEVMETHOD(msi_release_msix,	bcm2712_mip_release_msix),
	DEVMETHOD(msi_map_msi,		bcm2712_mip_map_msi),

	DEVMETHOD_END
};

static driver_t bcm2712_mip_driver = {
	"bcm2712_mip",
	bcm2712_mip_methods,
	sizeof(struct bcm2712_mip_softc),
};

DRIVER_MODULE(bcm2712_mip, gic, bcm2712_mip_driver, NULL, NULL);
MODULE_VERSION(bcm2712_mip, 1);

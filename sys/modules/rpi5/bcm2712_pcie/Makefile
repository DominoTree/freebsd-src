.PATH: ${SRCTOP}/sys/arm64/broadcom/rpi5

KMOD=	bcm2712_pcie
SRCS=	bcm2712_pcie.c
SRCS+=	acpi_if.h bus_if.h device_if.h
SRCS+=	opt_acpi.h opt_platform.h

EXPORT_SYMS=	bcm2712_pcie_register_rp1_intr \
		bcm2712_pcie_deregister_rp1_intr \
		bcm2712_pcie_gem_iack

.include <bsd.kmod.mk>

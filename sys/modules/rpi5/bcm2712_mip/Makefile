.PATH: ${SRCTOP}/sys/arm64/broadcom/rpi5

KMOD=	bcm2712_mip
SRCS=	bcm2712_mip.c
SRCS+=	bus_if.h device_if.h gic_if.h msi_if.h ofw_bus_if.h
SRCS+=	opt_acpi.h opt_platform.h

.include <bsd.kmod.mk>

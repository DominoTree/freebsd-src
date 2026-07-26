.PATH: ${SRCTOP}/sys/arm64/broadcom/rpi5

KMOD=	rp1_eth
SRCS=	rp1_eth.c rp1_eth_cfg.c
SRCS+=	bus_if.h device_if.h
SRCS+=	opt_platform.h

.include <bsd.kmod.mk>

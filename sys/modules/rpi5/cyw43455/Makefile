.PATH: ${SRCTOP}/sys/arm64/broadcom/rpi5

KMOD=	cyw43455
SRCS=	cyw43455.c cyw43455_cfg.c cyw43455_events.c cyw43455_fw.c \
	cyw43455_fwil.c cyw43455_scan.c cyw43455_sdio.c cyw43455_sdpcm.c \
	cyw43455_security.c
SRCS+=	bus_if.h device_if.h sdio_if.h
SRCS+=	opt_platform.h opt_wlan.h

.include <bsd.kmod.mk>

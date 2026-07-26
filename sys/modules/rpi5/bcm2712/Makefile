.PATH: ${SRCTOP}/sys/arm64/broadcom/rpi5

KMOD=	bcm2712
SRCS=	bcm2712.c
SRCS+=	bus_if.h device_if.h pwmbus_if.h
SRCS+=	opt_platform.h

EXPORT_SYMS=	bcm2712_read_cpu_temp \
		bcm2712_get_softc \
		bcm2712_pwm_set_config \
		bcm2712_pwm_enable \
		bcm2712_read_fan_rpm

.include <bsd.kmod.mk>

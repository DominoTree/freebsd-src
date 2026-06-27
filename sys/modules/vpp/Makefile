.PATH: ${SRCTOP}/sys/net/vpp

KMOD=	vpp
SRCS=	vpp_main.c vpp_if.c
SRCS+=	vpp_ethernet_input.c vpp_ip4_input.c vpp_ip4_lookup.c
SRCS+=	vpp_ip4_rewrite.c vpp_ip6.c vpp_adj.c vpp_output.c vpp_punt.c
SRCS+=	opt_inet.h opt_inet6.h opt_vpp.h

CFLAGS+=	-DINET -DINET6

.include <bsd.kmod.mk>

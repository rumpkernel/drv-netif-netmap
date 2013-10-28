LIB=	rumpnet_netmapif

SRCS=	if_virt.c
SRCS+=	component.c

RUMPTOP=${TOPRUMP}

.PATH:	${RUMPTOP}/net/lib/libvirtif

CPPFLAGS+=	-I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet
CPPFLAGS+=	-I${RUMPTOP}/net/lib/libvirtif
CPPFLAGS+=	-DVIRTIF_BASE=netmap

RUMPCOMP_USER=	#defined
RUMPCOMP_USER_CPPFLAGS+= ${NETMAPINCS:D-I${NETMAPINCS}}
RUMPCOMP_USER_CPPFLAGS+= -I${RUMPTOP}/net/lib/libvirtif
RUMPCOMP_USER_CPPFLAGS+= -DVIRTIF_BASE=netmap

.include "${RUMPTOP}/Makefile.rump"
.include <bsd.lib.mk>
.include <bsd.klinks.mk>

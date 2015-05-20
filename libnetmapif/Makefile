LIB=	rumpnet_netmapif

SRCS=	if_virt.c
SRCS+=	component.c

RUMPTOP=${TOPRUMP}

.PATH:	${.CURDIR}/../libvirtif

CPPFLAGS+=	-I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet
CPPFLAGS+=	-I${.CURDIR}/../libvirtif
CPPFLAGS+=	-DVIRTIF_BASE=netmap -DRUMP_VIF_LINKSTR

RUMPCOMP_USER_SRCS=	rumpcomp_user.c
RUMPCOMP_USER_CPPFLAGS+= ${NETMAPINCS:D-I${NETMAPINCS}}
RUMPCOMP_USER_CPPFLAGS+= -I${.CURDIR}/../libvirtif
RUMPCOMP_USER_CPPFLAGS+= -DVIRTIF_BASE=netmap

.include "${RUMPTOP}/Makefile.rump"
.include <bsd.lib.mk>
.include <bsd.klinks.mk>

LIB=	rumpnet_netmapif
VIRTIF=	netmap

RUMPTOP=${TOPRUMP}

.include "${RUMPTOP}/net/lib/libvirtif/Makefile.virtif"

CPPFLAGS+=	-I${RUMPTOP}/librump/rumpkern -I${RUMPTOP}/librump/rumpnet
CPPFLAGS+=	-DRUMP_VIF_LINKSTR

#RUMPCOMP_USER=	#defined
RUMPCOMP_USER_SRCS=	netmapif_user.c
RUMPCOMP_USER_CPPFLAGS+= ${NETMAPINCS:D-I${NETMAPINCS}}
RUMPCOMP_USER_CPPFLAGS+= -Wno-unused-but-set-variable

.include "${RUMPTOP}/Makefile.rump"
.include <bsd.lib.mk>
.include <bsd.klinks.mk>

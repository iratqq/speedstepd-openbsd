PROG=	speedstepd
SRCS=	speedstepd.c

NOMAN=	YES

CPPFLAGS+=	-I${BSDSRCDIR}/usr.sbin/apmd

.include <bsd.prog.mk>

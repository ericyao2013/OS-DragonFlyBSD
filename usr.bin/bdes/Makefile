#	@(#)Makefile	8.1 (Berkeley) 6/6/93
# $FreeBSD: src/secure/usr.bin/bdes/Makefile,v 1.14 2004/02/23 20:25:27 johan Exp $

PROG=	bdes

WARNS?=	2

DPADD=		${LIBCRYPTO}
LDADD=		-lprivate_crypto
LDFLAGS+=	${PRIVATELIB_LDFLAGS}
CFLAGS+=	-I${.CURDIR}/../../crypto/libressl/include

.include <bsd.prog.mk>

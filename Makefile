PROG =		galileo

SRCS =		galileo.c config.c fcgi.c log.c parse.y proc.c proxy.c \
		xmalloc.c

MAN =		${PROG}.conf.5 ${PROG}.8

# debug
CFLAGS +=	-O0 -g3

CFLAGS +=	-I${.CURDIR}

WARNINGS =	yes

CDIAGFLAGS =	-Wall -Wextra -Wpointer-arith -Wuninitialized
CDIAGFLAGS+=	-Wstrict-prototypes -Wmissing-prototypes -Wunused
CDIAGFLAGS+=	-Wsign-compare -Wshadow -Wno-unused-parameter
CDIAGFLAGS+=	-Wno-missing-field-initializers
CDIAGFLAGS+=	-Werror

LDADD =		-levent -ltls -lutil
DPADD =		${LIBEVENT} ${LIBTLS} ${LIBUTIL}

PREFIX?=	/usr/local
SBINDIR?=	${PREFIX}/sbin
MANDIR?=	${PREFIX}/man/man

realinstall:
	${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} \
		-m ${BINMODE} ${PROG} ${SBINDIR}/${PROG}

.include <bsd.prog.mk>

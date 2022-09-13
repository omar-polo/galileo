PROG =		galileo

SRCS =		galileo.c config.c fcgi.c log.c parse.y proc.c proxy.c \
		xmalloc.c

# XXX
NOMAN =		Yes

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

.include <bsd.prog.mk>

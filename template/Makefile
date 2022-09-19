PROG =		template
SRCS =		template.c parse.y

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

.if make(regress) || make(clean)
SUBDIR = regress
.endif

.include <bsd.prog.mk>

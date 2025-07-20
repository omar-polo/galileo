include config.mk

# -- options --

SBINDIR =	${PREFIX}/sbin
WWWDIR =	/var/www/htdocs

# -- build-related variables --

PROG =		galileo
VERSION =	0.4
DISTNAME =	${PROG}-${VERSION}

SRCS =		galileo.c config.c fcgi.c fragments.c log.c proc.c proxy.c \
		template/tmpl.c xmalloc.c y.tab.c

COBJS =		${COMPATS:.c=.o}
OBJS =		${SRCS:.c=.o} ${COBJS}

MAN =		${PROG}.conf.5 ${PROG}.8

# -- public targets --

all: ${PROG}
.PHONY: all clean distclean install uninstall

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f *.[do] y.tab.* compat/*.[do] tests/*.[do] fragments.c
	${MAKE} -C template clean

distclean: clean
	rm -f config.h config.h.old config.mk config.log config.log.old
	${MAKE} -C template distclean

install:
	mkdir -p ${DESTDIR}${MANDIR}/man5
	mkdir -p ${DESTDIR}${MANDIR}/man8
	mkdir -p ${DESTDIR}${SBINDIR}
	mkdir -p ${DESTDIR}${WWWDIR}
	${INSTALL_MAN} galileo.conf.5 ${DESTDIR}${MANDIR}/man5/${PROG}.conf.5
	${INSTALL_MAN} galileo.8 ${DESTDIR}${MANDIR}/man8/${PROG}.8
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${SBINDIR}
	${INSTALL_DATA} galileo.css ${DESTDIR}${WWWDIR}

uninstall:
	rm ${DESTDIR}${MANDIR}/man5/${PROG}.conf.5
	rm ${DESTDIR}${MANDIR}/man8/${PROG}.8
	rm ${DESTDIR}${SBINDIR}/${PROG}
	rm ${DESTDIR}${WWWDIR}/galileo.css

# -- internal build targets --

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LIBS} ${LDFLAGS}

fragments.c: fragments.tmpl
	${MAKE} -C template
	./template/template -o $@ fragments.tmpl

y.tab.c: parse.y
	${YACC} -b y parse.y

.c.o:
	${CC} ${CFLAGS} -c $< -o $@

# -- maintainer targets --

PRIVKEY =	missing-PRIVKEY
DISTFILES =	CHANGES \
		Makefile \
		README \
		config.c \
		configure \
		fcgi.c \
		fragments.c \
		fragments.tmpl \
		galileo.8 \
		galileo.c \
		galileo.conf.5 \
		galileo.css \
		galileo.h \
		log.c \
		log.h \
		parse.y \
		proc.c \
		proc.h \
		proxy.c \
		xmalloc.c \
		xmalloc.h \
		y.tab.c

.PHONY: release dist

release:
	sed -i -e '/^RELEASE=/s/no/yes/' configure
	${MAKE} ${DISTNAME}.sha256.sig
	sed -i -e '/^RELEASE=/s/yes/no/' configure

dist: ${DISTNAME}.sha256

${DISTNAME}.sha256.sig: ${DISTNAME}.sha256
	signify -S -e -m ${DISTNAME}.sha256 -s ${PRIVKEY}

${DISTNAME}.sha256: ${DISTNAME}.tar.gz
	sha256 ${DISTNAME}.tar.gz > $@

${DISTNAME}.tar.gz: ${DISTFILES}
	mkdir -p .dist/${DISTNAME}/
	${INSTALL} -m 0644 ${DISTFILES} .dist/${DISTNAME}
	${MAKE} -C compat	DESTDIR=${PWD}/.dist/${DISTNAME}/compat dist
	${MAKE} -C keys		DESTDIR=${PWD}/.dist/${DISTNAME}/keys dist
	${MAKE} -C template	DESTDIR=${PWD}/.dist/${DISTNAME}/template dist
	${MAKE} -C tests	DESTDIR=${PWD}/.dist/${DISTNAME}/tests dist
	cd .dist/${DISTNAME} && chmod 755 configure template/configure
	cd .dist && tar czf ../$@ ${DISTNAME}
	rm -rf .dist/

.PHONY: ${DISTNAME}.tar.gz

# -- dependencies --

-include config.d
-include fcgi.d
-include fragments.d
-include galileo.d
-include log.d
-include proc.d
-include proxy.d
-include template/tmpl.d
-include xmalloc.d
-include y.tab.d

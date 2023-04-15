NEXTV =		05

DISTFILES =	Makefile \
		galileo-01.pub \
		galileo-02.pub \
		galileo-03.pub \
		galileo-04.pub

all:
	false

newkey:
	signify -G -p galileo-${NEXTV}.pub -s galileo-${NEXTV}.sec

dist: ${DISTFILES}
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/

.PHONY: all dist
include ../config.mk

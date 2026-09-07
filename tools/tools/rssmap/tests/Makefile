PACKAGE=	tests

ATF_TESTS_SH=	rssmap_test
TEST_METADATA+=	execenv="host"

CFLAGS+=	-I${SRCTOP}/sys
${PACKAGE}FILES+=	rssmap_fixture.so
CLEANFILES+=	rssmap_fixture.so

all: rssmap_fixture.so

rssmap_fixture.so: rssmap_fixture.c
	${CC} ${CFLAGS} -fPIC -shared -o ${.TARGET} ${.ALLSRC}

.include <bsd.test.mk>

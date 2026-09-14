/*-
 * Copyright (c) 2018 The FreeBSD Foundation
 *
 * This software was developed by Mark Johnston under sponsorship from
 * the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/event.h>
#include <sys/filio.h>
#include <sys/ioccom.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <err.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atf-c.h>

/*
 * Given an array of non-blocking listening sockets configured in a LB group
 * for "addr", try connecting to "addr" in a loop and verify that connections
 * are roughly balanced across the sockets.
 */
static void
lb_simple_accept_loop(int domain, const struct sockaddr *addr, int sds[],
    size_t nsds, int nconns)
{
	size_t i;
	int *acceptcnt;
	int csd, error, excnt, sd;
	const struct linger lopt = { 1, 0 };

	/*
	 * We expect each listening socket to accept roughly nconns/nsds
	 * connections, but allow for some error.
	 */
	excnt = nconns / nsds / 8;
	acceptcnt = calloc(nsds, sizeof(*acceptcnt));
	ATF_REQUIRE_MSG(acceptcnt != NULL, "calloc() failed: %s",
	    strerror(errno));

	while (nconns-- > 0) {
		sd = socket(domain, SOCK_STREAM, 0);
		ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s",
		    strerror(errno));

		error = connect(sd, addr, addr->sa_len);
		ATF_REQUIRE_MSG(error == 0, "connect() failed: %s",
		    strerror(errno));

		error = setsockopt(sd, SOL_SOCKET, SO_LINGER, &lopt, sizeof(lopt));
		ATF_REQUIRE_MSG(error == 0, "Setting linger failed: %s",
		    strerror(errno));

		/*
		 * Poll the listening sockets.
		 */
		do {
			for (i = 0; i < nsds; i++) {
				csd = accept(sds[i], NULL, NULL);
				if (csd < 0) {
					ATF_REQUIRE_MSG(errno == EWOULDBLOCK ||
					    errno == EAGAIN,
					    "accept() failed: %s",
					    strerror(errno));
					continue;
				}

				error = close(csd);
				ATF_REQUIRE_MSG(error == 0,
				    "close() failed: %s", strerror(errno));

				acceptcnt[i]++;
				break;
			}
		} while (i == nsds);

		error = close(sd);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}

	for (i = 0; i < nsds; i++)
		ATF_REQUIRE_MSG(acceptcnt[i] > excnt, "uneven balancing");
}

static int
lb_listen_socket(int domain, int flags)
{
	int one;
	int error, sd;

	sd = socket(domain, SOCK_STREAM | flags, 0);
	ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));

	one = 1;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB, &one, sizeof(one));
	ATF_REQUIRE_MSG(error == 0, "setsockopt(SO_REUSEPORT_LB) failed: %s",
	    strerror(errno));

	return (sd);
}

ATF_TC_WITHOUT_HEAD(basic_ipv4);
ATF_TC_BODY(basic_ipv4, tc)
{
	struct sockaddr_in addr;
	socklen_t slen;
	size_t i;
	const int nconns = 16384;
	int error, sds[16];
	uint16_t port;

	sds[0] = lb_listen_socket(PF_INET, SOCK_NONBLOCK);

	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	error = bind(sds[0], (const struct sockaddr *)&addr, sizeof(addr));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));
	error = listen(sds[0], 1);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));

	slen = sizeof(addr);
	error = getsockname(sds[0], (struct sockaddr *)&addr, &slen);
	ATF_REQUIRE_MSG(error == 0, "getsockname() failed: %s",
	    strerror(errno));
	ATF_REQUIRE_MSG(slen == sizeof(addr), "sockaddr size changed");
	port = addr.sin_port;

	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = port;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	for (i = 1; i < nitems(sds); i++) {
		sds[i] = lb_listen_socket(PF_INET, SOCK_NONBLOCK);

		error = bind(sds[i], (const struct sockaddr *)&addr,
		    sizeof(addr));
		ATF_REQUIRE_MSG(error == 0, "bind() failed: %s",
		    strerror(errno));
		error = listen(sds[i], 1);
		ATF_REQUIRE_MSG(error == 0, "listen() failed: %s",
		    strerror(errno));
	}

	lb_simple_accept_loop(PF_INET, (struct sockaddr *)&addr, sds,
	    nitems(sds), nconns);
	for (i = 0; i < nitems(sds); i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
}

ATF_TC_WITHOUT_HEAD(basic_ipv6);
ATF_TC_BODY(basic_ipv6, tc)
{
	const struct in6_addr loopback6 = IN6ADDR_LOOPBACK_INIT;
	struct sockaddr_in6 addr;
	socklen_t slen;
	size_t i;
	const int nconns = 16384;
	int error, sds[16];
	uint16_t port;

	sds[0] = lb_listen_socket(PF_INET6, SOCK_NONBLOCK);

	memset(&addr, 0, sizeof(addr));
	addr.sin6_len = sizeof(addr);
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(0);
	addr.sin6_addr = loopback6;
	error = bind(sds[0], (const struct sockaddr *)&addr, sizeof(addr));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));
	error = listen(sds[0], 1);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));

	slen = sizeof(addr);
	error = getsockname(sds[0], (struct sockaddr *)&addr, &slen);
	ATF_REQUIRE_MSG(error == 0, "getsockname() failed: %s",
	    strerror(errno));
	ATF_REQUIRE_MSG(slen == sizeof(addr), "sockaddr size changed");
	port = addr.sin6_port;

	memset(&addr, 0, sizeof(addr));
	addr.sin6_len = sizeof(addr);
	addr.sin6_family = AF_INET6;
	addr.sin6_port = port;
	addr.sin6_addr = loopback6;
	for (i = 1; i < nitems(sds); i++) {
		sds[i] = lb_listen_socket(PF_INET6, SOCK_NONBLOCK);

		error = bind(sds[i], (const struct sockaddr *)&addr,
		    sizeof(addr));
		ATF_REQUIRE_MSG(error == 0, "bind() failed: %s",
		    strerror(errno));
		error = listen(sds[i], 1);
		ATF_REQUIRE_MSG(error == 0, "listen() failed: %s",
		    strerror(errno));
	}

	lb_simple_accept_loop(PF_INET6, (struct sockaddr *)&addr, sds,
	    nitems(sds), nconns);
	for (i = 0; i < nitems(sds); i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
}

struct concurrent_add_softc {
	struct sockaddr_storage ss;
	int socks[128];
	int kq;
};

static void *
listener(void *arg)
{
	for (struct concurrent_add_softc *sc = arg;;) {
		struct kevent kev;
		ssize_t n;
		int error, count, cs, s;
		uint8_t b;

		count = kevent(sc->kq, NULL, 0, &kev, 1, NULL);
		ATF_REQUIRE_MSG(count == 1,
		    "kevent() failed: %s", strerror(errno));

		s = (int)kev.ident;
		cs = accept(s, NULL, NULL);
		ATF_REQUIRE_MSG(cs >= 0,
		    "accept() failed: %s", strerror(errno));

		b = 'M';
		n = write(cs, &b, sizeof(b));
		ATF_REQUIRE_MSG(n >= 0, "write() failed: %s", strerror(errno));
		ATF_REQUIRE(n == 1);

		error = close(cs);
		ATF_REQUIRE_MSG(error == 0 || errno == ECONNRESET,
		    "close() failed: %s", strerror(errno));
	}
}

static void *
connector(void *arg)
{
	for (struct concurrent_add_softc *sc = arg;;) {
		ssize_t n;
		int error, s;
		uint8_t b;

		s = socket(sc->ss.ss_family, SOCK_STREAM, 0);
		ATF_REQUIRE_MSG(s >= 0, "socket() failed: %s", strerror(errno));

		error = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (int[]){1},
		    sizeof(int));

		error = connect(s, (struct sockaddr *)&sc->ss, sc->ss.ss_len);
		ATF_REQUIRE_MSG(error == 0, "connect() failed: %s",
		    strerror(errno));

		n = read(s, &b, sizeof(b));
		ATF_REQUIRE_MSG(n >= 0, "read() failed: %s",
		    strerror(errno));
		ATF_REQUIRE(n == 1);
		ATF_REQUIRE(b == 'M');
		error = close(s);
		ATF_REQUIRE_MSG(error == 0,
		    "close() failed: %s", strerror(errno));
	}
}

/*
 * Run three threads.  One accepts connections from listening sockets on a
 * kqueue, while the other makes connections.  The third thread slowly adds
 * sockets to the LB group.  This is meant to help flush out race conditions.
 */
ATF_TC_WITHOUT_HEAD(concurrent_add);
ATF_TC_BODY(concurrent_add, tc)
{
	struct concurrent_add_softc sc;
	struct sockaddr_in *sin;
	pthread_t threads[4];
	int error;

	sc.kq = kqueue();
	ATF_REQUIRE_MSG(sc.kq >= 0, "kqueue() failed: %s", strerror(errno));

	error = pthread_create(&threads[0], NULL, listener, &sc);
	ATF_REQUIRE_MSG(error == 0, "pthread_create() failed: %s",
	    strerror(error));

	sin = (struct sockaddr_in *)&sc.ss;
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = htons(0);
	sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (size_t i = 0; i < nitems(sc.socks); i++) {
		struct kevent kev;
		int s;

		sc.socks[i] = s = socket(AF_INET, SOCK_STREAM, 0);
		ATF_REQUIRE_MSG(s >= 0, "socket() failed: %s", strerror(errno));

		error = setsockopt(s, SOL_SOCKET, SO_REUSEPORT_LB, (int[]){1},
		    sizeof(int));
		ATF_REQUIRE_MSG(error == 0,
		    "setsockopt(SO_REUSEPORT_LB) failed: %s", strerror(errno));

		error = bind(s, (struct sockaddr *)sin, sizeof(*sin));
		ATF_REQUIRE_MSG(error == 0, "bind() failed: %s",
		    strerror(errno));

		error = listen(s, 5);
		ATF_REQUIRE_MSG(error == 0, "listen() failed: %s",
		    strerror(errno));

		EV_SET(&kev, s, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);
		error = kevent(sc.kq, &kev, 1, NULL, 0, NULL);
		ATF_REQUIRE_MSG(error == 0, "kevent() failed: %s",
		    strerror(errno));

		if (i == 0) {
			socklen_t slen = sizeof(sc.ss);

			error = getsockname(sc.socks[i],
			    (struct sockaddr *)&sc.ss, &slen);
			ATF_REQUIRE_MSG(error == 0, "getsockname() failed: %s",
			    strerror(errno));
			ATF_REQUIRE(sc.ss.ss_family == AF_INET);

			for (size_t j = 1; j < nitems(threads); j++) {
				error = pthread_create(&threads[j], NULL,
				    connector, &sc);
				ATF_REQUIRE_MSG(error == 0,
				    "pthread_create() failed: %s",
				    strerror(error));
			}
		}

		usleep(20000);
	}

	for (size_t j = nitems(threads); j > 0; j--) {
		ATF_REQUIRE(pthread_cancel(threads[j - 1]) == 0);
		ATF_REQUIRE(pthread_join(threads[j - 1], NULL) == 0);
	}
}

/*
 * Try calling listen(2) twice on a socket with SO_REUSEPORT_LB set.
 */
ATF_TC_WITHOUT_HEAD(double_listen_ipv4);
ATF_TC_BODY(double_listen_ipv4, tc)
{
	struct sockaddr_in sin;
	int error, s;

	s = lb_listen_socket(PF_INET, 0);

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(0);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	error = bind(s, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = listen(s, 1);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));
	error = listen(s, 2);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));

	error = close(s);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}

/*
 * Try calling listen(2) twice on a socket with SO_REUSEPORT_LB set.
 */
ATF_TC_WITHOUT_HEAD(double_listen_ipv6);
ATF_TC_BODY(double_listen_ipv6, tc)
{
	struct sockaddr_in6 sin6;
	int error, s;

	s = lb_listen_socket(PF_INET6, 0);

	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_len = sizeof(sin6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(0);
	sin6.sin6_addr = in6addr_loopback;
	error = bind(s, (struct sockaddr *)&sin6, sizeof(sin6));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = listen(s, 1);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));
	error = listen(s, 2);
	ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));

	error = close(s);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}

/*
 * Try binding many sockets to the same lbgroup without calling listen(2) on
 * them.
 */
ATF_TC_WITHOUT_HEAD(bind_without_listen);
ATF_TC_BODY(bind_without_listen, tc)
{
	const int nsockets = 100;
	struct sockaddr_in sin;
	socklen_t socklen;
	int error, s, s2[nsockets];

	s = lb_listen_socket(PF_INET, 0);

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(0);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	error = bind(s, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	socklen = sizeof(sin);
	error = getsockname(s, (struct sockaddr *)&sin, &socklen);
	ATF_REQUIRE_MSG(error == 0, "getsockname() failed: %s",
	    strerror(errno));

	for (int i = 0; i < nsockets; i++) {
		s2[i] = lb_listen_socket(PF_INET, 0);
		error = bind(s2[i], (struct sockaddr *)&sin, sizeof(sin));
		ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));
	}
	for (int i = 0; i < nsockets; i++) {
		error = listen(s2[i], 1);
		ATF_REQUIRE_MSG(error == 0, "listen() failed: %s", strerror(errno));
	}
	for (int i = 0; i < nsockets; i++) {
		error = close(s2[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
	}

	error = close(s);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}

/*
 * Check that SO_REUSEPORT_LB doesn't mess with connect(2).
 * Two sockets:
 * 1) auxiliary peer socket 'p', where we connect to
 * 2) test socket 's', that sets SO_REUSEPORT_LB and then connect(2)s to 'p'
 */
ATF_TC_WITHOUT_HEAD(connect_not_bound);
ATF_TC_BODY(connect_not_bound, tc)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_len = sizeof(sin),
		.sin_addr = { htonl(INADDR_LOOPBACK) },
	};
	socklen_t slen = sizeof(struct sockaddr_in);
	int p, s, rv;

	ATF_REQUIRE((p = socket(PF_INET, SOCK_STREAM, 0)) > 0);
	ATF_REQUIRE(bind(p, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	ATF_REQUIRE(listen(p, 1) == 0);
	ATF_REQUIRE(getsockname(p, (struct sockaddr *)&sin, &slen) == 0);

	s = lb_listen_socket(PF_INET, 0);
	rv = connect(s, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(rv == -1 && errno == EOPNOTSUPP,
	    "Expected EOPNOTSUPP on connect(2) not met. Got %d, errno %d",
	    rv, errno);
	rv = sendto(s, "test", 4, 0, (struct sockaddr *)&sin,
	    sizeof(sin));
	ATF_REQUIRE_MSG(rv == -1 && errno == EOPNOTSUPP,
	    "Expected EOPNOTSUPP on sendto(2) not met. Got %d, errno %d",
	    rv, errno);

	close(p);
	close(s);
}

/*
 * Same as above, but we also bind(2) between setsockopt(2) of SO_REUSEPORT_LB
 * and the connect(2).
 */
ATF_TC_WITHOUT_HEAD(connect_bound);
ATF_TC_BODY(connect_bound, tc)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_len = sizeof(sin),
		.sin_addr = { htonl(INADDR_LOOPBACK) },
	};
	socklen_t slen = sizeof(struct sockaddr_in);
	int p, s, rv;

	ATF_REQUIRE((p = socket(PF_INET, SOCK_STREAM, 0)) > 0);
	ATF_REQUIRE(bind(p, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	ATF_REQUIRE(listen(p, 1) == 0);

	s = lb_listen_socket(PF_INET, 0);
	ATF_REQUIRE(bind(s, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	ATF_REQUIRE(getsockname(p, (struct sockaddr *)&sin, &slen) == 0);
	rv = connect(s, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(rv == -1 && errno == EOPNOTSUPP,
	    "Expected EOPNOTSUPP on connect(2) not met. Got %d, errno %d",
	    rv, errno);
	rv = sendto(s, "test", 4, 0, (struct sockaddr *)&sin,
	    sizeof(sin));
	ATF_REQUIRE_MSG(rv == -1 && errno == EOPNOTSUPP,
	    "Expected EOPNOTSUPP on sendto(2) not met. Got %d, errno %d",
	    rv, errno);

	close(p);
	close(s);
}

/*
 * The kernel erroneously permits calling connect() on a UDP socket with
 * SO_REUSEPORT_LB set.  Verify that packets sent to the bound address are
 * dropped unless they come from the connected address.
 */
ATF_TC_WITHOUT_HEAD(connect_udp);
ATF_TC_BODY(connect_udp, tc)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_len = sizeof(sin),
		.sin_addr = { htonl(INADDR_LOOPBACK) },
	};
	ssize_t n;
	int error, len, s1, s2, s3;
	char ch;

	s1 = socket(PF_INET, SOCK_DGRAM, 0);
	ATF_REQUIRE(s1 >= 0);
	s2 = socket(PF_INET, SOCK_DGRAM, 0);
	ATF_REQUIRE(s2 >= 0);
	s3 = socket(PF_INET, SOCK_DGRAM, 0);
	ATF_REQUIRE(s3 >= 0);

	error = setsockopt(s1, SOL_SOCKET, SO_REUSEPORT_LB, (int[]){1},
	    sizeof(int));
	ATF_REQUIRE_MSG(error == 0,
	    "setsockopt(SO_REUSEPORT_LB) failed: %s", strerror(errno));
	error = bind(s1, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = bind(s2, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = bind(s3, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	/* Connect to an address not owned by s2. */
	error = getsockname(s3, (struct sockaddr *)&sin,
	    (socklen_t[]){sizeof(sin)});
	ATF_REQUIRE(error == 0);
	error = connect(s1, (struct sockaddr *)&sin, sizeof(sin));
	ATF_REQUIRE_MSG(error == 0, "connect() failed: %s", strerror(errno));

	/* Try to send a packet to s1 from s2. */
	error = getsockname(s1, (struct sockaddr *)&sin,
	    (socklen_t[]){sizeof(sin)});
	ATF_REQUIRE(error == 0);

	ch = 42;
	n = sendto(s2, &ch, sizeof(ch), 0, (struct sockaddr *)&sin,
	    sizeof(sin));
	ATF_REQUIRE(n == 1);

	/* Give the packet some time to arrive. */
	usleep(100000);

	/* s1 is connected to s3 and shouldn't receive from s2. */
	error = ioctl(s1, FIONREAD, &len);
	ATF_REQUIRE(error == 0);
	ATF_REQUIRE_MSG(len == 0, "unexpected data available");

	/* ... but s3 can of course send to s1. */
	n = sendto(s3, &ch, sizeof(ch), 0, (struct sockaddr *)&sin,
	    sizeof(sin));
	ATF_REQUIRE(n == 1);
	usleep(100000);
	error = ioctl(s1, FIONREAD, &len);
	ATF_REQUIRE(error == 0);
	ATF_REQUIRE_MSG(len == 1, "expected data available");
}

/*
 * The kernel erroneously permits calling connect() on a UDP socket with
 * SO_REUSEPORT_LB set.  Verify that packets sent to the bound address are
 * dropped unless they come from the connected address.
 */
ATF_TC_WITHOUT_HEAD(connect_udp6);
ATF_TC_BODY(connect_udp6, tc)
{
	struct sockaddr_in6 sin6 = {
		.sin6_family = AF_INET6,
		.sin6_len = sizeof(sin6),
		.sin6_addr = IN6ADDR_LOOPBACK_INIT,
	};
	ssize_t n;
	int error, len, s1, s2, s3;
	char ch;

	s1 = socket(PF_INET6, SOCK_DGRAM, 0);
	ATF_REQUIRE(s1 >= 0);
	s2 = socket(PF_INET6, SOCK_DGRAM, 0);
	ATF_REQUIRE(s2 >= 0);
	s3 = socket(PF_INET6, SOCK_DGRAM, 0);
	ATF_REQUIRE(s3 >= 0);

	error = setsockopt(s1, SOL_SOCKET, SO_REUSEPORT_LB, (int[]){1},
	    sizeof(int));
	ATF_REQUIRE_MSG(error == 0,
	    "setsockopt(SO_REUSEPORT_LB) failed: %s", strerror(errno));
	error = bind(s1, (struct sockaddr *)&sin6, sizeof(sin6));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = bind(s2, (struct sockaddr *)&sin6, sizeof(sin6));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	error = bind(s3, (struct sockaddr *)&sin6, sizeof(sin6));
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));

	/* Connect to an address not owned by s2. */
	error = getsockname(s3, (struct sockaddr *)&sin6,
	    (socklen_t[]){sizeof(sin6)});
	ATF_REQUIRE(error == 0);
	error = connect(s1, (struct sockaddr *)&sin6, sizeof(sin6));
	ATF_REQUIRE_MSG(error == 0, "connect() failed: %s", strerror(errno));

	/* Try to send a packet to s1 from s2. */
	error = getsockname(s1, (struct sockaddr *)&sin6,
	    (socklen_t[]){sizeof(sin6)});
	ATF_REQUIRE(error == 0);

	ch = 42;
	n = sendto(s2, &ch, sizeof(ch), 0, (struct sockaddr *)&sin6,
	    sizeof(sin6));
	ATF_REQUIRE(n == 1);

	/* Give the packet some time to arrive. */
	usleep(100000);

	/* s1 is connected to s3 and shouldn't receive from s2. */
	error = ioctl(s1, FIONREAD, &len);
	ATF_REQUIRE(error == 0);
	ATF_REQUIRE_MSG(len == 0, "unexpected data available");

	/* ... but s3 can of course send to s1. */
	n = sendto(s3, &ch, sizeof(ch), 0, (struct sockaddr *)&sin6,
	    sizeof(sin6));
	ATF_REQUIRE(n == 1);
	usleep(100000);
	error = ioctl(s1, FIONREAD, &len);
	ATF_REQUIRE(error == 0);
	ATF_REQUIRE_MSG(len == 1, "expected data available");
}

/*
 * Helpers for the SO_REUSEPORT_LB_CPU receive-CPU tag.
 */
static int
lb_cpu_get(int sd)
{
	socklen_t slen;
	int cpu, error;

	slen = sizeof(cpu);
	error = getsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB_CPU, &cpu, &slen);
	ATF_REQUIRE_MSG(error == 0,
	    "getsockopt(SO_REUSEPORT_LB_CPU) failed: %s", strerror(errno));
	ATF_REQUIRE_MSG(slen == sizeof(cpu), "option size changed");
	return (cpu);
}

static void
lb_cpu_set(int sd, int cpu)
{
	int error;

	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB_CPU, &cpu,
	    sizeof(cpu));
	ATF_REQUIRE_MSG(error == 0,
	    "setsockopt(SO_REUSEPORT_LB_CPU, %d) failed: %s", cpu,
	    strerror(errno));
}

/*
 * Return the CPU count, skipping the test if CPU IDs are not dense, as the
 * tests below tag listener i with CPU i.
 */
static int
lb_ncpus(void)
{
	size_t len;
	int maxid, ncpu;

	len = sizeof(ncpu);
	ATF_REQUIRE_MSG(sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0,
	    "sysctl hw.ncpu failed: %s", strerror(errno));
	len = sizeof(maxid);
	ATF_REQUIRE_MSG(sysctlbyname("kern.smp.maxid", &maxid, &len, NULL,
	    0) == 0, "sysctl kern.smp.maxid failed: %s", strerror(errno));
	if (ncpu != maxid + 1)
		atf_tc_skip("CPU IDs are not dense");
	return (ncpu);
}

static void
lb_pin(int cpu)
{
	cpuset_t set;
	int error;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	error = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(set), &set);
	ATF_REQUIRE_MSG(error == 0, "cpuset_setaffinity() failed: %s",
	    strerror(errno));
}

static socklen_t
lb_loopback_addr(int domain, uint16_t port, struct sockaddr_storage *ss)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;

	memset(ss, 0, sizeof(*ss));
	if (domain == PF_INET) {
		sin = (struct sockaddr_in *)ss;
		sin->sin_len = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_port = port;
		sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		return (sizeof(*sin));
	}
	sin6 = (struct sockaddr_in6 *)ss;
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = port;
	sin6->sin6_addr = in6addr_loopback;
	return (sizeof(*sin6));
}

static int
lb_bound_socket(int domain, int type, int flags, uint16_t port,
    struct sockaddr_storage *ss)
{
	socklen_t slen;
	int error, one, sd;

	sd = socket(domain, type | flags, 0);
	ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));
	one = 1;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB, &one, sizeof(one));
	ATF_REQUIRE_MSG(error == 0, "setsockopt(SO_REUSEPORT_LB) failed: %s",
	    strerror(errno));
	if (domain == PF_INET6) {
		error = setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, &one,
		    sizeof(one));
		ATF_REQUIRE_MSG(error == 0,
		    "setsockopt(IPV6_V6ONLY) failed: %s", strerror(errno));
	}

	slen = lb_loopback_addr(domain, port, ss);
	error = bind(sd, (struct sockaddr *)ss, slen);
	ATF_REQUIRE_MSG(error == 0, "bind() failed: %s", strerror(errno));
	if (type == SOCK_STREAM) {
		error = listen(sd, 32);
		ATF_REQUIRE_MSG(error == 0, "listen() failed: %s",
		    strerror(errno));
	}
	if (port == 0) {
		slen = sizeof(*ss);
		error = getsockname(sd, (struct sockaddr *)ss, &slen);
		ATF_REQUIRE_MSG(error == 0, "getsockname() failed: %s",
		    strerror(errno));
	}
	return (sd);
}

static uint16_t
lb_addr_port(const struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET)
		return (((const struct sockaddr_in *)ss)->sin_port);
	return (((const struct sockaddr_in6 *)ss)->sin6_port);
}

static void
lb_cpu_readback(int domain, int type)
{
	struct sockaddr_storage ss;
	int error, ncpu, pin, sd, val;

	ncpu = lb_ncpus();

	sd = socket(domain, type, 0);
	ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));
	val = 1;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB, &val, sizeof(val));
	ATF_REQUIRE_MSG(error == 0, "setsockopt(SO_REUSEPORT_LB) failed: %s",
	    strerror(errno));

	/* A socket has no affinity until one is requested. */
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	/* The option may be set before the socket joins a group. */
	lb_cpu_set(sd, 0);
	ATF_REQUIRE_EQ(0, lb_cpu_get(sd));
	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));

	/* ... and on a socket that is already a group member. */
	sd = lb_bound_socket(domain, type, 0, 0, &ss);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));
	lb_cpu_set(sd, ncpu - 1);
	ATF_REQUIRE_EQ(ncpu - 1, lb_cpu_get(sd));

	/* CURRENT resolves to the calling thread's CPU. */
	pin = ncpu > 1 ? 1 : 0;
	lb_pin(pin);
	lb_cpu_set(sd, SO_REUSEPORT_LB_CPU_CURRENT);
	ATF_REQUIRE_EQ(pin, lb_cpu_get(sd));

	/* ANY restores hashed selection. */
	lb_cpu_set(sd, SO_REUSEPORT_LB_CPU_ANY);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	/* Out of range CPU IDs are rejected. */
	val = ncpu;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB_CPU, &val,
	    sizeof(val));
	ATF_REQUIRE_MSG(error == -1 && errno == EINVAL,
	    "expected EINVAL for CPU %d, got %d/%d", val, error, errno);
	val = -3;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB_CPU, &val,
	    sizeof(val));
	ATF_REQUIRE_MSG(error == -1 && errno == EINVAL,
	    "expected EINVAL for CPU %d, got %d/%d", val, error, errno);

	/* So is a short option value. */
	val = 0;
	error = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT_LB_CPU, &val,
	    sizeof(char));
	ATF_REQUIRE_MSG(error == -1 && errno == EINVAL,
	    "expected EINVAL for a short optval, got %d/%d", error, errno);

	/* None of the failures changed the socket. */
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}

ATF_TC_WITHOUT_HEAD(lb_cpu_readback_ipv4);
ATF_TC_BODY(lb_cpu_readback_ipv4, tc)
{
	lb_cpu_readback(PF_INET, SOCK_STREAM);
}

ATF_TC_WITHOUT_HEAD(lb_cpu_readback_ipv6);
ATF_TC_BODY(lb_cpu_readback_ipv6, tc)
{
	lb_cpu_readback(PF_INET6, SOCK_STREAM);
}

ATF_TC_WITHOUT_HEAD(lb_cpu_readback_udp);
ATF_TC_BODY(lb_cpu_readback_udp, tc)
{
	lb_cpu_readback(PF_INET, SOCK_DGRAM);
}

/*
 * Connect once and return the index of the listening socket that accepted,
 * storing the accepted socket's receive CPU in *cpup.
 */
static int
lb_connect_once(int domain, const struct sockaddr_storage *ss, int sds[],
    int nsds, int *cpup)
{
	const struct linger lopt = { 1, 0 };
	socklen_t slen;
	int csd, error, i, sd;

	slen = ss->ss_family == AF_INET ? sizeof(struct sockaddr_in) :
	    sizeof(struct sockaddr_in6);
	sd = socket(domain, SOCK_STREAM, 0);
	ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));
	error = connect(sd, (const struct sockaddr *)ss, slen);
	ATF_REQUIRE_MSG(error == 0, "connect() failed: %s", strerror(errno));
	error = setsockopt(sd, SOL_SOCKET, SO_LINGER, &lopt, sizeof(lopt));
	ATF_REQUIRE_MSG(error == 0, "Setting linger failed: %s",
	    strerror(errno));

	for (;;) {
		for (i = 0; i < nsds; i++) {
			csd = accept(sds[i], NULL, NULL);
			if (csd < 0) {
				ATF_REQUIRE_MSG(errno == EWOULDBLOCK ||
				    errno == EAGAIN, "accept() failed: %s",
				    strerror(errno));
				continue;
			}
			*cpup = lb_cpu_get(csd);
			error = close(csd);
			ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
			    strerror(errno));
			error = close(sd);
			ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
			    strerror(errno));
			return (i);
		}
	}
}

/*
 * With one listener tagged for each CPU, a connection whose SYN the stack
 * processed on CPU c must be accepted by listener c.
 */
ATF_TC_WITHOUT_HEAD(lb_cpu_steering);
ATF_TC_BODY(lb_cpu_steering, tc)
{
	struct sockaddr_storage ss;
	int *sds;
	int cpu, error, i, mismatch, n, ncpu;
	const int nconns = 256;

	ncpu = lb_ncpus();
	if (ncpu > 64)
		ncpu = 64;
	sds = calloc(ncpu, sizeof(*sds));
	ATF_REQUIRE_MSG(sds != NULL, "calloc() failed: %s", strerror(errno));

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	lb_cpu_set(sds[0], 0);
	for (i = 1; i < ncpu; i++) {
		sds[i] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
		    lb_addr_port(&ss), &ss);
		lb_cpu_set(sds[i], i);
	}

	for (n = 0, mismatch = 0; n < nconns; n++) {
		i = lb_connect_once(PF_INET, &ss, sds, ncpu, &cpu);
		if (cpu >= 0 && cpu < ncpu && cpu != i)
			mismatch++;
	}

	/*
	 * The accepting thread is not pinned, so a connection whose handshake
	 * completed after a migration can miss.  Steering must still hold for
	 * the overwhelming majority.
	 */
	ATF_REQUIRE_MSG(mismatch <= nconns / 20,
	    "%d of %d connections were not steered", mismatch, nconns);

	for (i = 0; i < ncpu; i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
	free(sds);
}

/*
 * Two listeners tagged with the same CPU must share that CPU's connections.
 * Selecting the first matching member would starve the second one.
 */
ATF_TC_WITHOUT_HEAD(lb_cpu_duplicates);
ATF_TC_BODY(lb_cpu_duplicates, tc)
{
	struct sockaddr_storage ss;
	int *acceptcnt, *sds;
	int cpu, error, i, n, ncpu, nsds;
	const int nconns = 512;

	ncpu = lb_ncpus();
	if (ncpu > 32)
		ncpu = 32;
	nsds = 2 * ncpu;
	sds = calloc(nsds, sizeof(*sds));
	ATF_REQUIRE_MSG(sds != NULL, "calloc() failed: %s", strerror(errno));
	acceptcnt = calloc(nsds, sizeof(*acceptcnt));
	ATF_REQUIRE_MSG(acceptcnt != NULL, "calloc() failed: %s",
	    strerror(errno));

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	lb_cpu_set(sds[0], 0);
	for (i = 1; i < nsds; i++) {
		sds[i] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
		    lb_addr_port(&ss), &ss);
		lb_cpu_set(sds[i], i / 2);
	}

	for (n = 0; n < nconns; n++) {
		i = lb_connect_once(PF_INET, &ss, sds, nsds, &cpu);
		acceptcnt[i]++;
	}

	for (i = 0; i < ncpu; i++) {
		if (acceptcnt[2 * i] + acceptcnt[2 * i + 1] < 20)
			continue;
		ATF_REQUIRE_MSG(acceptcnt[2 * i] > 0 &&
		    acceptcnt[2 * i + 1] > 0,
		    "CPU %d: listeners took %d and %d connections", i,
		    acceptcnt[2 * i], acceptcnt[2 * i + 1]);
	}

	for (i = 0; i < nsds; i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
	free(acceptcnt);
	free(sds);
}

/*
 * Helpers for the tag captured at the first accept(2).  The tests below need
 * to change net.inet.tcp.reuseport_lb_accept_cpu, so they save the current
 * value in the work directory and restore it from a cleanup routine.
 */
#define	LB_ACCEPT_SYSCTL	"net.inet.tcp.reuseport_lb_accept_cpu"
#define	LB_ACCEPT_SAVEFILE	"lb_accept_mode"

static void
lb_accept_mode_set(int mode)
{
	int error;

	error = sysctlbyname(LB_ACCEPT_SYSCTL, NULL, NULL, &mode,
	    sizeof(mode));
	ATF_REQUIRE_MSG(error == 0, "sysctl %s=%d failed: %s",
	    LB_ACCEPT_SYSCTL, mode, strerror(errno));
}

static void
lb_accept_mode_save(void)
{
	FILE *fp;
	size_t len;
	int mode;

	len = sizeof(mode);
	ATF_REQUIRE_MSG(sysctlbyname(LB_ACCEPT_SYSCTL, &mode, &len, NULL,
	    0) == 0, "sysctl %s failed: %s", LB_ACCEPT_SYSCTL,
	    strerror(errno));
	fp = fopen(LB_ACCEPT_SAVEFILE, "w");
	ATF_REQUIRE_MSG(fp != NULL, "fopen() failed: %s", strerror(errno));
	ATF_REQUIRE(fprintf(fp, "%d\n", mode) > 0);
	ATF_REQUIRE(fclose(fp) == 0);
}

static void
lb_accept_mode_restore(void)
{
	FILE *fp;
	int mode;

	fp = fopen(LB_ACCEPT_SAVEFILE, "r");
	if (fp == NULL)
		return;
	if (fscanf(fp, "%d", &mode) == 1)
		(void)sysctlbyname(LB_ACCEPT_SYSCTL, NULL, NULL, &mode,
		    sizeof(mode));
	(void)fclose(fp);
}

/*
 * Connect to "ss" and accept the connection on one of "nsds" blocking
 * listeners, returning the index of the one that accepted.
 */
static int
lb_exchange(const struct sockaddr_storage *ss, int sds[], int nsds)
{
	struct pollfd *pfd;
	socklen_t slen;
	int csd, error, i, n, sd;

	slen = ss->ss_family == AF_INET ? sizeof(struct sockaddr_in) :
	    sizeof(struct sockaddr_in6);
	sd = socket(ss->ss_family == AF_INET ? PF_INET : PF_INET6,
	    SOCK_STREAM, 0);
	ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));
	error = connect(sd, (const struct sockaddr *)ss, slen);
	ATF_REQUIRE_MSG(error == 0, "connect() failed: %s", strerror(errno));

	pfd = calloc(nsds, sizeof(*pfd));
	ATF_REQUIRE_MSG(pfd != NULL, "calloc() failed: %s", strerror(errno));
	for (i = 0; i < nsds; i++) {
		pfd[i].fd = sds[i];
		pfd[i].events = POLLIN;
	}
	n = poll(pfd, nsds, 30000);
	ATF_REQUIRE_MSG(n > 0, "poll() failed or timed out: %d, %s", n,
	    strerror(errno));
	for (i = 0; i < nsds; i++)
		if ((pfd[i].revents & POLLIN) != 0)
			break;
	ATF_REQUIRE_MSG(i < nsds, "poll() reported no readable listener");
	free(pfd);

	csd = accept(sds[i], NULL, NULL);
	ATF_REQUIRE_MSG(csd >= 0, "accept() failed: %s", strerror(errno));
	error = close(csd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
	return (i);
}

/*
 * A pinned thread accepting on an untagged listener gives it that CPU.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_capture);
ATF_TC_HEAD(lb_cpu_accept_capture, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(lb_cpu_accept_capture, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, pin, sd;

	ncpu = lb_ncpus();
	lb_accept_mode_save();
	lb_accept_mode_set(2);

	pin = ncpu > 1 ? 1 : 0;
	lb_pin(pin);
	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(pin, lb_cpu_get(sd));

	/* A later accept(2) does not move the tag. */
	lb_pin(0);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(pin, lb_cpu_get(sd));

	/* An explicit request overrides a captured tag. */
	lb_cpu_set(sd, 0);
	ATF_REQUIRE_EQ(0, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_capture, tc)
{
	lb_accept_mode_restore();
}

/*
 * A thread that is not pinned to exactly one CPU tags nothing, so existing
 * applications keep the hashed distribution.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_unpinned);
ATF_TC_HEAD(lb_cpu_accept_unpinned, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(lb_cpu_accept_unpinned, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, sd;

	ncpu = lb_ncpus();
	if (ncpu == 1)
		atf_tc_skip("a single CPU is always a pinned thread");
	lb_accept_mode_save();
	lb_accept_mode_set(2);

	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_unpinned, tc)
{
	lb_accept_mode_restore();
}

/*
 * Asking for the hash opts a socket out of being tagged.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_sticky);
ATF_TC_HEAD(lb_cpu_accept_sticky, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(lb_cpu_accept_sticky, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, sd;

	ncpu = lb_ncpus();
	lb_accept_mode_save();
	lb_accept_mode_set(2);

	lb_pin(ncpu > 1 ? 1 : 0);
	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	lb_cpu_set(sd, SO_REUSEPORT_LB_CPU_ANY);

	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_sticky, tc)
{
	lb_accept_mode_restore();
}

/*
 * Nothing is tagged when the behaviour is turned off.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_off);
ATF_TC_HEAD(lb_cpu_accept_off, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(lb_cpu_accept_off, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, sd;

	ncpu = lb_ncpus();
	lb_accept_mode_save();
	lb_accept_mode_set(0);

	lb_pin(ncpu > 1 ? 1 : 0);
	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_off, tc)
{
	lb_accept_mode_restore();
}

/*
 * A captured tag must not steer while any member of the group is still
 * undecided, or a worker on a CPU the hardware never uses would be starved.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_gate);
ATF_TC_HEAD(lb_cpu_accept_gate, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(lb_cpu_accept_gate, tc)
{
	struct sockaddr_storage ss;
	int acceptcnt[2], sds[2];
	int error, i, n, ncpu, tagged;
	const int nconns = 256;

	ncpu = lb_ncpus();
	lb_accept_mode_save();
	lb_accept_mode_set(2);

	lb_pin(ncpu > 1 ? 1 : 0);
	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	sds[1] = lb_bound_socket(PF_INET, SOCK_STREAM, 0,
	    lb_addr_port(&ss), &ss);

	/* One accept(2) tags whichever listener took the connection. */
	tagged = lb_exchange(&ss, sds, 2);
	ATF_REQUIRE_MSG(lb_cpu_get(sds[tagged]) != SO_REUSEPORT_LB_CPU_ANY,
	    "the accepting listener was not tagged");
	ATF_REQUIRE_EQ(SO_REUSEPORT_LB_CPU_ANY, lb_cpu_get(sds[1 - tagged]));

	acceptcnt[0] = acceptcnt[1] = 0;
	for (n = 0; n < nconns; n++)
		acceptcnt[lb_exchange(&ss, sds, 2)]++;

	ATF_REQUIRE_MSG(acceptcnt[1 - tagged] > 0,
	    "the untagged listener was starved: %d and %d connections",
	    acceptcnt[0], acceptcnt[1]);

	for (i = 0; i < 2; i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
}
ATF_TC_CLEANUP(lb_cpu_accept_gate, tc)
{
	lb_accept_mode_restore();
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, basic_ipv4);
	ATF_TP_ADD_TC(tp, basic_ipv6);
	ATF_TP_ADD_TC(tp, concurrent_add);
	ATF_TP_ADD_TC(tp, double_listen_ipv4);
	ATF_TP_ADD_TC(tp, double_listen_ipv6);
	ATF_TP_ADD_TC(tp, bind_without_listen);
	ATF_TP_ADD_TC(tp, connect_not_bound);
	ATF_TP_ADD_TC(tp, connect_bound);
	ATF_TP_ADD_TC(tp, connect_udp);
	ATF_TP_ADD_TC(tp, connect_udp6);
	ATF_TP_ADD_TC(tp, lb_cpu_readback_ipv4);
	ATF_TP_ADD_TC(tp, lb_cpu_readback_ipv6);
	ATF_TP_ADD_TC(tp, lb_cpu_readback_udp);
	ATF_TP_ADD_TC(tp, lb_cpu_steering);
	ATF_TP_ADD_TC(tp, lb_cpu_duplicates);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_capture);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_unpinned);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_sticky);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_off);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_gate);

	return (atf_no_error());
}

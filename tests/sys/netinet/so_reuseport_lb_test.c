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
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
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
 * Helpers for the receive-CPU tag, which userland reads from the pcb list.
 */

/* The kernel address of the inpcb behind sd, from its descriptor entry. */
static uint64_t
lb_fd_inpcb(int sd)
{
	struct kinfo_file *kf;
	char *buf, *p;
	size_t len;
	uint64_t inpcb;
	int mib[4];

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_FILEDESC;
	mib[3] = getpid();
	ATF_REQUIRE_MSG(sysctl(mib, nitems(mib), NULL, &len, NULL, 0) == 0,
	    "sysctl kern.proc.filedesc failed: %s", strerror(errno));
	len *= 2;
	buf = malloc(len);
	ATF_REQUIRE_MSG(buf != NULL, "malloc() failed: %s", strerror(errno));
	ATF_REQUIRE_MSG(sysctl(mib, nitems(mib), buf, &len, NULL, 0) == 0,
	    "sysctl kern.proc.filedesc failed: %s", strerror(errno));
	inpcb = 0;
	for (p = buf; p < buf + len; p += kf->kf_structsize) {
		kf = (struct kinfo_file *)(void *)p;
		if (kf->kf_structsize == 0)
			break;
		if (kf->kf_fd == sd && kf->kf_type == KF_TYPE_SOCKET) {
			inpcb = kf->kf_un.kf_sock.kf_sock_inpcb;
			break;
		}
	}
	free(buf);
	ATF_REQUIRE_MSG(inpcb != 0, "no inpcb behind descriptor %d", sd);
	return (inpcb);
}

/*
 * The receive-CPU tag of TCP socket sd, or -1 if it has none.  The members
 * of a group share one local address and port, so the socket is found by
 * the pointer its descriptor entry carries, not by its addresses.
 */
static int
lb_cpu_get(int sd)
{
	struct xinpgen *xig;
	struct xtcpcb *xt;
	char *buf, *p;
	size_t len;
	uint64_t inpcb;
	int cpu;

	inpcb = lb_fd_inpcb(sd);
	ATF_REQUIRE_MSG(sysctlbyname("net.inet.tcp.pcblist", NULL, &len, NULL,
	    0) == 0, "sysctl net.inet.tcp.pcblist failed: %s", strerror(errno));
	len *= 2;
	buf = malloc(len);
	ATF_REQUIRE_MSG(buf != NULL, "malloc() failed: %s", strerror(errno));
	ATF_REQUIRE_MSG(sysctlbyname("net.inet.tcp.pcblist", buf, &len, NULL,
	    0) == 0, "sysctl net.inet.tcp.pcblist failed: %s", strerror(errno));
	xig = (struct xinpgen *)(void *)buf;
	cpu = INT_MIN;
	for (p = buf + xig->xig_len; p + sizeof(*xig) <= buf + len;
	    p += xt->xt_len) {
		xt = (struct xtcpcb *)(void *)p;
		if (xt->xt_len <= sizeof(*xig))
			break;
		if (xt->xt_inp.xi_socket.so_pcb == inpcb) {
			cpu = xt->xt_inp.inp_lb_cpu;
			break;
		}
	}
	free(buf);
	ATF_REQUIRE_MSG(cpu != INT_MIN, "descriptor %d is not in the pcb list",
	    sd);
	return (cpu);
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

static int lb_thread_cpus(void);

/*
 * A listener has no receive-CPU tag until it is given one; a socket returned
 * by accept(2) carries the CPU on which its handshake was completed.
 */
static void
lb_cpu_export(int domain)
{
	struct sockaddr_storage ss;
	int cpu, error, ncpu, sd;

	ncpu = lb_ncpus();
	sd = lb_bound_socket(domain, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	ATF_REQUIRE_EQ(-1, lb_cpu_get(sd));

	(void)lb_connect_once(domain, &ss, &sd, 1, &cpu);
	ATF_REQUIRE_MSG(cpu >= 0 && cpu < ncpu,
	    "accepted socket reports CPU %d of %d", cpu, ncpu);
	/* A thread pinned to one CPU would just have tagged the listener. */
	if (lb_thread_cpus() > 1)
		ATF_REQUIRE_EQ(-1, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}

ATF_TC_WITHOUT_HEAD(lb_cpu_export_ipv4);
ATF_TC_BODY(lb_cpu_export_ipv4, tc)
{
	lb_cpu_export(PF_INET);
}

ATF_TC_WITHOUT_HEAD(lb_cpu_export_ipv6);
ATF_TC_BODY(lb_cpu_export_ipv6, tc)
{
	lb_cpu_export(PF_INET6);
}

/*
 * Helpers for the learning done at accept(2).  The tests below change
 * net.inet.tcp.reuseport_lb_accept_cpu, so they save the current value in
 * the work directory and restore it from a cleanup routine.
 */
#define	LB_ACCEPT_SYSCTL	"net.inet.tcp.reuseport_lb_accept_cpu"
#define	LB_ACCEPT_SAVEFILE	"lb_accept_learn"

static void
lb_accept_learn_set(bool learn)
{
	int error;

	error = sysctlbyname(LB_ACCEPT_SYSCTL, NULL, NULL, &learn,
	    sizeof(learn));
	ATF_REQUIRE_MSG(error == 0, "sysctl %s=%d failed: %s",
	    LB_ACCEPT_SYSCTL, (int)learn, strerror(errno));
}

static void
lb_accept_learn_save(void)
{
	FILE *fp;
	size_t len;
	bool learn;

	len = sizeof(learn);
	ATF_REQUIRE_MSG(sysctlbyname(LB_ACCEPT_SYSCTL, &learn, &len, NULL,
	    0) == 0, "sysctl %s failed: %s", LB_ACCEPT_SYSCTL,
	    strerror(errno));
	fp = fopen(LB_ACCEPT_SAVEFILE, "w");
	ATF_REQUIRE_MSG(fp != NULL, "fopen() failed: %s", strerror(errno));
	ATF_REQUIRE(fprintf(fp, "%d\n", (int)learn) > 0);
	ATF_REQUIRE(fclose(fp) == 0);
}

static void
lb_accept_learn_restore(void)
{
	FILE *fp;
	bool learn;
	int val;

	fp = fopen(LB_ACCEPT_SAVEFILE, "r");
	if (fp == NULL)
		return;
	if (fscanf(fp, "%d", &val) == 1) {
		learn = val != 0;
		(void)sysctlbyname(LB_ACCEPT_SYSCTL, NULL, NULL, &learn,
		    sizeof(learn));
	}
	(void)fclose(fp);
}

static cpuset_t lb_thread_mask;
static bool lb_thread_mask_saved;

/* The number of CPUs the test thread started out allowed to run on. */
static int
lb_thread_cpus(void)
{
	int error;

	if (!lb_thread_mask_saved) {
		error = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
		    sizeof(lb_thread_mask), &lb_thread_mask);
		ATF_REQUIRE_MSG(error == 0, "cpuset_getaffinity() failed: %s",
		    strerror(errno));
		lb_thread_mask_saved = true;
	}
	return (CPU_COUNT(&lb_thread_mask));
}

static void
lb_pin(int cpu)
{
	cpuset_t set;
	int error;

	(void)lb_thread_cpus();
	if (!CPU_ISSET(cpu, &lb_thread_mask))
		atf_tc_skip("CPU %d is outside the test thread's cpuset", cpu);
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	error = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(set), &set);
	ATF_REQUIRE_MSG(error == 0, "cpuset_setaffinity() failed: %s",
	    strerror(errno));
}

static void
lb_unpin(void)
{
	int error;

	(void)lb_thread_cpus();
	error = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(lb_thread_mask), &lb_thread_mask);
	ATF_REQUIRE_MSG(error == 0, "cpuset_setaffinity() failed: %s",
	    strerror(errno));
}

static uint16_t
lb_addr_port(const struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET)
		return (((const struct sockaddr_in *)ss)->sin_port);
	return (((const struct sockaddr_in6 *)ss)->sin6_port);
}

/*
 * Give listener i the tag cpus[i] by accepting its first connection from a
 * thread pinned there, or no tag (cpus[i] < 0) by accepting unpinned.  Which
 * listener a connection reaches is the kernel's choice, so connect until
 * every listener has taken one.
 */
static void
lb_tag_listeners(int domain, const struct sockaddr_storage *ss, int sds[],
    int nsds, const int cpus[])
{
	const struct linger lopt = { 1, 0 };
	struct pollfd *pfd;
	bool *done;
	socklen_t slen;
	int csd, error, i, left, n, sd;

	done = calloc(nsds, sizeof(*done));
	pfd = calloc(nsds, sizeof(*pfd));
	ATF_REQUIRE_MSG(done != NULL && pfd != NULL, "calloc() failed: %s",
	    strerror(errno));
	slen = ss->ss_family == AF_INET ? sizeof(struct sockaddr_in) :
	    sizeof(struct sockaddr_in6);
	for (left = nsds, n = 0; left > 0 && n < 200 * nsds; n++) {
		sd = socket(domain, SOCK_STREAM, 0);
		ATF_REQUIRE_MSG(sd >= 0, "socket() failed: %s", strerror(errno));
		error = connect(sd, (const struct sockaddr *)ss, slen);
		ATF_REQUIRE_MSG(error == 0, "connect() failed: %s",
		    strerror(errno));
		error = setsockopt(sd, SOL_SOCKET, SO_LINGER, &lopt,
		    sizeof(lopt));
		ATF_REQUIRE_MSG(error == 0, "Setting linger failed: %s",
		    strerror(errno));
		for (;;) {
			for (i = 0; i < nsds; i++) {
				pfd[i].fd = sds[i];
				pfd[i].events = POLLIN;
			}
			error = poll(pfd, nsds, 30000);
			ATF_REQUIRE_MSG(error > 0,
			    "poll() failed or timed out: %d, %s", error,
			    strerror(errno));
			for (i = 0; i < nsds; i++)
				if ((pfd[i].revents & POLLIN) != 0)
					break;
			ATF_REQUIRE_MSG(i < nsds,
			    "poll() reported no readable listener");
			if (cpus[i] >= 0)
				lb_pin(cpus[i]);
			else
				lb_unpin();
			csd = accept(sds[i], NULL, NULL);
			if (csd >= 0)
				break;
			ATF_REQUIRE_MSG(errno == EWOULDBLOCK || errno == EAGAIN,
			    "accept() failed: %s", strerror(errno));
		}
		error = close(csd);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
		error = close(sd);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
		if (!done[i]) {
			done[i] = true;
			left--;
			ATF_REQUIRE_EQ(cpus[i], lb_cpu_get(sds[i]));
		}
	}
	lb_unpin();
	ATF_REQUIRE_MSG(left == 0, "%d of %d listeners never took a connection",
	    left, nsds);
	free(pfd);
	free(done);
}

/*
 * With one listener on each CPU, a connection whose handshake the stack
 * completed on CPU c must be accepted by listener c.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_steering);
ATF_TC_HEAD(lb_cpu_steering, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_steering, tc)
{
	struct sockaddr_storage ss;
	int *cpus, *sds;
	int cpu, error, i, mismatch, n, ncpu;
	const int nconns = 256;

	ncpu = lb_ncpus();
	if (ncpu > 64)
		ncpu = 64;
	lb_accept_learn_save();
	lb_accept_learn_set(true);
	sds = calloc(ncpu, sizeof(*sds));
	cpus = calloc(ncpu, sizeof(*cpus));
	ATF_REQUIRE_MSG(sds != NULL && cpus != NULL, "calloc() failed: %s",
	    strerror(errno));

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	for (i = 1; i < ncpu; i++)
		sds[i] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
		    lb_addr_port(&ss), &ss);
	for (i = 0; i < ncpu; i++)
		cpus[i] = i;
	lb_tag_listeners(PF_INET, &ss, sds, ncpu, cpus);

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
	free(cpus);
	free(sds);
}
ATF_TC_CLEANUP(lb_cpu_steering, tc)
{
	lb_accept_learn_restore();
}

/*
 * Two listeners on the same CPU must share that CPU's connections.
 * Selecting the first matching member would starve the second one.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_duplicates);
ATF_TC_HEAD(lb_cpu_duplicates, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_duplicates, tc)
{
	struct sockaddr_storage ss;
	int *acceptcnt, *cpus, *sds;
	int cpu, error, i, n, ncpu, nsds;
	const int nconns = 512;

	ncpu = lb_ncpus();
	if (ncpu > 32)
		ncpu = 32;
	lb_accept_learn_save();
	lb_accept_learn_set(true);
	nsds = 2 * ncpu;
	sds = calloc(nsds, sizeof(*sds));
	cpus = calloc(nsds, sizeof(*cpus));
	acceptcnt = calloc(nsds, sizeof(*acceptcnt));
	ATF_REQUIRE_MSG(sds != NULL && cpus != NULL && acceptcnt != NULL,
	    "calloc() failed: %s", strerror(errno));

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	for (i = 1; i < nsds; i++)
		sds[i] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
		    lb_addr_port(&ss), &ss);
	for (i = 0; i < nsds; i++)
		cpus[i] = i / 2;
	lb_tag_listeners(PF_INET, &ss, sds, nsds, cpus);

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
	free(cpus);
	free(sds);
}
ATF_TC_CLEANUP(lb_cpu_duplicates, tc)
{
	lb_accept_learn_restore();
}

/*
 * A member with no tag is owed the same share as the tagged ones: the CPUs
 * that receive spend what is left of their slots on it.  An exclusive claim
 * on each CPU would starve it instead.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_untagged_share);
ATF_TC_HEAD(lb_cpu_untagged_share, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_untagged_share, tc)
{
	struct sockaddr_storage ss;
	int *acceptcnt, *cpus, *sds;
	int cpu, error, i, n, ncpu, nsds;
	const int nconns = 512;

	ncpu = lb_ncpus();
	if (lb_thread_cpus() < 2)
		atf_tc_skip("the test thread is pinned to one CPU");
	if (ncpu > 32)
		ncpu = 32;
	lb_accept_learn_save();
	lb_accept_learn_set(true);
	nsds = ncpu + 1;		/* one per CPU, plus one with no tag */
	sds = calloc(nsds, sizeof(*sds));
	cpus = calloc(nsds, sizeof(*cpus));
	acceptcnt = calloc(nsds, sizeof(*acceptcnt));
	ATF_REQUIRE_MSG(sds != NULL && cpus != NULL && acceptcnt != NULL,
	    "calloc() failed: %s", strerror(errno));

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	for (i = 1; i < nsds; i++)
		sds[i] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
		    lb_addr_port(&ss), &ss);
	for (i = 0; i < ncpu; i++)
		cpus[i] = i;
	cpus[nsds - 1] = -1;
	lb_tag_listeners(PF_INET, &ss, sds, nsds, cpus);

	for (n = 0; n < nconns; n++) {
		i = lb_connect_once(PF_INET, &ss, sds, nsds, &cpu);
		acceptcnt[i]++;
	}

	ATF_REQUIRE_MSG(acceptcnt[nsds - 1] > 0,
	    "the untagged listener took none of %d connections", nconns);
	ATF_REQUIRE_MSG(acceptcnt[nsds - 1] < nconns / 2,
	    "the untagged listener took %d of %d connections",
	    acceptcnt[nsds - 1], nconns);

	for (i = 0; i < nsds; i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
	free(acceptcnt);
	free(cpus);
	free(sds);
}
ATF_TC_CLEANUP(lb_cpu_untagged_share, tc)
{
	lb_accept_learn_restore();
}

/*
 * Tagging only some of the CPUs still steers the ones that were tagged.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_partial_claims);
ATF_TC_HEAD(lb_cpu_partial_claims, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_partial_claims, tc)
{
	struct sockaddr_storage ss;
	const int cpus[2] = { 0, 1 };
	int sds[2];
	int cpu, error, i, matched, mismatch, n;
	const int nconns = 256;

	if (lb_ncpus() < 2)
		atf_tc_skip("a single CPU cannot be partially claimed");
	lb_accept_learn_save();
	lb_accept_learn_set(true);

	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK, 0, &ss);
	sds[1] = lb_bound_socket(PF_INET, SOCK_STREAM, SOCK_NONBLOCK,
	    lb_addr_port(&ss), &ss);
	lb_tag_listeners(PF_INET, &ss, sds, 2, cpus);

	for (n = 0, matched = 0, mismatch = 0; n < nconns; n++) {
		i = lb_connect_once(PF_INET, &ss, sds, 2, &cpu);
		if (cpu != 0 && cpu != 1)
			continue;	/* no member claimed that CPU */
		if (cpu == i)
			matched++;
		else
			mismatch++;
	}

	ATF_REQUIRE_MSG(mismatch <= (matched + mismatch) / 20,
	    "%d of %d connections on a claimed CPU were not steered",
	    mismatch, matched + mismatch);

	for (i = 0; i < 2; i++) {
		error = close(sds[i]);
		ATF_REQUIRE_MSG(error == 0, "close() failed: %s",
		    strerror(errno));
	}
}
ATF_TC_CLEANUP(lb_cpu_partial_claims, tc)
{
	lb_accept_learn_restore();
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
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_accept_capture, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, pin, sd;

	ncpu = lb_ncpus();
	lb_accept_learn_save();
	lb_accept_learn_set(true);

	pin = ncpu > 1 ? 1 : 0;
	lb_pin(pin);
	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	ATF_REQUIRE_EQ(-1, lb_cpu_get(sd));

	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(pin, lb_cpu_get(sd));

	/* A later accept(2) does not move the tag. */
	lb_pin(0);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(pin, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_capture, tc)
{
	lb_accept_learn_restore();
}

/*
 * A thread that is not pinned to exactly one CPU says nothing, so existing
 * applications keep the distribution they have.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_unpinned);
ATF_TC_HEAD(lb_cpu_accept_unpinned, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_accept_unpinned, tc)
{
	struct sockaddr_storage ss;
	int error, sd;

	if (lb_thread_cpus() < 2)
		atf_tc_skip("the test thread is pinned to one CPU");
	lb_accept_learn_save();
	lb_accept_learn_set(true);

	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(-1, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_unpinned, tc)
{
	lb_accept_learn_restore();
}

/*
 * Nothing is learned when the behaviour is turned off.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_off);
ATF_TC_HEAD(lb_cpu_accept_off, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_accept_off, tc)
{
	struct sockaddr_storage ss;
	int error, ncpu, sd;

	ncpu = lb_ncpus();
	lb_accept_learn_save();
	lb_accept_learn_set(false);

	lb_pin(ncpu > 1 ? 1 : 0);
	sd = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	(void)lb_exchange(&ss, &sd, 1);
	ATF_REQUIRE_EQ(-1, lb_cpu_get(sd));

	error = close(sd);
	ATF_REQUIRE_MSG(error == 0, "close() failed: %s", strerror(errno));
}
ATF_TC_CLEANUP(lb_cpu_accept_off, tc)
{
	lb_accept_learn_restore();
}

/*
 * One listener being given a CPU must not starve the others, however few
 * CPUs the group is known to receive on.
 */
ATF_TC_WITH_CLEANUP(lb_cpu_accept_no_starve);
ATF_TC_HEAD(lb_cpu_accept_no_starve, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "is_exclusive", "true");
}
ATF_TC_BODY(lb_cpu_accept_no_starve, tc)
{
	struct sockaddr_storage ss;
	int acceptcnt[2], sds[2];
	int error, i, n, ncpu, tagged;
	const int nconns = 256;

	ncpu = lb_ncpus();
	lb_accept_learn_save();
	lb_accept_learn_set(true);

	lb_pin(ncpu > 1 ? 1 : 0);
	sds[0] = lb_bound_socket(PF_INET, SOCK_STREAM, 0, 0, &ss);
	sds[1] = lb_bound_socket(PF_INET, SOCK_STREAM, 0,
	    lb_addr_port(&ss), &ss);

	/* One accept(2) gives a CPU to whichever listener took it. */
	tagged = lb_exchange(&ss, sds, 2);
	ATF_REQUIRE_MSG(lb_cpu_get(sds[tagged]) != -1,
	    "the accepting listener was given no CPU");
	ATF_REQUIRE_EQ(-1, lb_cpu_get(sds[1 - tagged]));

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
ATF_TC_CLEANUP(lb_cpu_accept_no_starve, tc)
{
	lb_accept_learn_restore();
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
	ATF_TP_ADD_TC(tp, lb_cpu_export_ipv4);
	ATF_TP_ADD_TC(tp, lb_cpu_export_ipv6);
	ATF_TP_ADD_TC(tp, lb_cpu_steering);
	ATF_TP_ADD_TC(tp, lb_cpu_duplicates);
	ATF_TP_ADD_TC(tp, lb_cpu_untagged_share);
	ATF_TP_ADD_TC(tp, lb_cpu_partial_claims);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_capture);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_unpinned);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_off);
	ATF_TP_ADD_TC(tp, lb_cpu_accept_no_starve);

	return (atf_no_error());
}

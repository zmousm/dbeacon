/*
 * dbeacon, a Multicast Beacon
 *   dbeacon_posix.cpp
 *
 * Copyright (C) 2005 Hugo Santos
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:	Hugo Santos <hsantos@av.it.pt>
 */

#include "dbeacon.h"
#include "msocket.h"
#include "address.h"
#include "ptime.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netdb.h>

#if __linux__ || (__FreeBSD_version > 500042)

#if !defined(MCAST_JOIN_GROUP)
#define MCAST_JOIN_GROUP 42
struct group_req {
	uint32_t gr_interface;
	struct sockaddr_storage gr_group;
};
#endif

#if defined(MCAST_JOIN_GROUP) && !defined(MCAST_JOIN_SOURCE_GROUP)
#define MCAST_JOIN_SOURCE_GROUP 46
#define MCAST_LEAVE_SOURCE_GROUP 47
struct group_source_req {
	uint32_t gsr_interface;
	struct sockaddr_storage gsr_group;
	struct sockaddr_storage gsr_source;
};
#endif

#endif

static bool set_address(sockaddr_storage &t, const address &addr) {
	if (addr.family() == AF_INET)
		memcpy(&t, addr.v4(), addr.addrlen());
	else if (addr.family() == AF_INET6)
		memcpy(&t, addr.v6(), addr.addrlen());
	else
		return false;
	return true;
}

int MulticastListen(int sock, const address &grpaddr) {
#ifdef MCAST_JOIN_GROUP
	struct group_req grp;

	memset(&grp, 0, sizeof(grp));
	grp.gr_interface = mcastInterface;

	set_address(grp.gr_group, grpaddr);

	return setsockopt(sock, grpaddr.optlevel(), MCAST_JOIN_GROUP, &grp, sizeof(grp));
#else
	if (grpaddr.family() == AF_INET6) {
		ipv6_mreq mreq;
		mreq.ipv6mr_interface = mcastInterface;
		mreq.ipv6mr_multiaddr = grpaddr.v6()->sin6_addr;

		return setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));
	} else {
		ip_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		// Specifying the interface doesn't work, there's ip_mreqn in linux..
		// but what about other OSs? -hugo
		mreq.imr_multiaddr = grpaddr.v4()->sin_addr;

		return setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
	}
#endif
}

#ifdef MCAST_JOIN_SOURCE_GROUP
static int SSMJoinLeave(int sock, int type, const address &grpaddr, const address &srcaddr) {
	struct group_source_req req;
	memset(&req, 0, sizeof(req));

	req.gsr_interface = mcastInterface;

	set_address(req.gsr_group, grpaddr);
	set_address(req.gsr_source, srcaddr);

	return setsockopt(sock, srcaddr.optlevel(), type, &req, sizeof(req));
}

int SSMJoin(int sock, const address &grpaddr, const address &srcaddr) {
	return SSMJoinLeave(sock, MCAST_JOIN_SOURCE_GROUP, grpaddr, srcaddr);
}

int SSMLeave(int sock, const address &grpaddr, const address &srcaddr) {
	return SSMJoinLeave(sock, MCAST_LEAVE_SOURCE_GROUP, grpaddr, srcaddr);
}

#else

int SSMJoin(int sock, const address &grpaddr, const address &srcaddr) {
	errno = ENOPROTOOPT;
	return -1;
}

int SSMLeave(int sock, const address &grpaddr, const address &srcaddr) {
	errno = ENOPROTOOPT;
	return -1;
}

#endif

int SetupSocket(const address &addr, bool shouldbind, bool ssm) {
	int af_family = addr.family();
	int level = addr.optlevel();

	int sock = socket(af_family, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("Failed to create multicast socket");
		return -1;
	}

	int on = 1;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
		perror("setsockopt");
		return -1;
	}

	if (shouldbind) {
		if (bind(sock, addr.saddr(), addr.addrlen()) != 0) {
			perror("Failed to bind multicast socket");
			return -1;
		}
	}

#ifdef SO_TIMESTAMP
	if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on)) != 0) {
		perror("setsockopt(SO_TIMESTAMP)");
		return -1;
	}
#endif

	int type = level == IPPROTO_IPV6 ? IPV6_HOPLIMIT :
#ifdef IP_RECVTTL
				IP_RECVTTL;
#else
				IP_TTL;
#endif

	if (setsockopt(sock, level, type, &on, sizeof(on)) != 0) {
		perror("receiving hop limit/ttl setsockopt()");
		return -1;
	}

	TTLType ttl = defaultTTL;

	if (setsockopt(sock, level, level == IPPROTO_IPV6 ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
		perror(level == IPPROTO_IPV6 ?
			"setsockopt(IPV6_MULTICAST_HOPS)"
			: "setsockopt(IP_MULTICAST_TTL)");
		return -1;
	}

	if (!ssm && addr.is_multicast()) {
		if (MulticastListen(sock, addr) != 0) {
			perror("Failed to join multicast group");
			return -1;
		}
	}

	return sock;
}

int RecvMsg(int sock, address &from, uint8_t *buffer, int buflen, int &ttl, uint64_t &ts) {
	int len;
	struct msghdr msg;
	struct iovec iov;
	uint8_t ctlbuf[64];

	from.set_family(beaconUnicastAddr.family());

	msg.msg_name = (char *)from.saddr();
	msg.msg_namelen = from.addrlen();
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = (char *)ctlbuf;
	msg.msg_controllen = sizeof(ctlbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *)buffer;
	iov.iov_len = buflen;

	len = recvmsg(sock, &msg, 0);
	if (len < 0)
		return len;

	ts = 0;
	ttl = -1;

	if (msg.msg_controllen > 0) {
		for (cmsghdr *hdr = CMSG_FIRSTHDR(&msg); hdr; hdr = CMSG_NXTHDR(&msg, hdr)) {
			if (hdr->cmsg_level == IPPROTO_IPV6 && hdr->cmsg_type == IPV6_HOPLIMIT) {
				ttl = *(int *)CMSG_DATA(hdr);
			} else if (hdr->cmsg_level == IPPROTO_IP && hdr->cmsg_type == IP_RECVTTL) {
				ttl = *(uint8_t *)CMSG_DATA(hdr);
			} else if (hdr->cmsg_level == IPPROTO_IP && hdr->cmsg_type == IP_TTL) {
				ttl = *(int *)CMSG_DATA(hdr);
#ifdef SO_TIMESTAMP
			} else if (hdr->cmsg_level == SOL_SOCKET && hdr->cmsg_type == SO_TIMESTAMP) {
				timeval *tv = (timeval *)CMSG_DATA(hdr);
				ts = tv->tv_sec;
				ts *= 1000;
				ts += tv->tv_usec / 1000;
#endif
			}
		}
	}

	if (!ts) {
		ts = get_timestamp();
	}

	return len;
}

address::address() {
	memset(&stor, 0, sizeof(stor));
}

sockaddr_in *address::v4() { return (sockaddr_in *)&stor; }
sockaddr_in6 *address::v6() { return (sockaddr_in6 *)&stor; }

const sockaddr_in *address::v4() const { return (const sockaddr_in *)&stor; }
const sockaddr_in6 *address::v6() const { return (const sockaddr_in6 *)&stor; }

sockaddr *address::saddr() { return (sockaddr *)&stor; }
const sockaddr *address::saddr() const { return (const sockaddr *)&stor; }

int address::family() const {
	return stor.ss_family;
}

bool address::set_family(int family) {
	if (family != AF_INET && family != AF_INET6)
		return false;
	stor.ss_family = family;
	return true;
}

int address::optlevel() const {
	return stor.ss_family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
}

int address::addrlen() const {
	return stor.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

bool address::parse(const char *str, bool multicast, bool addport) {
	char tmp[128];
	strncpy(tmp, str, sizeof(tmp));

	char *port = strchr(tmp, '/');
	if (port) {
		*port = 0;
		port ++;
	} else if (addport) {
		port = (char *)defaultPort;
	}

	int cres;
	addrinfo hint, *res;
	memset(&hint, 0, sizeof(hint));

	hint.ai_family = forceFamily;
	hint.ai_socktype = SOCK_DGRAM;

	if ((cres = getaddrinfo(tmp, port, &hint, &res)) != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(cres));
		return false;
	}

	for (; res; res = res->ai_next) {
		set(res->ai_addr);
		if (multicast) {
			if (is_multicast())
				break;
		} else if (!is_unspecified())
			break;
	}

	if (!res) {
		fprintf(stderr, "No usable records for %s\n", tmp);
		return false;
	}

	return true;
}

bool address::is_multicast() const {
	if (stor.ss_family == AF_INET6)
		return IN6_IS_ADDR_MULTICAST(&v6()->sin6_addr);
	else if (stor.ss_family == AF_INET)
		return IN_CLASSD(htonl(v4()->sin_addr.s_addr));
	return false;
}

bool address::is_unspecified() const {
	if (stor.ss_family == AF_INET6)
		return IN6_IS_ADDR_UNSPECIFIED(&v6()->sin6_addr);
	else if (stor.ss_family == AF_INET)
		return v4()->sin_addr.s_addr == 0;
	return true;
}

void address::print(char *str, size_t len, bool printport) const {
	uint16_t port;

	if (stor.ss_family == AF_INET6) {
		inet_ntop(AF_INET6, &v6()->sin6_addr, str, len);
		port = ntohs(v6()->sin6_port);
	} else if (stor.ss_family == AF_INET) {
		inet_ntop(AF_INET, &v4()->sin_addr, str, len);
		port = ntohs(v4()->sin_port);
	} else {
		return;
	}

	if (printport)
		snprintf(str + strlen(str), len - strlen(str), "/%u", port);
}

bool address::is_equal(const address &a) const {
	if (stor.ss_family != a.stor.ss_family)
		return false;
	if (stor.ss_family == AF_INET6)
		return memcmp(&v6()->sin6_addr, &a.v6()->sin6_addr, sizeof(in6_addr)) == 0;
	else if (stor.ss_family == AF_INET)
		return v4()->sin_addr.s_addr == a.v4()->sin_addr.s_addr;
	return false;
}

int address::compare(const address &a) const {
	return memcmp(&stor, &a.stor, sizeof(stor));
}

void address::set(const sockaddr *sa) {
	stor.ss_family = sa->sa_family;
	if (stor.ss_family == AF_INET6) {
		v6()->sin6_addr = ((const sockaddr_in6 *)sa)->sin6_addr;
		v6()->sin6_port = ((const sockaddr_in6 *)sa)->sin6_port;
	} else {
		v4()->sin_addr = ((const sockaddr_in *)sa)->sin_addr;
		v4()->sin_port = ((const sockaddr_in *)sa)->sin_port;
	}
}

uint64_t get_timestamp() {
	struct timeval tv;
	uint64_t timestamp;

	if (gettimeofday(&tv, 0) != 0)
		return 0;

	timestamp = tv.tv_sec;
	timestamp *= 1000;
	timestamp += tv.tv_usec / 1000;

	return timestamp;
}

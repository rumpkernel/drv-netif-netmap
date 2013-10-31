/*-
 * Copyright (c) 2013 Luigi Rizzo.  All Rights Reserved.
 * Copyright (c) 2013 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/if.h>
#include <net/netmap.h>
#include <net/netmap_user.h>

#include <rump/rumpuser_component.h>

#include "if_virt.h"
#include "rumpcomp_user.h"

struct virtif_user {
	int viu_fd;
	int viu_dying;
	pthread_t viu_pt;

	struct virtif_sc *viu_virtifsc;

	void *nm_nifp; /* points to nifp if we use netmap */
	char *nm_mem;	/* redundant */
};

#ifdef NETMAPIF_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

static int source_hwaddr(const char *, uint8_t *);

static int
opennetmap(int devnum, struct virtif_user *viu, uint8_t *enaddr)
{
	int fd = -1;
	char *mydev;

	mydev = getenv("RUMP_NETIF");
	if (mydev) {
		struct nmreq req;
		int err;

		fprintf(stderr, "trying to use netmap on %s\n", mydev);

		fd = open("/dev/netmap", O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "Unable to open /dev/netmap\n");
			goto netmap_error;
		}
		bzero(&req, sizeof(req));
		req.nr_version = NETMAP_API;
		strncpy(req.nr_name, mydev, sizeof(req.nr_name));
		req.nr_ringid = NETMAP_NO_TX_POLL;
		err = ioctl(fd, NIOCREGIF, &req);
		if (err) {
			fprintf(stderr, "Unable to register %s errno  %d\n",
			    req.nr_name, errno);
			goto netmap_error;
		}
		fprintf(stderr, "need %d MB\n", req.nr_memsize >> 20);

		viu->nm_mem = mmap(0, req.nr_memsize,
		    PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
		if (viu->nm_mem == MAP_FAILED) {
			fprintf(stderr, "Unable to mmap\n");
			viu->nm_mem = NULL;
			goto netmap_error;
		}
		viu->nm_nifp = NETMAP_IF(viu->nm_mem, req.nr_offset);
		fprintf(stderr, "netmap:%s mem %d\n", mydev, req.nr_memsize);

		if (source_hwaddr(mydev, enaddr) != 0) {
			if (strncmp(mydev, "vale", 4) != 0) {
				fprintf(stderr, "netmap:%s: failed to retrieve "
				    "MAC address\n", mydev);
			}
		}
		return fd;
	}

 netmap_error:
	if (fd)
		close(fd);
	return -1;
}

/*
 * Note: this thread is the only one pulling packets off of any
 * given netmap instance
 */
static void *
receiver(void *arg)
{
	struct virtif_user *viu = arg;
	struct iovec iov;
	struct netmap_if *nifp = viu->nm_nifp;
	struct netmap_ring *ring = NETMAP_RXRING(nifp, 0);
	struct netmap_slot *slot;
	struct pollfd pfd;
	int prv;

	rumpuser_component_kthread();

	for (;;) {
		pfd.fd = viu->viu_fd;
		pfd.events = POLLIN;

		if (viu->viu_dying) {
			break;
		}

		prv = 0;
		while (ring->avail == 0 && prv == 0) {
			DPRINTF(("receive pkt via netmap\n"));
			prv = poll(&pfd, 1, 1000);
			if (prv > 0 || (prv < 0 && errno != EAGAIN))
				break;
		}
#if 0
		/* XXX: report non-transient errors */
		if (ring->avail == 0) {
			rv = errno;
			break;
		}
#endif
		slot = &ring->slot[ring->cur];
		DPRINTF(("got pkt of size %d\n", slot->len));
		iov.iov_base = NETMAP_BUF(ring, slot->buf_idx);
		iov.iov_len = slot->len;

		/* XXX: allow batch processing */
		rumpuser_component_schedule(NULL);
		rump_virtif_pktdeliver(viu->viu_virtifsc, &iov, 1);
		rumpuser_component_unschedule();

		ring->cur = NETMAP_RING_NEXT(ring, ring->cur);
		ring->avail--;
	}

	rumpuser_component_kthread_release();
	return NULL;
}

int
VIFHYPER_CREATE(int devnum, struct virtif_sc *vif_sc, uint8_t *enaddr,
	struct virtif_user **viup)
{
	struct virtif_user *viu = NULL;
	void *cookie;
	int rv;

	cookie = rumpuser_component_unschedule();

	viu = malloc(sizeof(*viu));
	if (viu == NULL) {
		rv = errno;
		goto out;
	}

	viu->viu_fd = opennetmap(devnum, viu, enaddr);
	if (viu->viu_fd == -1) {
		rv = errno;
		free(viu);
		goto out;
	}
	viu->viu_dying = 0;
	viu->viu_virtifsc = vif_sc;

	if ((rv = pthread_create(&viu->viu_pt, NULL, receiver, viu)) != 0) {
		printf("%s: pthread_create failed!\n",
		    VIF_STRING(VIFHYPER_CREATE));
		close(viu->viu_fd);
		free(viu);
	}

 out:
	rumpuser_component_schedule(cookie);

	*viup = viu;
	return rumpuser_component_errtrans(rv);
}

void
VIFHYPER_SEND(struct virtif_user *viu,
	struct iovec *iov, size_t iovlen)
{
	void *cookie = rumpuser_component_unschedule();
	struct netmap_if *nifp = viu->nm_nifp;
	struct netmap_ring *ring = NETMAP_TXRING(nifp, 0);
	char *p;
	int retries;

	DPRINTF(("sending pkt via netmap len %d\n", (int)iovlen));
	for (retries = 10; ring->avail == 0 && retries > 0; retries--) {
		struct pollfd pfd;
		int err;

		pfd.fd = viu->viu_fd;
		pfd.events = POLLOUT;
		DPRINTF(("cannot send on netmap, ring full\n"));
		err = poll(&pfd, 1, 500 /* ms */);
	}
	if (ring->avail > 0) {
		int i, totlen = 0;
		struct netmap_slot *slot = &ring->slot[ring->cur];
#define MAX_BUF_SIZE 1900
		p = NETMAP_BUF(ring, slot->buf_idx);
		for (i = 0; totlen < MAX_BUF_SIZE && i < iovlen; i++) {
			int n = iov[i].iov_len;
			if (totlen + n > MAX_BUF_SIZE) {
				n = MAX_BUF_SIZE - totlen;
				DPRINTF(("truncating long pkt"));
			}
			memcpy(p + totlen, iov[i].iov_base, n);
			totlen += n;
		}
#undef MAX_BUF_SIZE
		slot->len = totlen;
		ring->cur = NETMAP_RING_NEXT(ring, ring->cur);
		ring->avail--;
		if (ioctl(viu->viu_fd, NIOCTXSYNC, NULL) < 0)
			perror("NIOCTXSYNC");
	}

	rumpuser_component_schedule(cookie);
}

void
VIFHYPER_DYING(struct virtif_user *viu)
{

	/* no locking necessary.  it'll be seen eventually */
	viu->viu_dying = 1;
}

void
VIFHYPER_DESTROY(struct virtif_user *viu)
{
	void *cookie = rumpuser_component_unschedule();

	pthread_join(viu->viu_pt, NULL);
	close(viu->viu_fd);
	free(viu);

	rumpuser_component_schedule(cookie);
}

/* From netmap/examples/pkt-gen.c */
/*
 * Copyright (C) 2011-2012 Matteo Landi, Luigi Rizzo. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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

#ifdef __linux__
#include <netpacket/packet.h>

#define sockaddr_dl    sockaddr_ll
#define sdl_family     sll_family
#define AF_LINK        AF_PACKET
#define LLADDR(s)      s->sll_addr
#endif /* __linux__ */

/*
 * locate the src mac address for our interface, put it
 * into the user-supplied buffer. return 0 if ok, -1 on error.
 */
#define D(x, y)
static int
source_hwaddr(const char *ifname, uint8_t *enaddr)
{
	struct ifaddrs *ifaphead, *ifap;
	int l = sizeof(ifap->ifa_name);

	if (getifaddrs(&ifaphead) != 0) {
		D("getifaddrs %s failed", ifname);
		return (-1);
	}

	for (ifap = ifaphead; ifap; ifap = ifap->ifa_next) {
		struct sockaddr_dl *sdl =
			(struct sockaddr_dl *)ifap->ifa_addr;

		if (!sdl || sdl->sdl_family != AF_LINK)
			continue;
		if (strncmp(ifap->ifa_name, ifname, l) != 0)
			continue;
		memcpy(enaddr, LLADDR(sdl), 6);
		break;
	}
	freeifaddrs(ifaphead);
	return ifap ? 0 : 1;
}
#undef D

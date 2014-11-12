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

#ifndef _KERNEL
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <assert.h>
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
#include "virtif_user.h"

#if VIFHYPER_REVISION != 20140313
#error VIFHYPER_REVISION mismatch
#endif

struct virtif_user {
	struct virtif_sc *viu_virtifsc;

	int viu_fd;
	int viu_pipe[2];
	pthread_t viu_rcvthr;

	int viu_dying;

	struct netmap_if *nm_nifp;
	char *nm_mem;
	size_t nm_memsize;
};

#ifdef NETMAPIF_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

static int source_hwaddr(const char *, uint8_t *);

static int
opennetmap(const char *devstr, struct virtif_user *viu, uint8_t *enaddr)
{
	int fd = -1;
	struct nmreq req;
	int err = 0;

	/* fprintf(stderr, "trying to use netmap on %s\n", devstr); */

	fd = open("/dev/netmap", O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "Unable to open /dev/netmap\n");
		goto out;
	}

	bzero(&req, sizeof(req));
	req.nr_version = NETMAP_API;
	strncpy(req.nr_name, devstr, sizeof(req.nr_name));
	req.nr_ringid = NETMAP_NO_TX_POLL;
	err = ioctl(fd, NIOCREGIF, &req);
	if (err) {
		fprintf(stderr, "Unable to register %s errno  %d\n",
		    req.nr_name, errno);
		goto out;
	}
	/* fprintf(stderr, "need %d MB\n", req.nr_memsize >> 20); */

	viu->nm_memsize = req.nr_memsize;
	viu->nm_mem = mmap(0, req.nr_memsize,
	    PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
	if (viu->nm_mem == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap\n");
		viu->nm_mem = NULL;
		goto out;
	}
	viu->nm_nifp = NETMAP_IF(viu->nm_mem, req.nr_offset);
	/* fprintf(stderr, "netmap:%s mem %d\n", devstr, req.nr_memsize); */

	if (source_hwaddr(devstr, enaddr) != 0) {
		if (strncmp(devstr, "vale", 4) != 0) {
			fprintf(stderr, "netmap:%s: failed to retrieve "
			    "MAC address\n", devstr);
		}
	}

 out:
	if (err && fd != -1) {
		close(fd);
		fd = -1;
	}
	return fd;
}

static void
closenetmap(struct virtif_user *viu)
{
	if (viu->nm_mem && viu->nm_mem != MAP_FAILED)
		munmap(viu->nm_mem, viu->nm_memsize);
	if (viu->viu_fd >= 0) close(viu->viu_fd);
}

/*
 * Note: this thread is the only one pulling packets off of any
 * given netmap instance
 */
static void *
receiver(void *arg)
{
	struct virtif_user *viu = arg;
	struct pollfd pfd[2];
	struct iovec iov;
	struct netmap_if *nifp = viu->nm_nifp;
	struct netmap_ring *ring;
	struct netmap_slot *slot;
	int prv;
	int i;

	rumpuser_component_kthread();

	pfd[0].fd = viu->viu_fd;
	pfd[0].events = POLLIN;
	pfd[1].fd = viu->viu_pipe[0];
	pfd[1].events = POLLIN;

	while (!viu->viu_dying) {
		prv = poll(pfd, 2, -1);
		if (prv == 0)
			continue;
		if (prv == -1) {
			/* XXX */
			fprintf(stderr, "%s: poll error: %d\n",
			    nifp->ni_name, errno);
			sleep(1);
			continue;
		}
		if (pfd[1].revents & POLLIN)
			continue;

#if 0
		/* XXX: report non-transient errors */
		if (ring->avail == 0) {
			rv = errno;
			break;
		}
#endif
		for (i = 0; i < nifp->ni_rx_rings; i++) {
			ring = NETMAP_RXRING(nifp, i);
			while (!nm_ring_empty(ring)) {
				slot = &ring->slot[ring->cur];
				DPRINTF(("got pkt of size %d\n", slot->len));
				iov.iov_base = NETMAP_BUF(ring, slot->buf_idx);
				iov.iov_len = slot->len;

				/* XXX: allow batch processing */
				rumpuser_component_schedule(NULL);
				VIF_DELIVERPKT(viu->viu_virtifsc, &iov, 1);
				rumpuser_component_unschedule();

				ring->head = ring->cur = nm_ring_next(ring, ring->cur);
			}
		}
	}

	assert(viu->viu_dying);

	rumpuser_component_kthread_release();
	return NULL;
}

int
VIFHYPER_CREATE(const char *devstr, struct virtif_sc *vif_sc, uint8_t *enaddr,
	struct virtif_user **viup)
{
	struct virtif_user *viu = NULL;
	void *cookie;
	int rv;

	cookie = rumpuser_component_unschedule();

	viu = calloc(1, sizeof(*viu));
	if (viu == NULL) {
		rv = errno;
		goto oerr1;
	}
	viu->viu_virtifsc = vif_sc;

	viu->viu_fd = opennetmap(devstr, viu, enaddr);
	if (viu->viu_fd == -1) {
		rv = errno;
		goto oerr2;
	}

	if (pipe(viu->viu_pipe) == -1) {
		rv = errno;
		goto oerr3;
	}

	if ((rv = pthread_create(&viu->viu_rcvthr, NULL, receiver, viu)) != 0)
		goto oerr4;

	rumpuser_component_schedule(cookie);
	*viup = viu;
	return 0;

oerr4:
	close(viu->viu_pipe[0]);
	close(viu->viu_pipe[1]);
oerr3:
	closenetmap(viu);
oerr2:
	free(viu);
oerr1:
	rumpuser_component_schedule(cookie);
	return rumpuser_component_errtrans(rv);
}

void
VIFHYPER_SEND(struct virtif_user *viu, struct iovec *iov, size_t iovlen)
{
	void *cookie = NULL; /* XXXgcc */
	struct netmap_if *nifp = viu->nm_nifp;
	struct netmap_ring *ring = NETMAP_TXRING(nifp, 0);
	char *p;
	int retries;
	int unscheduled = 0;
	unsigned n;

	DPRINTF(("sending pkt via netmap len %d\n", (int)iovlen));
	for (retries = 10; !(n = nm_ring_space(ring)) && retries > 0; retries--) {
		struct pollfd pfd;

		if (!unscheduled) {
			cookie = rumpuser_component_unschedule();
			unscheduled = 1;
		}
		pfd.fd = viu->viu_fd;
		pfd.events = POLLOUT;
		DPRINTF(("cannot send on netmap, ring full\n"));
		(void)poll(&pfd, 1, 500 /* ms */);
	}
	if (n > 0) {
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
		ring->head = ring->cur = nm_ring_next(ring, ring->cur);
		if (ioctl(viu->viu_fd, NIOCTXSYNC, NULL) < 0)
			perror("NIOCTXSYNC");
	}

	if (unscheduled)
		rumpuser_component_schedule(cookie);
}

int
VIFHYPER_DYING(struct virtif_user *viu)
{

	void *cookie = rumpuser_component_unschedule();

	viu->viu_dying = 1;
	if (write(viu->viu_pipe[1],
			&viu->viu_dying, sizeof(viu->viu_dying)) == -1) {
		/*
		 * this is here mostly to avoid a compiler warning
		 * about ignoring the return value of write()
		 */
		fprintf(stderr, "%s: failed to signal thread\n",
			viu->nm_nifp->ni_name);
	}

	rumpuser_component_schedule(cookie);

	return 0;
}

void
VIFHYPER_DESTROY(struct virtif_user *viu)
{
	void *cookie = rumpuser_component_unschedule();

	pthread_join(viu->viu_rcvthr, NULL);
	closenetmap(viu);
	close(viu->viu_pipe[0]);
	close(viu->viu_pipe[1]);
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
#else
#include <net/if_dl.h>
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

#endif /* !_KERNEL */

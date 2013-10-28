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
#include <sys/uio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rump/rumpuser_component.h>

#include <inttypes.h>
#include <sys/mman.h>
#include <net/if.h>
#include <net/netmap.h>
#include <net/netmap_user.h>

#include "if_virt.h"
#include "rumpcomp_user.h"

struct virtif_user {
	int viu_fd;
	int viu_dying;

	void *nm_nifp; /* points to nifp if we use netmap */
	char *nm_mem;	/* redundant */
};

#ifdef NETMAPIF_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

static int
opennetmap(int devnum, struct virtif_user *viu)
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
		req.nr_ringid = 0;
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
		return fd;
	}

 netmap_error:
	if (fd)
		close(fd);
	return -1;
}

int
VIFHYPER_CREATE(int devnum, struct virtif_user **viup)
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

	viu->viu_fd = opennetmap(devnum, viu);
	if (viu->viu_fd == -1) {
		rv = errno;
		free(viu);
		goto out;
	}
	viu->viu_dying = 0;
	rv = 0;

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
		ioctl(viu->viu_fd, NIOCTXSYNC);
	}

	rumpuser_component_schedule(cookie);
}

int
VIFHYPER_RECV(struct virtif_user *viu,
	void *data, size_t dlen, size_t *rcv)
{
	void *cookie = rumpuser_component_unschedule();
	struct pollfd pfd;
	int rv, prv;

	pfd.fd = viu->viu_fd;
	pfd.events = POLLIN;

	for (;;) {
		struct netmap_if *nifp = viu->nm_nifp;
		struct netmap_ring *ring = NETMAP_RXRING(nifp, 0);
		struct netmap_slot *slot = &ring->slot[ring->cur];

		if (viu->viu_dying) {
			rv = 0;
			*rcv = 0;
			break;
		}

		prv = 0;
		while (ring->avail == 0 && prv == 0) {
			DPRINTF(("receive pkt via netmap\n"));
			prv = poll(&pfd, 1, 1000);
			if (prv > 0 || (prv < 0 && errno != EAGAIN))
				break;
		}
		if (ring->avail == 0) {
			rv = errno;
			break;
		}
		DPRINTF(("got pkt of size %d\n", slot->len));
		memcpy(data, NETMAP_BUF(ring, slot->buf_idx), slot->len);
		ring->cur = NETMAP_RING_NEXT(ring, ring->cur);
		ring->avail--;
		*rcv = (size_t)slot->len;
		rv = 0;
		break;
	}

	rumpuser_component_schedule(cookie);
	return rumpuser_component_errtrans(rv);
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

	close(viu->viu_fd);
	free(viu);

	rumpuser_component_schedule(cookie);
}

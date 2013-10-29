/*-
 * Copyright (c) 2004 Robert N. M. Watson
 * All rights reserved.
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
 *
 * $FreeBSD: src/tools/tools/netrate/netreceive/netreceive.c,v 1.4 2011/12/28 13:01:12 cognet Exp $
 */

#include <sys/types.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/poll.h>

#include <netinet/in.h>
#include <netdb.h>          /* getaddrinfo */

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         /* close */
#include <signal.h>

#include <rump/rump.h>
#include <rump/rump_syscalls.h>
#include <rump/netconfig.h>

#define MAXSOCK 1

long read_errors = 0;
long read_num = 0;

static void sig_handler(int signo)
{
	printf("received: %ld\n", read_num);
	printf("errors: %ld\n", read_errors);
	exit(0);
}

static void
usage(void)
{

	fprintf(stderr, "netreceive netmapif ip port\n");
	exit(-1);
}

int
main(int argc, char *argv[])
{
	struct sockaddr_in sin;
	char *dummy, *packet;
	int port;
	int error = 0;
	const char *cause = NULL;
	int s;
	struct pollfd fds[MAXSOCK];

	if (argc != 4)
		usage();

	rump_init();
	setenv("RUMP_NETIF", argv[1], 1);
	error = rump_pub_netconfig_ifcreate("netmap0");
	if (error)
		errx(1, "ifcreate %d", error);
	error = rump_pub_netconfig_ipv4_ifaddr("netmap0",
		argv[2], "255.255.255.0");
	if (error)
		errx(1, "interface configuration failed: %d\n", error);
	fprintf(stderr, "netmap0 configured!\n");


	port = strtoul(argv[3], &dummy, 10);
	if (port < 1 || port > 65535 || *dummy != '\0')
		usage();

	memset(&sin, 0, sizeof(sin));

	sin.sin_family = RUMP_AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = INADDR_ANY;

	packet = malloc(65536);
	if (packet == NULL) {
		cause = "malloc";
		goto out;
	}
	bzero(packet, 65536);

	s = rump_sys_socket(RUMP_PF_INET, RUMP_SOCK_DGRAM, 0);
	if (s < 0) {
		cause = "socket";
		goto out;
	}

	/*
	v = 1024 * 1024;
	if (setsockopt(s[nsock], SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) < 0) {
		cause = "SO_RCVBUF";
		close(s[nsock]);
		continue;
	}
	*/
	if (rump_sys_bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		cause = "bind";
		rump_sys_close(s);
		goto out;
	}
	(void) rump_sys_listen(s, 5);
	fds[0].fd = s;
	fds[0].events = POLLIN;

	printf("netreceive listening on UDP port %d\n", (u_short)port);
	signal(SIGINT, sig_handler);

	while (1) {
		if (rump_sys_poll(fds, 1, -1) < 0) 
			perror("poll");
		if (fds[0].revents & POLLIN) {
			if (rump_sys_recvfrom(s, packet, 65536, 0, NULL, 0) < 0) {
				read_errors++;
				perror("recv");
			} else {
				read_num++;
			}
		}
		if ((fds[0].revents &~ POLLIN) != 0)
			perror("poll");
	}
	
	
out:
	if (error) perror(cause);
	rump_sys_reboot(0, NULL);	
	return !!error;
}

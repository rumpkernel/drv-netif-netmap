/*
 * A very simple "netcat" workalike, to demonstrate one way to use
 * the netmap backing interface with a rump kernel TCP/IP stack.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <rump/rump.h>
#include <rump/rump_syscalls.h>
#include <rump/netconfig.h>

static void
usage(void)
{

	fprintf(stderr, "usage: netmapcat netmapif [ip|dhcp] [connect|listen] "
	    "[addr port|port]\n");
	exit(1);
}

static void
server(const char *port)
{
	char buf[1024];
	struct sockaddr_in sin;
	ssize_t nn;
	int s, s2;
	socklen_t slen;

	s = rump_sys_socket(RUMP_PF_INET, RUMP_SOCK_STREAM, 0);
	if (s == -1) {
		fprintf(stderr, "socket %d\n", errno);
		return;
	}

	memset(&sin, 0, sizeof(sin));

	sin.sin_family = RUMP_AF_INET;
	sin.sin_port = htons(atoi(port));
	sin.sin_addr.s_addr = INADDR_ANY;

	if (rump_sys_bind(s, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		fprintf(stderr, "bind %d\n", errno);
		return;
	}
	if (rump_sys_listen(s, 1) == -1) {
		fprintf(stderr, "listen %d\n", errno);
		return;
	}

	fprintf(stderr, "waiting for connection ...");
	slen = sizeof(sin);
	if ((s2 = rump_sys_accept(s, (struct sockaddr *)&sin, &slen)) == -1) {
		fprintf(stderr, "accept %d\n", errno);
		return;
	}

	fprintf(stderr, "connected!\n");

	while ((nn = rump_sys_read(s2, buf, sizeof(buf))) > 0) {
		if (write(STDOUT_FILENO, buf, nn) != nn) {
			fprintf(stderr, "stdout write failed\n");
			return;
		}
	}

	if (nn != 0) {
		fprintf(stderr, "socket read failed: %d\n", errno);
	} else {
		fprintf(stderr, "EOF\n");
	}
	rump_sys_close(s2);
}

static void
client(const char *addr, const char *port)
{
	char buf[1024];
	struct sockaddr_in sin;
	ssize_t nn;
	int s;

	s = rump_sys_socket(RUMP_PF_INET, RUMP_SOCK_STREAM, 0);
	if (s == -1) {
		fprintf(stderr, "socket %d\n", errno);
		return;
	}

	memset(&sin, 0, sizeof(sin));

	sin.sin_family = RUMP_AF_INET;
	sin.sin_port = htons(atoi(port));
	sin.sin_addr.s_addr = inet_addr(addr);

	fprintf(stderr, "connecting ... ");

	if (rump_sys_connect(s, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		fprintf(stderr, "connect failed: %d\n", errno);
		return;
	}
	fprintf(stderr, "connected!\n");

	while ((nn = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
		if (rump_sys_write(s, buf, nn) != nn) {
			fprintf(stderr, "socket write failed\n");
			return;
		}
	}

	if (nn != 0) {
		fprintf(stderr, "stdin read failed: %d\n", errno);
	} else {
		fprintf(stderr, "EOF\n");
	}
	rump_sys_close(s);
	sleep(1); /* give a chance for everything to be transmitted */
}

#define IS_SERVER (strcmp(role, "listen") == 0)

int
main(int argc, char *argv[])
{
	const char *netmapif = argv[1];
	const char *ifaddr = argv[2];
	const char *role = argv[3];
	int error;

	if (IS_SERVER && argc != 5)
		usage();
	if (!IS_SERVER && argc != 6)
		usage();

	rump_init();
	error = rump_pub_netconfig_ifcreate("netmap0");
	if (error)
		errx(1, "ifcreate %d", error);
	error = rump_pub_netconfig_ifsetlinkstr("netmap0", netmapif);
	if (error)
		errx(1, "linkstr %d", error);

	if (strcmp(ifaddr, "dhcp") == 0) {
		error = rump_pub_netconfig_dhcp_ipv4_oneshot("netmap0");
	} else {
		error = rump_pub_netconfig_ipv4_ifaddr("netmap0",
		    ifaddr, "255.255.255.0");
	}
	if (error)
		errx(1, "interface configuration failed: %d\n", error);

	fprintf(stderr, "netmap0 configured!\n");

	if (IS_SERVER)
		server(argv[4]);
	else
		client(argv[4], argv[5]);

	rump_sys_reboot(0, NULL);
	exit(0);
}

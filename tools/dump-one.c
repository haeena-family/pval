/*
 * readv from a pval chardev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <poll.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include <pval.h>

#define BULKNUM	16

void parse_and_print(struct pval_slot *slot)
{
	struct ethhdr *eth;
	struct iphdr *iph;
	char abuf1[16], abuf2[16];

	printf("%llu ", slot->tstamp);
	
	eth = (struct ethhdr *)slot->pkt;
	printf("%02x:%02x:%02x:%02x:%02x:%02x -> "
	       "%02x:%02x:%02x:%02x:%02x:%02x Type%04x",
	       eth->h_source[0], eth->h_source[1], eth->h_source[2],
	       eth->h_source[3], eth->h_source[4], eth->h_source[5],
	       eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
	       eth->h_dest[3], eth->h_dest[4], eth->h_dest[5],
	       ntohs(eth->h_proto));

	if (ntohs(eth->h_proto) != ETH_P_IP)
		goto out;

	printf(", ");

	iph = (struct iphdr *)(eth + 1);
	inet_ntop(AF_INET, &iph->saddr, abuf1, sizeof(abuf1));
	inet_ntop(AF_INET, &iph->daddr, abuf2, sizeof(abuf2));
	printf("%s -> %s", abuf1, abuf2);

out:
	printf("\n");
}

int main(int argc, char **argv)
{
	int fd, n, ret;
	struct pval_slot slots[BULKNUM];
	struct iovec iov[BULKNUM];
	struct pollfd x;

	if (argc < 2) {
		printf("%s [Pval chardev]\n", argv[0]);
		return -1;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		printf("%s\n", argv[1]);
		perror("open");
		return -1;
	}

	for (n = 0; n < BULKNUM; n++) {
		memset(&slots[n], 0, sizeof(struct pval_slot));
		iov[n].iov_base = &slots[n];
		iov[n].iov_len = sizeof(struct pval_slot);
	}

	x.fd = fd;
	x.events = POLLIN;

	while (1) {
		if (poll(&x, 1, 1000) < 0) {
			perror("poll");
			return -1;
		}

		if (!x.revents & POLLIN)
			continue;

		ret = readv(fd, iov, BULKNUM);
		if (ret < 0) {
			perror("readv");
			continue;
		}

		for (n = 0; n < ret; n++)
			parse_and_print(&slots[n]);
	}

	return 0;
}

/*
 * readv from multiple pval char devices
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <poll.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <limits.h>
#include <pthread.h>

#include <pval.h>

#define BULKNUM	16

static int caught_signal = 0;

struct thread_body {
	char path[PATH_MAX];	/* path of char dev */
	int cpu;	/* CPU this thread run */
};

void parse_and_print(struct pval_slot *slot, char *prefix)
{
	struct ethhdr *eth;
	struct iphdr *iph;
	char abuf1[16], abuf2[16];

	printf("%s: TS=%llu ", prefix, slot->tstamp);
	
	eth = (struct ethhdr *)slot->pkt;
	printf("%02x:%02x:%02x:%02x:%02x:%02x -> "
	       "%02x:%02x:%02x:%02x:%02x:%02x Type 0x%04x ",
	       eth->h_source[0], eth->h_source[1], eth->h_source[2],
	       eth->h_source[3], eth->h_source[4], eth->h_source[5],
	       eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
	       eth->h_dest[3], eth->h_dest[4], eth->h_dest[5],
	       ntohs(eth->h_proto));

	if (ntohs(eth->h_proto) != ETH_P_IP)
		goto out;


	iph = (struct iphdr *)(eth + 1);
	inet_ntop(AF_INET, &iph->saddr, abuf1, sizeof(abuf1));
	inet_ntop(AF_INET, &iph->daddr, abuf2, sizeof(abuf2));
	printf("%s -> %s", abuf1, abuf2);

out:
	printf("\n");
}

void * dump_thread(void *param)
{
	int fd, n, ret;
	struct thread_body *tb = (struct thread_body *)param;
	struct pollfd x;
	struct iovec iov[BULKNUM];
	struct pval_slot slots[BULKNUM];

	fd = open(tb->path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", tb->path);
		perror("open");
		return NULL;
	}

	for (n = 0; n < BULKNUM; n++) {
		memset(&slots[n], 0, sizeof(struct pval_slot));
		iov[n].iov_base = &slots[n];
		iov[n].iov_len = sizeof(struct pval_slot);
	}

	x.fd = fd;
	x.events = POLLIN;

	while (1) {

		if (caught_signal)
			break;

		if (poll(&x, 1, 1000) < 0) {
			perror("poll");
			return NULL;
		}

		if (!x.revents & POLLIN)
			continue;

		ret = readv(fd, iov, BULKNUM);
		if (ret < 0) {
			perror("readv");
			continue;
		}

		for (n = 0; n < ret; n++)
			parse_and_print(&slots[n], tb->path);
	}

	return NULL;
}

void sig_handler(int sig)
{
	if (sig == SIGINT)
		caught_signal = 1;
}

int main(int argc, char **argv)
{
	int n, ret, num_of_th = argc - 1;
	pthread_t tids[32];
	struct thread_body tbs[32];

	if (argc < 2) {
		printf("%s [Pval chardev] [Pval chardev] ...\n", argv[0]);
		return -1;
	}

	for (n = 0; n < num_of_th; n++) {
		strncpy(tbs[n].path, argv[n + 1], PATH_MAX);
		ret = pthread_create(&tids[n], NULL, dump_thread, &tbs[n]);
		if (ret) {
			perror("pthread_create");
			return ret;
		}
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("cannot set signal");
		return -1;
	}

	for (n = 0; n < num_of_th; n++)
		pthread_join(tids[n], NULL);

	return 0;
}

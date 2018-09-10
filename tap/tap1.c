#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/errqueue.h>
#include <poll.h>


void print_pkt(long long unsigned int hwts, char *prefix,
		       char *pkt, int pktlen)
{
	char out[256], buf[256];
        struct ethhdr   *eth;
        struct iphdr    *iph;
        struct tcphdr   *tcp;

        eth = (struct ethhdr *)pkt;
        snprintf(out, sizeof(out), "%s TS=%llu PKTLEN=%d ETHER_TYPE=0x%04x ",
                 prefix, hwts, pktlen, ntohs(eth->h_proto));
        if (ntohs(eth->h_proto) != ETH_P_IP)
                goto out;


        iph = (struct iphdr *)(eth + 1);
        snprintf(buf, sizeof(buf), "IP_PROTO=%u ", iph->protocol);
        strncat(out, buf, sizeof(out));
        if (iph->protocol != IPPROTO_TCP)
                goto out;

        tcp = (struct tcphdr *)(((char*)iph) + (iph->ihl << 2));
        snprintf(buf, sizeof(buf), "SPORT=%u DPORT=%u SYN=%u ACK=%u PSH=%u RST=%u FIN=%u",
		 ntohs(tcp->source), ntohs(tcp->dest),
                 tcp->syn, tcp->ack, tcp->psh, tcp->rst, tcp->fin);
        strncat(out, buf, sizeof(out));
out:
        printf("%s\n", out);

}

int hwtstamp_config_set(const char *ifname, int tx_type, int rx_filter)
{
        struct hwtstamp_config config;
        int sock, ret = 0;
        struct ifreq ifr;

        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
                perror("socket");
                ret = -1;
                goto out;
        }

        config.flags = 0;
        config.tx_type = tx_type;
        config.rx_filter = rx_filter;
        strcpy(ifr.ifr_name, ifname);
        ifr.ifr_data = (caddr_t)&config;

        if (ioctl(sock, SIOCSHWTSTAMP, &ifr)) {
                perror("ioctl");
                ret = -1;
                goto out;
        }

out:
        return ret;
}


uint64_t get_tstamp(int fd) {

	uint64_t hwts;
	struct scm_timestamping *ts = NULL;
	struct sockaddr_in sin;
	struct iovec iov;
	struct msghdr msg = {0};
	char ctrl[1024] = {};
	char buf[2048] = {};
	int level, type;
	int count;
	struct cmsghdr *cm;

        iov.iov_base = &buf;
        iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &ctrl;
        msg.msg_controllen = sizeof(ctrl);
        msg.msg_name = (caddr_t)&sin;
        msg.msg_namelen = sizeof(sin);

	usleep(1000);

	count = recvmsg(fd, &msg, MSG_ERRQUEUE);
        if (count < 0) {
                printf("%s:%s: %s\n", __func__, "recvmsg", strerror(errno));
                goto out;
        }

        for (cm = CMSG_FIRSTHDR(&msg); cm != NULL;
	     cm = CMSG_NXTHDR(&msg, cm)) {
                level = cm->cmsg_level;
                type = cm->cmsg_type;
                if (level == SOL_SOCKET && type == SO_TIMESTAMPING) {
                        ts = (struct scm_timestamping *)CMSG_DATA(cm);
			hwts = ts->ts[2].tv_sec * 1000000000 +
				ts->ts[2].tv_nsec;
			return hwts;
                }
        }

out:
	return 0;
}

int tap_alloc(char *dev) {
	struct ifreq ifr;
	int fd, ret;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		perror("failed to open /dev/net/tun");
		return fd;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if ((ret = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
		perror("failed to create tap device via ioctl()");
		close(fd);
		return ret;
	}

	return fd;
}


int raw_alloc(char *dev) {
	int fd;
	struct sockaddr_ll sll;
	unsigned int ifindex;

	ifindex = if_nametoindex(dev);
	if (ifindex == 0) {
		fprintf(stderr, "invalid dev name %s: %s\n", dev,
			strerror(errno));
		return errno;
	}

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0)
		return fd;
	
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifindex;
	sll.sll_pkttype = PACKET_HOST;
	sll.sll_halen = ETH_ALEN;

	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		fprintf(stderr, "failed to bind packet socket: %s\n",
			strerror(errno));
		return errno;
	}

	// enable h/w timestamp
        enum _rx_mode { NONE, ALL } rx_mode = ALL;
        enum _tx_mode { OFF, ON } tx_mode = ON;	
	hwtstamp_config_set(dev, tx_mode, rx_mode); // TX on, RX ALL


	unsigned int skopt = (SOF_TIMESTAMPING_TX_HARDWARE |
			      SOF_TIMESTAMPING_RX_HARDWARE |
			      SOF_TIMESTAMPING_RAW_HARDWARE);


	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, (char *)&skopt,
		       sizeof(skopt)))
		perror("setsockopt for SO_TIMESTAMPING");

/*
	unsigned int errqopt = txskopt | rxskopt;
	if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, (char *)&errqopt,
		       sizeof(errqopt)))
		perror("setsockopt for SO_SELECT_ERR_QUEUE");
*/

	return fd;
}

int main(int argc, char **argv)
{
	int tap_fd, raw_fd, ret;
	char buf[2048];
	struct pollfd x[2];
	struct sockaddr_ll sll;

	long long unsigned int hwts;

	/* buffer for rxtstamp */
	struct iovec iov;
	struct sockaddr_in sin;
	struct scm_timestamping *ts;
	struct msghdr msg = {0};
        char ctrl[512] = {};
        int level, type;
	struct cmsghdr *cm;

        iov.iov_base = &buf;
        iov.iov_len = sizeof(buf);

        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &ctrl;
        msg.msg_controllen = sizeof(ctrl);
        msg.msg_name = (caddr_t)&sin;
        msg.msg_namelen = sizeof(sin);


	if (argc < 3) {
		fprintf(stderr, "%s [tap dev] [raw dev]\n", argv[0]);
		return -1;
	}

	tap_fd = tap_alloc(argv[1]);
	if (tap_fd < 0)
		return tap_fd;
	
	raw_fd = raw_alloc(argv[2]);
	if (raw_fd < 0)
		return raw_fd;

	x[0].fd = tap_fd;
	x[0].events = POLLIN;
	x[1].fd = raw_fd;
	x[1].events = POLLIN;

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_halen = ETH_ALEN;
	sll.sll_ifindex = if_nametoindex(argv[2]);

	while (1) {
		poll(x, 2, 1000);

		if (x[0].revents & POLLIN) {
			/* tap -> raw */
			ret = read(x[0].fd, buf, sizeof(buf));
			if (ret <= 0)
				perror("read from tap");
			else {
				ret = sendto(x[1].fd, buf, ret, 0,
					     (struct sockaddr *)&sll,
					     sizeof(sll));
				if (ret <= 0)
					perror("write to raw");
				else {
					hwts = get_tstamp(x[1].fd);
					print_pkt(hwts, "tx", buf, ret);
				}
			}
		}

		if (x[1].revents & POLLIN) {
			/* raw -> tap */
			ret = recvmsg(x[1].fd, &msg, 0);
			if (ret <= 0) {
				perror("read from raw");
				continue;
			}

			/* retrieve rxtstamp */
			hwts = 0;
			for (cm = CMSG_FIRSTHDR(&msg);
			     cm != NULL;
			     cm = CMSG_NXTHDR(&msg, cm)) {
				level = cm->cmsg_level;
				type = cm->cmsg_type;
				if (level == SOL_SOCKET &&
				    type == SO_TIMESTAMPING) {
					ts = (struct scm_timestamping *)
						CMSG_DATA(cm);
					hwts = ts->ts[2].tv_sec * 1000000000 +
					       ts->ts[2].tv_nsec;
				}
			}

			print_pkt(hwts, "rx", buf, ret);

			ret = write(x[0].fd, buf, ret);
			if (ret <= 0)
				perror("write to tap");
		}
	}


	return 0;
}


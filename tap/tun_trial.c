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
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <poll.h>


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


void get_tstamp(int fd) {

	struct timespec *tstmp = NULL;
	struct sockaddr_in sin;
	struct iovec iov;
	struct msghdr msg = {0};
	char ctrl[512] = {};
	char buf[256] = {};
	int level, type;
	ssize_t count;
	struct cmsghdr *cm;

        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &ctrl;
        msg.msg_controllen = sizeof(ctrl);
        msg.msg_name = (caddr_t)&sin;
        msg.msg_namelen = sizeof(sin);

        iov.iov_base = &buf;
        iov.iov_len = sizeof(buf);

	count = recvmsg(fd, &msg, MSG_ERRQUEUE);
        if (count < 1) {
                printf("%s: %s\n", "recvmsg", strerror(errno));
                goto out;
        } else {
                printf("recvmsg: success\n");
        }

        for (cm = CMSG_FIRSTHDR(&msg); cm != NULL;
	     cm = CMSG_NXTHDR(&msg, cm)) {
                level = cm->cmsg_level;
                type = cm->cmsg_type;
                if (level == SOL_SOCKET && type == SO_TIMESTAMPING) {
                        tstmp = (struct timespec *)CMSG_DATA(cm);
			printf("%lu\n",
			       tstmp->tv_sec * 1000000000 + tstmp->tv_nsec);

                        goto out;
                }
        }

out:
	return;
}

int tap_alloc(char *dev) {
	struct ifreq ifr;
	int fd, ret;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		perror("failed to open /dev/net/tun");
		return fd;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
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

	fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		perror("socket SOCK_RAW");
		return fd;
	}
	
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, dev, strlen(dev)))
		perror("setsockopt for SO_BINDTODEVICe");


	// enable h/w timestamp
        enum _rx_mode { NONE, ALL } rx_mode = ALL;
        enum _tx_mode { OFF, ON } tx_mode = ON;	
	hwtstamp_config_set(dev, tx_mode, rx_mode); // TX on, RX ALL


	unsigned int txskopt;
	txskopt = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, (char *)&txskopt,
		       sizeof(txskopt)))
		perror("setsockopt for SO_TIMESTAMPING");

	if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, (char *)&txskopt,
		       sizeof(txskopt)))
		perror("setsockopt for SO_SELECT_ERR_QUEUE");


	unsigned int rxskopt;
	rxskopt = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, (char *)&rxskopt,
		       sizeof(rxskopt)))
		perror("setsockopt for SO_TIMESTAMPING");

	if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, (char *)&rxskopt,
		       sizeof(rxskopt)))
		perror("setsockopt for SO_SELECT_ERR_QUEUE");

	return fd;
}

int main(int argc, char **argv)
{
	int tap_fd, raw_fd, ret;
	char buf[2048];
	struct pollfd x[2];


	/* buffer for rxtstamp */
	struct iovec iov;
	struct sockaddr_in sin;
	struct timespec *tstmp;
	struct msghdr msg = {0};
        char ctrl[512] = {};
        int level, type;
	struct cmsghdr *cm;
	struct sockaddr_in dst;
	struct ip *iph;

        iov.iov_base = &buf;
        iov.iov_len = sizeof(buf);

        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &ctrl;
        msg.msg_controllen = sizeof(ctrl);
        msg.msg_name = (caddr_t)&sin;
        msg.msg_namelen = sizeof(sin);

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;


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

	while (1) {
		poll(x, 2, 1000);

		if (x[0].revents & POLLIN) {
			/* tap -> raw */
			ret = read(x[0].fd, buf, sizeof(buf));
			if (ret <= 0)
				perror("read from tap");
			else {
			
				iph = (struct ip *)buf;
				if (iph->ip_v != 4) {
					printf("tap->raw not ipv4 packet\n");
					continue;
				}
				dst.sin_addr = iph->ip_dst;
				printf("tap->raw\n");
				printf("src: %s\n", inet_ntoa(iph->ip_src));
				printf("dst: %s\n", inet_ntoa(iph->ip_dst));

				ret = sendto(x[1].fd, buf, ret, 0,
					     (struct sockaddr *)&dst,
					     sizeof(dst));

				if (ret <= 0)
					perror("write to raw");
				else {
					printf("tap->raw %d byte\n", ret);
					get_tstamp(x[1].fd);
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
			for (cm = CMSG_FIRSTHDR(&msg);
			     cm != NULL;
			     cm = CMSG_NXTHDR(&msg, cm)) {
				level = cm->cmsg_level;
				type = cm->cmsg_type;
				if (level == SOL_SOCKET &&
				    type == SO_TIMESTAMPING) {
					tstmp = (struct timespec *)
						CMSG_DATA(cm);
					printf("%lu\n",
					       tstmp->tv_sec * 1000000000 +
					       tstmp->tv_nsec);
					
				}
			}

			ret = write(x[0].fd, buf, ret);
			if (ret <= 0)
				perror("write to tap");
			else {
				printf("raw->tap %d byte\n", ret);
			}
		}
	}


	return 0;
}


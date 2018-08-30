

### Pval kernel module

1. Compile and install pval.ko

```shell-session
$ git clone https://github.com/haeena-family/pval
$ cd pval/kmod
$ make
$ sudo insmod pval.ko
```

2. Compile the modified iproute2

```shell-session
$ sudo apt install xtables-addons-source flex bison
$ cd pval/iproute2-4.18.0
$ ./configure
$ make
$ sudo ./ip/ip link add type pval link enp0s9
$ sudo ip -d link show dev pval0
25: pval0: <BROADCAST,MULTICAST> mtu 1500 qdisc noqueue state DOWN mode DEFAULT group default qlen 1000
    link/ether 2e:4a:6f:78:49:09 brd ff:ff:ff:ff:ff:ff promiscuity 0 
    pval link enp0s9 addrgenmode eui64 numtxqueues 1 numrxqueues 1 gso_max_size 65536 gso_max_segs 65535
$ sudo ip link set dev pval0 up
$ sudo ip addr add dev pval0 10.0.0.3/24
```

pval0 interface enslaves enp0s9 ethernet interface. All packets
receved from enp0s9 are received from pval0, and all transmitted
packets to pval0 is transmitted through enp0s9. This relationship is
similar to ethernet and bridge interfaces.


3. what's happen

Pval interface embeds IP Pval Option (experimental option 222) into
all transmitted packets. pval/tcpdump is capable to see this option.

```shell-session
$ sudo apt install libpcap-dev
$ cd pval/tcpdump
$ ./configure
$ make
$ sudo ./tcpdump -nvvvi enp0s9 icmp
tcpdump: listening on enp0s9, link-type EN10MB (Ethernet), capture size 262144 bytes
15:39:06.490227 IP (tos 0x0, ttl 64, id 61629, offset 0, flags [DF], proto ICMP (1), length 84)
    10.0.0.2 > 10.0.0.3: ICMP echo request, id 21554, seq 172, length 64
15:39:06.490251 IP (tos 0x0, ttl 64, id 9776, offset 0, flags [none], proto ICMP (1), length 96, options (pval seq 357))
    10.0.0.3 > 10.0.0.2: ICMP echo reply, id 21554, seq 172, length 64
15:39:07.514330 IP (tos 0x0, ttl 64, id 61715, offset 0, flags [DF], proto ICMP (1), length 84)
    10.0.0.2 > 10.0.0.3: ICMP echo request, id 21554, seq 173, length 64
15:39:07.514355 IP (tos 0x0, ttl 64, id 9930, offset 0, flags [none], proto ICMP (1), length 96, options (pval seq 358))
```

ICMP echo reply packets from 10.0.0.3 (pval0) to 10.0.0.2 have Pval
options.
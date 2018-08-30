

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
```

pval0 interface enslaves enp0s9 ethernet interface. All packets
receved from enp0s9 are received to pval0, and all transmitted packets
to pval0 is transmitted through enp0s9. This relationship is similar
to ethernet and bridge interfaces.

Pval interface embeds IP Pval Option (experimental option 222) into
all transmitted packets. pval/tcpdump is capable to see this option
(it requires libpcap-dev).

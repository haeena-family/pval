

## Pval kernel module

### 1. Compile and install pval.ko

```shell-session
$ git clone https://github.com/haeena-family/pval
$ cd pval/kmod
$ make
$ sudo insmod pval.ko
```

### 2. Compile the modified iproute2

```shell-session
$ sudo apt install xtables-addons-source flex bison
$ cd pval/iproute2-4.18.0
$ ./configure
$ make
$ ./ip/ip link add type pval help
Usage: ... pval link PHYS_DEV
                 [ ipopt { on | off } ]
                 [ txtstamp { on | off } ]
                 [ rxtstamp { on | off } ]
                 [ txcopy { on | off } ]
                 [ rxcopy { on | off } ]
$ sudo ./ip/ip link add type pval link enp0s9
$ sudo ip -d link show dev pval0
25: pval0: <BROADCAST,MULTICAST> mtu 1500 qdisc noqueue state DOWN mode DEFAULT group default qlen 1000
    link/ether 2e:4a:6f:78:49:09 brd ff:ff:ff:ff:ff:ff promiscuity 0 
    pval link enp0s9 addrgenmode eui64 numtxqueues 1 numrxqueues 1 gso_max_size 65536 gso_max_segs 65535
$ sudo ip link set dev pval0 up
$ ip -d link show dev pval0
39: pval0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/ether 92:b7:3a:2d:ff:e1 brd ff:ff:ff:ff:ff:ff promiscuity 0 
    pval link enp0s9 ipopt off txtstamp off rxtstamp off txcopy off rxcopy off addrgenmode eui64 numtxqueues 1 numrxqueues 1 gso_max_size 65536 gso_max_segs 65535

$ sudo ip addr add dev pval0 10.0.0.3/24
```

pval0 interface enslaves enp0s9 ethernet interface. All packets
receved from enp0s9 are received from pval0, and all transmitted
packets to pval0 are transmitted through enp0s9. This relationship is
similar to ethernet and bridge interfaces.


### 3. Configure pval0 interface

```shell-session
$ sudo ./ip/ip link set dev pval0 type pval ipopt on
$ ./ip/ip -d link show dev pval0
39: pval0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/ether 92:b7:3a:2d:ff:e1 brd ff:ff:ff:ff:ff:ff promiscuity 0 
    pval link enp0s9 ipopt on txtstamp off rxtstamp off txcopy off rxcopy off addrgenmode eui64 numtxqueues 1 numrxqueues 1 gso_max_size 65536 gso_max_segs 65535 
```

You can configure pval interfaces through `ip link set`. `ipopt on`
enables inserting Pval IP Option to transmitting packets. Other
options are shown in the help (but not implemented currently).  These
options can be specified at link creation by `ip link add`.


### 4. What's happen

Pval interface embeds IP Pval Option (experimental option 222) into
all transmitted packets. pval/tcpdump is capable to see this option.

```shell-session
$ sudo apt install libpcap-dev
$ cd pval/tcpdump
$ ./configure
$ make
$ sudo ./tcpdump -nvvvi enp0s9
tcpdump: listening on enp0s9, link-type EN10MB (Ethernet), capture size 262144 bytes
16:04:05.596047 IP (tos 0x0, ttl 64, id 48565, offset 0, flags [DF], proto ICMP (1), length 84)
    10.0.0.2 > 10.0.0.3: ICMP echo request, id 21554, seq 1636, length 64
16:04:05.596071 IP (tos 0x0, ttl 64, id 2758, offset 0, flags [none], proto ICMP (1), length 96, options (pval cpu 0, seq 46))
    10.0.0.3 > 10.0.0.2: ICMP echo reply, id 21554, seq 1636, length 64
16:04:06.621014 IP (tos 0x0, ttl 64, id 48781, offset 0, flags [DF], proto ICMP (1), length 84)
    10.0.0.2 > 10.0.0.3: ICMP echo request, id 21554, seq 1637, length 64
16:04:06.621036 IP (tos 0x0, ttl 64, id 2789, offset 0, flags [none], proto ICMP (1), length 96, options (pval cpu 0, seq 47))
```

ICMP echo reply packets from 10.0.0.3 (pval0) to 10.0.0.2 have Pval
options.


### 5. Gathring copied packets

`txcopy` and `rxcopy` copy TXed and RXed packets through the pval
interfaces. You can optain the copied packets from the Pval character
devices.

```shell-session
$ ls /dev/pval
pval0-rx-cpu-0	pval0-tx-cpu-0
```

After pval interfaces are created, you can see the associated charater
devices at /dev/pval directory. The character devices are created for
TX, RX for each CPU (queue). Applications can obtain the copied
packets with hardware timestamps (when `txtstamp` and/or `rxtstamp`
option is enabled on the interfaces, but not currently tested).

Read API of the character device is a bit different from the
traditional sysmte call. The API provides bulked packet read through
the slightly modified usage of `writev()` system call.
tools/dump-one.c is a sample application. An iovec contains a `struct
pval_slot` as a structured buffer, and `writev()` returns how many
packets are read. This method diverts schatter gather I/O to bulked
packet transfer.

```shell-session
$ cd pval
$ sudo ./iproute2-4.18.0/ip/ip link set dev pval0 type pval txcopy on rxcopy on
$ cd tools
$ make
$ sudo ./dump-one /dev/pval/pval0-rx-cpu-0
0 08:00:27:19:12:a4 -> ea:51:d7:0c:95:1a Type0800, 10.0.0.2 -> 10.0.0.3
0 08:00:27:19:12:a4 -> ea:51:d7:0c:95:1a Type0800, 10.0.0.2 -> 10.0.0.3
0 08:00:27:19:12:a4 -> ea:51:d7:0c:95:1a Type0800, 10.0.0.2 -> 10.0.0.3
0 08:00:27:19:12:a4 -> ea:51:d7:0c:95:1a Type0800, 10.0.0.2 -> 10.0.0.3
^C
$ sudo ./dump-one /dev/pval/pval0-tx-cpu-0
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0806
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0800, 10.0.0.3 -> 10.0.0.2
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0800, 10.0.0.3 -> 10.0.0.2
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0806
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0800, 10.0.0.3 -> 10.0.0.2
0 ea:51:d7:0c:95:1a -> 08:00:27:19:12:a4 Type0800, 10.0.0.3 -> 10.0.0.2
^C
```
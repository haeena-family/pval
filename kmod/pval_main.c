/* pval_main.c */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/rculist.h>
#include <linux/etherdevice.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#include <net/genetlink.h>
#include <net/ip_tunnels.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <uapi/linux/limits.h>
#include <uapi/linux/if.h>
#include <uapi/linux/net_tstamp.h>
#include <asm/string.h>

#include <pval.h>

#define PVAL_VERSION 	"0.0.1"
#define DRV_NAME	"pval"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


const static struct net_device_ops pdev_netdev_ops;


/* structures describing pval ring buffer */
struct pval_ring {
	u8	cpu;
	u32	head;	/* write point */
	u32	tail;	/* read point */
	u32	mask;	/* bit mask of the ring buffer */

	struct pval_slot *slots;	/* array of pval slot */
};
#define PVAL_SLOT_NUM	1024	/* length of a ring (num of slots) */



/* structure describing pval misc device. TX/RX on per CPU */
#define PVAL_NAME_MAX	(IFNAMSIZ + 16)
struct pval_mdev {
	char	name[PVAL_NAME_MAX];	/* IFNAM-{tx|rx}-cpu-%d */
	int	cpu;			/* CPU where this ring allocated */
	bool	opened;			/* This miscdev is opend.
					 * XXX: this should be atmoic value
					 * to avoid race condition.
					 */

	struct pval_ring	ring;
	struct miscdevice	mdev;
};

/* waitqueue for poll */
static DECLARE_WAIT_QUEUE_HEAD(pval_wait);


/* structure describing pval device */
#define PVAL_MAX_CPUS	16

struct pval_dev {
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;

	struct net_device	*link;	/* underlay link this pval hiring */
	u64 __percpu		 seq;	/* sequence for TXed packets */

	/* on/off switches for functionalities */
	bool run;
	bool ipopt;
	bool txtstamp;
	bool rxtstamp;
	bool txcopy;
	bool rxcopy;

	/* @original_config: config before pval manipulates */
	struct hwtstamp_config original_config;

	/* misc device structures */
	int num_cpus;
	struct pval_mdev txmdevs[PVAL_MAX_CPUS];
	struct pval_mdev rxmdevs[PVAL_MAX_CPUS];
};
#define pdev_tx_ring(pdev) (&((pdev)->txmdevs[smp_processor_id()].ring))
#define pdev_rx_ring(pdev) (&((pdev)->rxmdevs[smp_processor_id()].ring))
#define pdev_tx_pmdev(pdev) (&((pdev)->txmdevs[smp_processor_id()]))
#define pdev_rx_pmdev(pdev) (&((pdev)->rxmdevs[smp_processor_id()]))

/* netns parameters */
static unsigned int pval_net_id;

struct pval_net {
	struct list_head	dev_list;	/* per netns pval dev list */
};



/* file operation to bring packets to user space */


/* ring operations */
static inline bool ring_emtpy(const struct pval_ring *r)
{
	return (r->head == r->tail);
}

static inline bool ring_full(const struct pval_ring *r)
{
	return (((r->head + 1) & r->mask) == r->tail);
}

static inline void ring_write_next(struct pval_ring *r)
{
	r->head = (r->head + 1) & r->mask;
}

static inline void ring_read_next(struct pval_ring *r)
{
	r->tail = (r->tail + 1) & r->mask;
}

static inline u32 ring_read_avail(const struct pval_ring *r)
{
	if (r->head > r->tail)
		return r->head - r->tail;
	if (r->tail > r->head)
		return r->mask - r->tail + r->head + 1;
	return 0;	// empty
}

static inline u32 ring_write_avail(const struct pval_ring *r)
{
	if (r->tail > r->head)
		return r->tail - r->head;
	if (r->head > r->tail)
		return r->mask - r->head + r->tail + 1;
	return 0;	// full
}

static inline void ring_zero(struct pval_ring *r)
{
	r->head = 0;
	r->tail = 0;
}

static inline ssize_t write_to_ring(struct pval_ring *r, struct sk_buff *skb)
{
	u32 pktlen = skb->mac_len + skb->len;
	u32 copylen = pktlen > PVAL_PKT_LEN ? PVAL_PKT_LEN : pktlen;
	struct pval_slot *s;

	if (ring_full(r))
		return 0;

	s = &r->slots[r->head];

	s->len = copylen;
	s->tstamp = skb_hwtstamps(skb)->hwtstamp;
	memcpy(s->pkt, skb_mac_header(skb), copylen);
	ring_write_next(r);

	return copylen;
}

static int pval_file_open(struct inode *inode, struct file *filp)
{
	int cpu, n;
	char devname[IFNAMSIZ], direct[16], buf[PVAL_NAME_MAX];
	struct net_device *dev;
	struct pval_dev *pdev;
	struct pval_mdev *pmdev;

	/* copy and change chardev to be capable of sscanf */
	strncpy(buf, filp->f_path.dentry->d_name.name, PVAL_NAME_MAX);
	for (n = 0; n < strlen(buf); n++) {
		if (buf[n] == '-')
			buf[n] = ' ';
	}
	if (sscanf(buf, "%s %s cpu %d", devname, direct, &cpu) < 1) {
		pr_err("%s: failed to parse char dev %s\n",
		       __func__, filp->f_path.dentry->d_name.name);
		return -EINVAL;
	}

	dev = __dev_get_by_name(&init_net, devname); // Do not dev_hold()
	if (!dev) {
		pr_err("netdevice %s not found\n", devname);
		return -ENODEV;
	}

	if (dev->netdev_ops != &pdev_netdev_ops) {
		pr_err("%s is not pval interface\n", devname);
		return -EINVAL;
	}

	pdev = netdev_priv(dev);

	if (cpu > pdev->num_cpus) {
		pr_err("invalid cpu number %d of %s\n", cpu,
			filp->f_path.dentry->d_name.name);
		return -EINVAL;
	}

	if (strncmp(direct, "tx", 2) == 0)
		pmdev = &pdev->txmdevs[cpu];
	else if (strncmp(direct, "rx", 2) == 0)
		pmdev = &pdev->rxmdevs[cpu];
	else {
		pr_err("invalid direction %s of %s\n", direct,
		       filp->f_path.dentry->d_name.name);
		return -EINVAL;
	}

	if (pmdev->opened) {
		pr_err("this miscdevice %s is already opened\n",
		       filp->f_path.dentry->d_name.name);
		return -EBUSY;
	}

	pmdev->opened = true;
	ring_zero(&pmdev->ring);	// flush the ring
	filp->private_data = pmdev;

	return 0;
}

static int
pval_file_release(struct inode *inode, struct file *filp)
{
	struct pval_mdev *pmdev = (struct pval_mdev *)filp->private_data;

	ring_zero(&pmdev->ring);	// flush the ring
	pmdev->opened = false;
	filp->private_data = NULL;
	return 0;
}

static ssize_t
pval_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t ret = 0;
	size_t count = iter->nr_segs;
	u32 avail, n, copylen, copynum;
	struct file *filp = iocb->ki_filp;
	struct pval_mdev *pmdev = (struct pval_mdev *)filp->private_data;
	struct pval_ring *r = &pmdev->ring;
	struct pval_slot *s;

	if (unlikely(iter->type != ITER_IOVEC)) {
		pr_err("unsupported iter type %d\n", iter->type);
		return -EOPNOTSUPP;
	}

	if (ring_emtpy(r))
		goto out;

	avail = ring_read_avail(r);
	copynum = avail > count ? count : avail;

	for (n = 0; n < copynum ; n++) {
		s = &r->slots[r->tail];
		copylen = sizeof(struct pval_slot) > iter->iov[n].iov_len ?
			iter->iov[n].iov_len : sizeof(struct pval_slot);
		copy_to_user(iter->iov[n].iov_base, s, copylen);
		ring_read_next(r);
		ret++;
	}

out:
	return ret;
}

static unsigned int pval_file_poll(struct file *file, poll_table *wait)
{
	struct pval_mdev *pmdev = (struct pval_mdev *)file->private_data;

	poll_wait(file, &pval_wait, wait);
	if (!ring_emtpy(&pmdev->ring))
		return POLLIN | POLLRDNORM;

	return 0;
}

static const struct file_operations pval_fops = {
	.owner		= THIS_MODULE,
	.open		= pval_file_open,
	.release	= pval_file_release,
	.read_iter	= pval_file_read_iter,
	.poll		= pval_file_poll,
};


static int pval_init_ring(struct pval_ring *ring, int cpu)
{
	ring->cpu = cpu;
	ring->head = 0;
	ring->tail = 0;
	ring->mask = PVAL_SLOT_NUM - 1;
	ring->slots = kmalloc(sizeof(struct pval_slot) * PVAL_SLOT_NUM,
			      GFP_KERNEL);
	if (!ring->slots) {
		pr_err("failed to kmalloc pval_slots for ring %d\n", cpu);
		return -ENOMEM;
	}

	return 0;
}

static void pval_destroy_ring(struct pval_ring *ring)
{
	kfree(ring->slots);
}

static int pval_init_miscdevice(struct pval_mdev *pmdev, char *name, int cpu)
{
	int rc;

	strncpy(pmdev->name, name, PVAL_NAME_MAX);
	pmdev->cpu		= cpu;
	pmdev->opened		= false;
	pmdev->mdev.name	= pmdev->name;
	pmdev->mdev.minor	= MISC_DYNAMIC_MINOR;
	pmdev->mdev.fops	= &pval_fops;

	rc = pval_init_ring(&pmdev->ring, cpu);
	if (rc < 0) {
		pr_err("failed to init ring on cpu %d for %s\n", cpu, name);
		goto err_out;
	}

	rc = misc_register(&pmdev->mdev);
	if (rc < 0) {
		pr_err("failed to register misc device %s\n", name);
		goto err_misc_dev;
	}

	pr_info("%s is registered \n", pmdev->name);

	return 0;

err_misc_dev:
	pval_destroy_ring(&pmdev->ring);

err_out:
	return rc;
}


static void pval_destroy_miscdevice(struct pval_mdev *pmdev)
{
	misc_deregister(&pmdev->mdev);
	pval_destroy_ring(&pmdev->ring);
}



/* misc from netmap pkt-gen */
static uint16_t
checksum(const void * data, uint16_t len, uint32_t sum)
{
        const uint8_t *addr = data;
        uint32_t i;

        /* Checksum all the pairs of bytes first... */
        for (i = 0; i < (len & ~1U); i += 2) {
                sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
                if (sum > 0xFFFF)
                        sum -= 0xFFFF;
        }
        /*
         * If there's a single byte left over, checksum it, too.
         * Network byte order is big-endian, so the remaining byte is
         * the high byte.
         */

        if (i < len) {
                sum += addr[i] << 8;
                if (sum > 0xFFFF)
                        sum -= 0xFFFF;
        }

        return sum;
}

static u_int16_t
wrapsum(u_int32_t sum)
{
        sum = ~sum & 0xFFFF;
        return (htons(sum));
}



static int netdev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	int rc;
	mm_segment_t fs;

	if (!dev->netdev_ops->ndo_do_ioctl) {
		pr_err("%s does not have ndo_do_ioctl\n", dev->name);
		return -ENOTSUPP;
	}

	fs = get_fs();
	set_fs(get_ds());

	/* XXX: ioctl is called in user context, so that ioctl
	 * implementations uses copy_from/to_user for ifr->data. So,
	 * switch FS segment register.
	 */

	rc = dev->netdev_ops->ndo_do_ioctl(dev, ifr, cmd);

	set_fs(fs);

	return rc;
}


static int pval_save_tstamp_config(struct pval_dev *pdev)
{
	int rc;
	struct hwtstamp_config config;
	struct ifreq ifr;

	/* save the current hwtstamp config to pdev->original */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, pdev->link->name, IFNAMSIZ);
	ifr.ifr_data = &config;

	rc = netdev_ioctl(pdev->link, &ifr, SIOCGHWTSTAMP);

	if (rc) {
		pr_err("%s: %s failed to get hwtstamp config: %d\n",
		       __func__, pdev->link->name, rc);
		return rc;
	}

	pdev->original_config = config;

	return 0;
}

static int pval_restore_tstamp_config(struct pval_dev *pdev)
{
	int rc = 0;
	struct ifreq ifr;

	/* set the hwtstamp config from pdev->original_config */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, pdev->link->name, IFNAMSIZ);
	ifr.ifr_data = &pdev->original_config;

	rc = netdev_ioctl(pdev->link, &ifr, SIOCSHWTSTAMP);
	if (rc)
		pr_err("%s: %s failed to set hwtstamp config: %d\n",
		       __func__, pdev->link->name, rc);

	return rc;
}


static int pval_set_tstamp_config(struct pval_dev *pdev)
{
	int rc = 0;
	struct hwtstamp_config config;
	struct ifreq ifr;

	/* set the hwtstamp config by pdev parameters */
	config.flags = 0;
	config.tx_type = 0;
	config.rx_filter = 0;

	if (pdev->txtstamp)
		config.tx_type = HWTSTAMP_TX_ON;
	if (pdev->rxtstamp)
		config.rx_filter = HWTSTAMP_FILTER_ALL;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, pdev->link->name, IFNAMSIZ);
	ifr.ifr_data = &config;

	rc = netdev_ioctl(pdev->link, &ifr, SIOCSHWTSTAMP);
	if (rc)
		pr_err("%s: %s failed to set hwtstamp config: %d\n",
		       __func__, pdev->link->name, rc);

	return rc;
}


/* Rx handler */
rx_handler_result_t pdev_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct pval_dev *pdev = rcu_dereference(skb->dev->rx_handler_data);
	
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return RX_HANDLER_CONSUMED;

	*pskb = skb;
	skb->dev = pdev->dev;
	skb->pkt_type = PACKET_HOST;

	if (pdev->rxcopy && pdev_rx_pmdev(pdev)->opened)
		write_to_ring(pdev_rx_ring(pdev), skb);

	return RX_HANDLER_ANOTHER;
}


static int pval_init(struct net_device *dev)
{
	/* setup stats when this device is created */
	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
        if (!dev->tstats)
                return -ENOMEM;

        return 0;
}

static void pval_uninit(struct net_device *dev)
{
	free_percpu(dev->tstats);
}
	

static int pval_open(struct net_device *dev)
{
	int rc = 0;
	struct pval_dev *pdev = netdev_priv(dev);

	if (netdev_is_rx_handler_busy(pdev->link)) {
		pr_info("Rx Handler of %s is busy. Cannot open %s\n",
		       pdev->link->name, pdev->dev->name);
		rc = -EBUSY;
		goto out;
	} else {
		pr_info("Register RX handler for %s\n", pdev->link->name);
		netdev_rx_handler_register(pdev->link,  pdev_handle_frame,
					   pdev);
	}

	/* XXX: save and configure hwtstamp 
	 * This should handle erros (mainy, -ENOTSUPP).
	 * However, it resitrct development environment because
	 * emulated e1000 devices do not suport hwtstamping.
	 * Therefore, we ignore tstamp config errors.
	 */
	pval_set_tstamp_config(pdev);

	/* set link device all multi and promisc */
	rc = dev_set_promiscuity(pdev->link, 1);
	if (rc < 0)
		goto err_out;

	return rc;

err_out:
	pval_restore_tstamp_config(pdev);
	netdev_rx_handler_unregister(pdev->link);
out:
	return rc;
}

static int pval_stop(struct net_device *dev)
{
	struct pval_dev *pdev = netdev_priv(dev);

	netdev_rx_handler_unregister(pdev->link);
	dev_set_promiscuity(pdev->link, -1);

	return 0;
}

static netdev_tx_t pval_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct pval_dev *pdev = netdev_priv(dev);
	struct ethhdr *eth_old, *eth_new;
	struct iphdr *iph_old, *iph_new;
	struct ipopt_pval *ipp;

	if (!pdev->ipopt)
		goto xmit;

	/* Advance eth+iph sizeof(struct ipopt_pval) bytes */
	eth_old = (struct ethhdr *)skb_mac_header(skb);
	iph_old = (struct iphdr *)skb_network_header(skb);

	if (ntohs(eth_old->h_proto) != ETH_P_IP)
		goto xmit;

	if (skb_cow_head(skb, sizeof(struct ipopt_pval))) {
		pr_err("skb_cow_head failed\n");
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	__skb_push(skb, sizeof(struct ipopt_pval));
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));

	eth_new = (struct ethhdr *)skb_mac_header(skb);
	iph_new = (struct iphdr *)skb_network_header(skb);
	
	memmove(eth_new, eth_old, sizeof(struct ethhdr));
	memmove(iph_new, iph_old, sizeof(struct iphdr));

	/* Insert Pval IP Option and update iphlen and checksum */
	ipp = (struct ipopt_pval *)(iph_new + 1);
	ipp->type	= IPOPT_PVAL;
	ipp->length	= sizeof(struct ipopt_pval);
	ipp->reserved	= 0;
	ipp->cpu	= smp_processor_id();
	ipp->seq	= pdev->seq++;

	iph_new->ihl	+= sizeof(struct ipopt_pval) >> 2;
	iph_new->tot_len	= htons(ntohs(iph_new->tot_len) +
					sizeof(struct ipopt_pval));
	iph_new->check	= 0;
	iph_new->check	= wrapsum(checksum(iph_new,
					   sizeof(struct iphdr) +
					   sizeof(struct ipopt_pval),
					   0));

xmit:
	/* XXX: Gather hwtstamp here */
	if (pdev->txcopy && pdev_tx_pmdev(pdev)->opened)
		write_to_ring(pdev_tx_ring(pdev), skb);

	/* Xmit this packet through under device */
	skb->dev = pdev->link;
	return dev_queue_xmit(skb);
}


static const struct net_device_ops pdev_netdev_ops = {
	.ndo_init		= pval_init,
	.ndo_uninit		= pval_uninit,
	.ndo_open	       	= pval_open,
	.ndo_stop		= pval_stop,
	.ndo_start_xmit		= pval_xmit,
	.ndo_get_stats64	= ip_tunnel_get_stats64,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};


static struct device_type pval_type = {
	.name = "pval",
};

/* RTNL link operations */

static const struct nla_policy pval_policy[IFLA_PVAL_MAX + 1] = {
	[IFLA_PVAL_LINK]	= { .type = NLA_U32 },
};

static void pval_setup(struct net_device *dev) {
	/* initialize the device structure. */

	struct pval_dev *pdev = netdev_priv(dev);

	eth_hw_addr_random(dev);
	ether_setup(dev);

	dev->needs_free_netdev = true;
	dev->netdev_ops = &pdev_netdev_ops;
	SET_NETDEV_DEVTYPE(dev,  &pval_type);
	
        dev->features   |= NETIF_F_LLTX;
        //dev->features   |= NETIF_F_SG | NETIF_F_HW_CSUM;
        //dev->features   |= NETIF_F_RXCSUM;
        //dev->features   |= NETIF_F_GSO_SOFTWARE;

	// dev->vlan_features = dev->features;
        // dev->hw_features |= NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_RXCSUM;
        // dev->hw_features |= NETIF_F_GSO_SOFTWARE;
        // netif_keep_dst(dev);
        dev->priv_flags |= IFF_NO_QUEUE;
	dev->priv_flags &= ~IFF_TX_SKB_SHARING;

	/* MTU range: 68 - 65535 */
        dev->min_mtu = ETH_MIN_MTU;
        dev->max_mtu = ETH_MAX_MTU;

	INIT_LIST_HEAD(&pdev->list);
}

static int pval_nl_config(struct pval_dev *pdev,
			  struct nlattr *tb[], struct nlattr *data[],
			  struct netlink_ext_ack *extack)
{
	/* XXX: 
	 * Changing lower link is not supported.
	 */

	/* parse and load configurations */
	if (data && data[IFLA_PVAL_IPOPT]) {
		if (nla_get_u8(data[IFLA_PVAL_IPOPT]))
			pdev->ipopt = true;
		else
			pdev->ipopt = false;
	}

	if (data && data[IFLA_PVAL_TXTSTAMP]) {
		if (nla_get_u8(data[IFLA_PVAL_TXTSTAMP]))
			pdev->txtstamp = true;
		else
			pdev->txtstamp = false;
	}

	if (data && data[IFLA_PVAL_RXTSTAMP]) {
		if (nla_get_u8(data[IFLA_PVAL_RXTSTAMP]))
			pdev->rxtstamp = true;
		else
			pdev->rxtstamp = false;
	}

	if (data && data[IFLA_PVAL_TXCOPY]) {
		if (nla_get_u8(data[IFLA_PVAL_TXCOPY]))
			pdev->txcopy = true;
		else
			pdev->txcopy = false;
	}

	if (data && data[IFLA_PVAL_RXCOPY]) {
		if (nla_get_u8(data[IFLA_PVAL_RXCOPY]))
			pdev->rxcopy = true;
		else
			pdev->rxcopy = false;
	}
	
	return 0;
}

static int pval_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	int err, n;
	char name[PVAL_NAME_MAX];
	u32 ifindex;
	unsigned short needed_headroom;
	struct net_device *link = NULL;
	struct pval_net *pnet = net_generic(src_net, pval_net_id);
	struct pval_dev *pdev = netdev_priv(dev);

	/* initialize pdev parameters */
	pdev->dev	= dev;
	pdev->run	= false;
	pdev->ipopt	= false;
	pdev->txtstamp	= false;
	pdev->rxtstamp	= false;
	pdev->txcopy	= false;
	pdev->rxcopy	= false;
	memset(&pdev->original_config, 0, sizeof(struct hwtstamp_config));

	/* check underlay link */
	if (data && data[IFLA_PVAL_LINK]) {
		ifindex = nla_get_u32(data[IFLA_PVAL_LINK]);
		link = dev_get_by_index(src_net, ifindex);
	}
	if (!link) {
		NL_SET_ERR_MSG(extack, "Invalid ifindex for underlay link");
		return -ENODEV;
	}
	pdev->link = link;

	/* parse and configure device */
	pval_nl_config(pdev, tb, data, extack);

	/* headroom allocate */
	needed_headroom = sizeof(struct ipopt_pval);
	needed_headroom += link->needed_headroom;
	dev->needed_headroom = needed_headroom;

	/* register ethernet device */
	err = register_netdevice(dev);
	if (err) {
		netdev_err(dev, "failed to register netdevice %s\n",
			   pdev->dev->name);
		return err;
	}

	err = netdev_upper_dev_link(link, dev, extack);
	if (err)
		goto unregister_netdev;

	/* register misc device */
	pdev->num_cpus = PVAL_MAX_CPUS > num_possible_cpus() ?
		num_possible_cpus() : PVAL_MAX_CPUS;

	for (n = 0; n < pdev->num_cpus; n++) {
		/* XXX: should handle errors (free succseed mdevs )*/
		snprintf(name, sizeof(name),
			 "pval/%s-tx-cpu-%d", pdev->dev->name, n);
		err = pval_init_miscdevice(&pdev->txmdevs[n], name, n);
		if (err < 0)
			goto unregister_netdev;

		snprintf(name, sizeof(name),
			 "pval/%s-rx-cpu-%d", pdev->dev->name, n);
		err = pval_init_miscdevice(&pdev->rxmdevs[n], name, n);
		if (err < 0)
			goto unregister_netdev;
	}

	/* save current hwtstamp config of lower link */
	pval_save_tstamp_config(pdev);

	/* finished */
	list_add_tail_rcu(&pdev->list, &pnet->dev_list);

	return 0;

unregister_netdev:
	unregister_netdevice(dev);
	return err;
}

static int pval_changelink(struct net_device *dev, struct nlattr *tb[],
			   struct nlattr *data[],
			   struct netlink_ext_ack *extack)
{
	struct pval_dev *pdev = netdev_priv(dev);
	
	if (data && data[IFLA_PVAL_LINK]) {
		NL_SET_ERR_MSG(extack, "changing link is not supported\n");
		return -ENOTSUPP;
	}

	pval_nl_config(pdev, tb, data, extack);;

	/* XXX: update tstamp config 
	 * should handle pval_*_tstamp_config errors here.
	 */
	pval_set_tstamp_config(pdev);

	return 0;
}

static void pval_dellink(struct net_device *dev, struct list_head *head)
{
	int n;
	struct pval_dev *pdev = netdev_priv(dev);

	pval_restore_tstamp_config(pdev);
	dev_put(pdev->link);
	list_del_rcu(&pdev->list);

	unregister_netdevice_queue(dev, head);
	netdev_upper_dev_unlink(pdev->link, dev);

	for (n = 0; n < pdev->num_cpus; n++) {
		pval_destroy_miscdevice(&pdev->txmdevs[n]);
		pval_destroy_miscdevice(&pdev->rxmdevs[n]);
	}
}


static size_t pval_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(u32));	/* IFLA_PVAL_LINK */

	return 0;
}

static int pval_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct pval_dev *pdev = netdev_priv(dev);

	if (nla_put_u32(skb, IFLA_PVAL_LINK, pdev->link->ifindex))
		return -EMSGSIZE;

	if (nla_put_u8(skb, IFLA_PVAL_IPOPT, pdev->ipopt ? 1 : 0))
		return -EMSGSIZE;

	if (nla_put_u8(skb, IFLA_PVAL_TXTSTAMP, pdev->txtstamp ? 1 : 0))
		return -EMSGSIZE;

	if (nla_put_u8(skb, IFLA_PVAL_RXTSTAMP, pdev->rxtstamp ? 1 : 0))
		return -EMSGSIZE;

	if (nla_put_u8(skb, IFLA_PVAL_TXCOPY, pdev->txcopy ? 1 : 0))
		return -EMSGSIZE;

	if (nla_put_u8(skb, IFLA_PVAL_RXCOPY, pdev->rxcopy ? 1 : 0))
		return -EMSGSIZE;

	return 0;
}


static struct net *pval_get_link_net(const struct net_device *dev)
{
	struct pval_dev *pdev = netdev_priv(dev);
	if (pdev->link) {
		return dev_net(pdev->link);
	}

	return NULL;
}


static struct rtnl_link_ops pval_link_ops __read_mostly = {
	.kind		= "pval",
	.maxtype	= IFLA_PVAL_MAX,
	.policy		= pval_policy,
	.priv_size	= sizeof(struct pval_dev),
	.setup		= pval_setup,
	.newlink	= pval_newlink,
	.changelink	= pval_changelink,
	.dellink	= pval_dellink,
	.get_size	= pval_get_size,
	.fill_info	= pval_fill_info,
	.get_link_net	= pval_get_link_net,
};


/* pernet oprations */

static __net_init int
pval_init_net(struct net *net)
{
	struct pval_net *pnet = net_generic(net, pval_net_id);
	INIT_LIST_HEAD(&pnet->dev_list);

	return 0;
}

static __net_exit void
pval_exit_net(struct net *net)
{
	struct pval_net *pnet = net_generic(net, pval_net_id);
	struct pval_dev *pdev, *next;
	LIST_HEAD(list);

	rtnl_lock();
	list_for_each_entry_safe(pdev, next, &pnet->dev_list, list) {
		/* XXX: release 'pdev->link */
		unregister_netdevice_queue(pdev->dev, &list);
	}
	rtnl_unlock();
}

static struct pernet_operations pval_net_ops = {
	.init	= pval_init_net,
	.exit	= pval_exit_net,
	.id	= &pval_net_id,
	.size	= sizeof(struct pval_net),
};



static int __init pval_init_module(void)
{
	int rc;

	rc = register_pernet_subsys(&pval_net_ops);
	if (rc)
		goto out1;

	rc = rtnl_link_register(&pval_link_ops);
	if (rc)
		goto out2;

	pr_info("Load Pval Moudle (v%s)\n", PVAL_VERSION);

	return 0;
out2:
	unregister_pernet_subsys(&pval_net_ops);
out1:
	return 0;
}
module_init(pval_init_module);


static void __exit pval_exit_module(void)
{
	rtnl_link_unregister(&pval_link_ops);
	unregister_pernet_subsys(&pval_net_ops);

	pr_info("Unload Pval Module (v%s)\n", PVAL_VERSION);
}
module_exit(pval_exit_module);


MODULE_LICENSE("GPL");
MODULE_VERSION(PVAL_VERSION);
MODULE_AUTHOR("upa@haeena.net");
MODULE_DESCRIPTION("Measuring Pcaket intervals Minutely");
MODULE_ALIAS_RTNL_LINK("pval");



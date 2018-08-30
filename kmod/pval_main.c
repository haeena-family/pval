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
#include <asm/string.h>

#include <pval.h>

#define PVAL_VERSION "0.0.1"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* Pval IP Option */
#define IPOPT_PVAL	222 /* reserved for Experimental use in RFC4727 */

struct ipopt_pval {
	u8	type;
	u8	length;
	u16	reserved;
	u64	seq;
} __attribute__ ((__packed__));



/* structure describing pval device */
struct pval_dev {
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;

	struct net_device	*link;	/* underlay link this pval hiring */
	u64 __percpu		 seq;	/* sequence for TXed packets */
};


/* netns parameters */
static unsigned int pval_net_id;

struct pval_net {
	struct list_head	dev_list;	/* per netns pval dev list */
};

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


/* Rx handler */
rx_handler_result_t pdev_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct pval_dev *pdev = rcu_dereference(skb->dev->rx_handler_data);
	
	/* XXX: put the packet and rx hwtstamp to the buffer exposed to user */
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return RX_HANDLER_CONSUMED;

	*pskb = skb;
	skb->dev = pdev->dev;
	skb->pkt_type = PACKET_HOST;

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
	} else {
		pr_info("Register RX handler for %s\n", pdev->link->name);
		netdev_rx_handler_register(pdev->link,  pdev_handle_frame,
					   pdev);
	}

	/* set link device all multi and promisc */
	rc = dev_set_promiscuity(pdev->link, 1);
	if (rc < 0)
		goto err_out;

	return rc;

err_out:
	netdev_rx_handler_unregister(pdev->link);
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
	ipp->seq	= pdev->seq++;

	iph_new->ihl	+= sizeof(struct ipopt_pval) >> 2;
	iph_new->tot_len	= htons(ntohs(iph_new->tot_len) +
					sizeof(struct ipopt_pval));
	iph_new->check	= 0;
	iph_new->check	= wrapsum(checksum(iph_new,
					   sizeof(struct iphdr) +
					   sizeof(struct ipopt_pval),
					   0));

	/* Xmit this packet through under device */
xmit:
	skb->dev = pdev->link;
	return dev_queue_xmit(skb);

	/* XXX: Gather hwtstamp here */
	/* XXX: Put the cloned packet to the buffer exposing user space */
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

static int pval_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	int err;
	u32 ifindex;
	unsigned short needed_headroom;
	struct net_device *link = NULL;
	struct pval_net *pnet = net_generic(src_net, pval_net_id);
	struct pval_dev *pdev = netdev_priv(dev);

	pdev->dev = dev;

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

	/* headroom allocate */
	needed_headroom = sizeof(struct ipopt_pval);
	needed_headroom += link->needed_headroom;
	dev->needed_headroom = needed_headroom;

	/* register the device */
	err = register_netdevice(dev);
	if (err) {
		netdev_err(dev, "failed to register netdevice %s\n",
			   pdev->dev->name);
		return err;
	}

	err = netdev_upper_dev_link(link, dev, extack);
	if (err)
		goto unregister_netdev;

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
	return -ENOTSUPP;
}

static void pval_dellink(struct net_device *dev, struct list_head *head)
{
	struct pval_dev *pdev = netdev_priv(dev);

	dev_put(pdev->link);
	list_del_rcu(&pdev->list);
	unregister_netdevice_queue(dev, head);
	netdev_upper_dev_unlink(pdev->link, dev);
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

	pr_info("Unload Pval Module (%s)\n", PVAL_VERSION);
}
module_exit(pval_exit_module);


MODULE_LICENSE("GPL");
MODULE_VERSION(PVAL_VERSION);
MODULE_AUTHOR("upa@haeena.net");
MODULE_DESCRIPTION("Measuring Pcaket intervals Minutely");
MODULE_ALIAS_RTNL_LINK("pval");



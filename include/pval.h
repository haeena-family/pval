#ifndef _PVAL_H_
#define _PVAL_H_
/* 
 * pval.h
 */

#ifndef __KERNEL__
#include <asm/types.h>
#endif

/* Pval IP Option */
#define IPOPT_PVAL      222 /* reserved for Experimental use in RFC4727 */

/* Pval IP Option */
struct ipopt_pval {
	__u8 	type;
	__u8	length;
	__u8	reserved;
	__u8	cpu;
	__u64	seq;
} __attribute__ ((__packed__));



/* Netlink parameters */
enum {
	IFLA_PVAL_UNSPEC,
	IFLA_PVAL_LINK,		/* ifindex of lower link */
	IFLA_PVAL_IPOPT,	/* ON/OFF: add Pval IP Option */
	IFLA_PVAL_TXTSTAMP,	/* ON/OFF: TX timestamping */
	IFLA_PVAL_RXTSTAMP,	/* ON/OFF: RX timestamping */
	IFLA_PVAL_TXCOPY,	/* ON/OFF: Copy TXed pkts to user */
	IFLA_PVAL_RXCOPY,	/* ON/OFF: Copy RXed pkts to user */
	IFLA_PVAL_TXBUSYDROP,	/* ON/OFF: Drop TXed pkts when tstamp busy */
	__IFLA_PVAL_MAX
};
#define IFLA_PVAL_MAX	(__IFLA_PVAL_MAX - 1)


/* attrs */
enum {
	PVAL_ATTR_UNSPEC,
	PVAL_ATTR_LINK,		/* 32bit underlay link ifindex */
	__PVAL_ATTR_MAX,
};
#define PVAL_ATTR_MAX	(__PVAL_ATTR_MAX)


/* Pval ring buffer structures */

#define PVAL_PKT_LEN	256

/* pval_slot is stored in each iovec by readv() syscall */
struct pval_slot {
	__u32	len;
	__u32	pktlen;
	__u64	tstamp;
	char	pkt[PVAL_PKT_LEN];
} __attribute__((__packed__));




#endif /* _PVAL_H_ */

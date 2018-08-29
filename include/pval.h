#ifndef _PVAL_H_
#define _PVAL_H_

/* 
 * pval.h
 * Pval Netlink Parameters
 */


enum {
	IFLA_PVAL_UNSPEC,
	IFLA_PVAL_LINK,
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

#endif /* _PVAL_H_ */

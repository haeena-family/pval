/*
 * iplink_pval.c	Pval link
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>

#include "rt_names.h"
#include "utils.h"
#include "ip_common.h"

#include "../../include/pval.h"

#define PVAL_ATTRSET(attrs, type) (((attrs) & (1L << (type))) != 0)

static void check_duparg(__u64 *attrs, int type, const char *key,
                         const char *argv)
{
	if (!PVAL_ATTRSET(*attrs, type)) {
		*attrs |= (1L << type);
		return;
	}
	duparg2(key, argv);
}



static void print_explain(FILE *f)
{
	fprintf(f,
		"Usage: ... pval link PHYS_DEV\n"
		"                 [ ipopt { on | off } ]\n"
		"                 [ txtstamp { on | off } ]\n"
		"                 [ rxtstamp { on | off } ]\n"
		"                 [ txcopy { on | off } ]\n"
		"                 [ rxcopy { on | off } ]\n"
		);
}

static void explain(void)
{
	print_explain(stderr);
}

static int pval_parse_opt(struct link_util *lu, int argc, char **argv,
			  struct nlmsghdr *n)
{
	__u64 attrs = 0;
	__u32 link = 0;

	while (argc > 0) {
		if (!matches(*argv, "link")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_LINK, "link", *argv);
			link = if_nametoindex(*argv);
			if (!link) {
				invarg("invalid device", *argv);
			}
			addattr32(n, 1024, IFLA_PVAL_LINK, link);
		} else if (!matches(*argv, "ipopt")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_IPOPT, "ipopt", *argv);
			if (!matches(*argv, "on"))
				addattr8(n, 1024, IFLA_PVAL_IPOPT, 1);
			else if (!matches(*argv, "off"))
				addattr8(n, 1024, IFLA_PVAL_IPOPT, 0);
		} else if (!matches(*argv, "txtstamp")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_TXTSTAMP, "txtstamp",
				     *argv);
			if (!matches(*argv, "on"))
				addattr8(n, 1024, IFLA_PVAL_TXTSTAMP, 1);
			else if (!matches(*argv, "off"))
				addattr8(n, 1024, IFLA_PVAL_TXTSTAMP, 0);
		} else if (!matches(*argv, "rxtstamp")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_RXTSTAMP, "rxtstamp",
				     *argv);
			if (!matches(*argv, "on"))
				addattr8(n, 1024, IFLA_PVAL_RXTSTAMP, 1);
			else if (!matches(*argv, "off"))
				addattr8(n, 1024, IFLA_PVAL_RXTSTAMP, 0);
		} else if (!matches(*argv, "txcopy")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_TXCOPY, "txcopy",
				     *argv);
			if (!matches(*argv, "on"))
				addattr8(n, 1024, IFLA_PVAL_TXCOPY, 1);
			else if (!matches(*argv, "off"))
				addattr8(n, 1024, IFLA_PVAL_TXCOPY, 0);
		} else if (!matches(*argv, "rxcopy")) {
			NEXT_ARG();
			check_duparg(&attrs, IFLA_PVAL_RXCOPY, "rxcopy",
				     *argv);
			if (!matches(*argv, "on"))
				addattr8(n, 1024, IFLA_PVAL_RXCOPY, 1);
			else if (!matches(*argv, "off"))
				addattr8(n, 1024, IFLA_PVAL_RXCOPY, 0);
		} else if (!matches(*argv, "help")) {
			explain();
			return -1;
		} else {
			fprintf(stderr, "pval: unknown command \"%s\"?\n",
				*argv);
			explain();
			return -1;
		}

		argc--, argv++;
	}

	return 0;
}

static void pval_print_opt(struct link_util *lu, FILE *f, struct rtattr *tb[])
{
	__u32 link;
	char linkname[IF_NAMESIZE];
	char *r, *on = "on", *off = "off";


	if (!tb)
		return;

	if (tb[IFLA_PVAL_LINK]) {
		link = rta_getattr_u32(tb[IFLA_PVAL_LINK]);
		if (if_indextoname(link, linkname)) {
			print_string(PRINT_ANY, "link", "link %s ",
				     ll_index_to_name(link));
		}
	}

	if (tb[IFLA_PVAL_IPOPT]) {
		r = rta_getattr_u8(tb[IFLA_PVAL_IPOPT]) ? on : off;
		print_string(PRINT_ANY, "ipopt", "ipopt %s ", r);
	}

	if (tb[IFLA_PVAL_TXTSTAMP]) {
		r = rta_getattr_u8(tb[IFLA_PVAL_TXTSTAMP]) ? on : off;
		print_string(PRINT_ANY, "txtstamp", "txtstamp %s ", r);
	}

	if (tb[IFLA_PVAL_RXTSTAMP]) {
		r = rta_getattr_u8(tb[IFLA_PVAL_RXTSTAMP]) ? on : off;
		print_string(PRINT_ANY, "rxtstamp", "rxtstamp %s ", r);
	}

	if (tb[IFLA_PVAL_TXCOPY]) {
		r = rta_getattr_u8(tb[IFLA_PVAL_TXCOPY]) ? on : off;
		print_string(PRINT_ANY, "txcopy", "txcopy %s ", r);
	}

	if (tb[IFLA_PVAL_RXCOPY]) {
		r = rta_getattr_u8(tb[IFLA_PVAL_RXCOPY]) ? on : off;
		print_string(PRINT_ANY, "rxcopy", "rxcopy %s ", r);
	}
}

static void pval_print_help(struct link_util *lu, int argc, char **argv,
			    FILE *f)
{
	print_explain(f);
}

struct link_util pval_link_util = {
	.id		= "pval",
	.maxattr	= IFLA_PVAL_MAX,
	.parse_opt	= pval_parse_opt,
	.print_opt	= pval_print_opt,
	.print_help	= pval_print_help,
};

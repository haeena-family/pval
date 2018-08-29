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

static void print_explain(FILE *f)
{
	fprintf(f,
		"Usage: ... pval link PHYS_DEV\n"
		);
}

static void explain(void)
{
	print_explain(stderr);
}

static int pval_parse_opt(struct link_util *lu, int argc, char **argv,
			  struct nlmsghdr *n)
{
	__u32 link = 0;

	while (argc > 0) {
		if (!matches(*argv, "link")) {
			NEXT_ARG();
			link = if_nametoindex(*argv);
			if (!link) {
				invarg("invalid device", *argv);
			}
			addattr32(n, 1024, IFLA_PVAL_LINK, link);
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

	if (!tb)
		return;

	if (tb[IFLA_PVAL_LINK]) {
		link = rta_getattr_u32(tb[IFLA_PVAL_LINK]);
		if (if_indextoname(link, linkname)) {
			print_string(PRINT_ANY, "link", "link %s ",
				     ll_index_to_name(link));
		}

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

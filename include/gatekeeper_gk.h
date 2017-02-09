/*
 * Gatekeeper - DoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GATEKEEPER_GK_H_
#define _GATEKEEPER_GK_H_

#include <rte_atomic.h>

#include "gatekeeper_net.h"
#include "gatekeeper_ipip.h"
#include "gatekeeper_ggu.h"
#include "gatekeeper_mailbox.h"
#include "gatekeeper_lpm.h"

/*
 * The LPM supports 8-bit next hop,
 * which at most has 256 different values.
 */
#define GK_MAX_NUM_FIB_ENTRIES (256)

/*
 * A flow entry can be in one of three states:
 * request, granted, or declined.
 */
enum gk_flow_state { GK_REQUEST, GK_GRANTED, GK_DECLINED };

enum gk_fib_action {

	/* Forward the packet to the corresponding Grantor. */
	GK_FWD_GRANTOR,

	/* Forward the packet to the corresponding gateway. */
	GK_FWD_GATEWAY,

	/*
	 * The destination address is a neighbor.
	 * Forward the packet to the destination directly.
	 */
	GK_FWD_NEIGHBOR,

	/* Forward the packet to the back interface. */
	GK_FWD_BACK_NET,

	/* Drop the packet. */
	GK_DROP,

	/* Invalid forward action. */
	GK_FIB_MAX,
};

/* The Ethernet header cache. */
struct ether_cache {

	/* Indicate whether the MAC address is stale or not. */
	bool stale;

	/* The whole Ethernet header. */
	struct ether_hdr eth_hdr;
};

/* The nexthop information. */
struct gk_nexthop {

	/* The IP address of the nexthop. */
	struct ipaddr ip_addr;

	/* The cached Ethernet header. */
	struct ether_cache eth_cache;
};

/* The gk forward information base (fib). */
struct gk_fib {

	/* The fib action. */
	enum gk_fib_action   action;

	/*
	 * The count of how many times the LPM tables refer to it,
	 * so a fib entry can go away only when no LPM entry referring to it.
	 */
	uint32_t             ref_cnt;

	union {
		/*
	 	 * The nexthop information when the action is GK_FWD_GATEWAY.
	 	 */
		struct gk_nexthop nexthop;

		struct {
			/*
		 	 * When the action is GK_FWD_GRANTOR, we need
			 * the next fib entry for either the gateway or
			 * the grantor server itself as a neighbor.
			 */
			struct gk_fib *next_fib;

			/*
			 * Indicate whether the fib entry is used for
			 * Grantor IP prefix.
			 */
			bool is_grantor_prefix_fib;

			/*
		 	 * When the action is GK_FWD_GRANTOR, we need
			 * the IP flow information.
		 	 */
			struct ip_flow flow;

			/*
			 * Cache the whole Ethernet header when the @next_fib
			 * action is GK_FWD_NEIGHBOR.
			 */
			struct ether_cache eth_cache;
		} grantor;
	} u;
};

/* Structure for the GK global LPM table. */
struct gk_lpm {
	/* The IPv4 LPM table shared by the GK instances on the same socket. */
	struct rte_lpm    *lpm;

	/*
	 * The fib table for IPv4 LPM table that
	 * decides the actions on packets.
	 */
	struct gk_fib     fib_tbl[GK_MAX_NUM_FIB_ENTRIES];

	/* The IPv6 LPM table shared by the GK instances on the same socket. */
	struct rte_lpm6   *lpm6;

	/*
	 * The fib table for IPv6 LPM table that
	 * decides the actions on packets.
	 */
	struct gk_fib     fib_tbl6[GK_MAX_NUM_FIB_ENTRIES];
};

/* Structures for each GK instance. */
struct gk_instance {
	struct rte_hash   *ip_flow_hash_table;
	struct flow_entry *ip_flow_entry_table;
	/* RX queue on the front interface. */
	uint16_t          rx_queue_front;
	/* TX queue on the back interface. */
	uint16_t          tx_queue_back;
	struct mailbox    mb;
};

/* Configuration for the GK functional block. */
struct gk_config {
	/* Specify the size of the flow hash table. */
	unsigned int	   flow_ht_size;

	/*
	 * DPDK LPM library implements the DIR-24-8 algorithm
	 * using two types of tables:
	 * (1) tbl24 is a table with 2^24 entries.
	 * (2) tbl8 is a table with 2^8 entries.
	 *
	 * To configure an LPM component instance, one needs to specify:
	 * @max_rules: the maximum number of rules to support.
	 * @number_tbl8s: the number of tbl8 tables.
	 *
	 * Here, it supports both IPv4 and IPv6 configuration.
	 */
	unsigned int       max_num_ipv4_rules;
	unsigned int       num_ipv4_tbl8s;
	unsigned int       max_num_ipv6_rules;
	unsigned int       num_ipv6_tbl8s;

	/*
	 * The fields below are for internal use.
	 * Configuration files should not refer to them.
	 */
	rte_atomic32_t	   ref_cnt;

	/* The lcore ids at which each instance runs. */
	unsigned int       *lcores;

	/* The number of lcore ids in @lcores. */
	int                num_lcores;

	struct gk_instance *instances;
	struct net_config  *net;
	/*
	 * The LPM table used by the GK instances.
	 * We assume that all the GK instances are
	 * on the same numa node, so that only one global
	 * LPM table is maintained.
	 */
	struct gk_lpm      lpm_tbl;
	struct gatekeeper_rss_config rss_conf;
};

/* Structure for Lua set up the LPM IP range fib. */
struct lua_gk_fib {

	/* The IP prefix, to which the fib entry applied. */
	const char         *ip_prefix;

	/* The action of the fib entry. */
	enum gk_fib_action action;

	/* The Grantor server IP address. */
	const char         *grantor;

	/* The gateway IP address. */
	const char         *gateway;
};

/* Define the possible command operations for GK block. */
enum gk_cmd_op { GGU_POLICY_ADD, };

/*
 * XXX Structure for each command. Add new fields to support more commands.
 *
 * Notice that, the writers of a GK mailbox: the GK-GT unit and Dynamic config.
 */
struct gk_cmd_entry {
	enum gk_cmd_op  op;

	union {
		struct ggu_policy ggu;
	} u;
};

struct gk_config *alloc_gk_conf(void);
int gk_conf_put(struct gk_config *gk_conf);
int lua_init_gk_lpm(
	struct gk_config *gk_conf, struct net_config *net_conf,
	struct lua_gk_fib *fib_entries, int num_fib_entries);
int run_gk(struct net_config *net_conf, struct gk_config *gk_conf);
struct mailbox *get_responsible_gk_mailbox(
	const struct ip_flow *flow, const struct gk_config *gk_conf);

static inline void
gk_conf_hold(struct gk_config *gk_conf)
{
	rte_atomic32_inc(&gk_conf->ref_cnt);
}

#endif /* _GATEKEEPER_GK_H_ */

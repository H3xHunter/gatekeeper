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

#ifndef _GATEKEEPER_GK_FIB_H_
#define _GATEKEEPER_GK_FIB_H_

#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_spinlock.h>

#include "gatekeeper_net.h"
#include "gatekeeper_lpm.h"

enum gk_fib_action {

	/* Forward the packet to the corresponding Grantor. */
	GK_FWD_GRANTOR,

	/*
	 * Forward the packet to the corresponding gateway
	 * in the front network.
	 */
	GK_FWD_GATEWAY_FRONT_NET,

	/*
	 * Forward the packet to the corresponding gateway
	 * in the back network.
	 */
	GK_FWD_GATEWAY_BACK_NET,

	/*
	 * The destination address is a neighbor in the front network.
	 * Forward the packet to the destination directly.
	 */
	GK_FWD_NEIGHBOR_FRONT_NET,

	/*
	 * The destination address is a neighbor in the back network.
	 * Forward the packet to the destination directly.
	 */
	GK_FWD_NEIGHBOR_BACK_NET,

	/* Drop the packet. */
	GK_DROP,

	/* Invalid forward action. */
	GK_FIB_MAX,
};

/* The Ethernet header cache. */
struct ether_cache {

	/* Indicate whether the MAC address is stale or not. */
	bool             stale;

	/* The IP address of the nexthop. */
	struct ipaddr    ip_addr;

	/* The whole Ethernet header. */
	struct ether_hdr eth_hdr;

	/*
	 * The count of how many times the LPM tables refer to it,
	 * so a neighbor entry can go away only when no one referring to it.
	 */
	uint32_t         ref_cnt;
};

struct neighbor_hash_table {

	struct rte_hash    *hash_table;

	/* The tables that store the Ethernet headers. */
	struct ether_cache *cache_tbl;
};

/* The gk forward information base (fib). */
struct gk_fib {

	/* The fib action. */
	enum gk_fib_action action;

	/*
	 * The callee that finished processing the notification
	 * needs to increment this counter, so that the block
	 * that is updating the FIB entry can finish its operation.
	 */
	rte_atomic16_t     num_updated_instances;

	union {
		/*
	 	 * The nexthop information when the action is
		 * GK_FWD_GATEWAY_*_NET.
	 	 */
		struct {
			/* The cached Ethernet header. */
			struct ether_cache *eth_cache;
		} gateway;

		struct {
			/*
		 	 * When the action is GK_FWD_GRANTOR, we need
			 * the Grantor IP address.
		 	 */
			struct ipaddr gt_addr;

			/* The cached Ethernet header. */
			struct ether_cache *eth_cache;
		} grantor;

		/*
		 * When the action is GK_FWD_NEIGHBOR_*_NET, it stores all
		 * the neighbors' Ethernet headers in a hash table.
		 * The entries can be accessed according to its IP address.
		 */
		struct neighbor_hash_table neigh;

		struct neighbor_hash_table neigh6;
	} u;
};

struct gk_fib_dump_entry {

	/* The prefix string. */
	char     prefix[INET6_ADDRSTRLEN + 5];

	/* The Grantor IP address. */
	char     grantor_ip[INET6_ADDRSTRLEN];

	bool     stale;

	/* The IP address of the nexthop. */
	char     nexthop_ip[INET6_ADDRSTRLEN];

	uint16_t ether_type;

	char     d_addr[ETHER_ADDR_FMT_SIZE];

	char     s_addr[ETHER_ADDR_FMT_SIZE];

	uint32_t ref_cnt;

	/* The fib action. */
	enum gk_fib_action action;
};

/* Structure for the GK global LPM table. */
struct gk_lpm {
	/* Use a spin lock to edit the FIB table. */
	rte_spinlock_t  lock;

	/* The IPv4 LPM table shared by the GK instances on the same socket. */
	struct rte_lpm  *lpm;

	/*
	 * The fib table for IPv4 LPM table that
	 * decides the actions on packets.
	 */
	struct gk_fib   *fib_tbl;

	/* The IPv6 LPM table shared by the GK instances on the same socket. */
	struct rte_lpm6 *lpm6;

	/*
	 * The fib table for IPv6 LPM table that
	 * decides the actions on packets.
	 */
	struct gk_fib   *fib_tbl6;
};

struct gk_config;

int setup_gk_lpm(struct gk_config *gk_conf, unsigned int socket_id);
void destroy_neigh_hash_table(struct neighbor_hash_table *neigh);

int add_fib_entry(const char *prefix, const char *gt_ip, const char *gw_ip,
	enum gk_fib_action action, struct gk_config *gk_conf);
int del_fib_entry(const char *ip_prefix, struct gk_config *gk_conf);
struct gk_fib_dump_entry *list_fib_entries(struct gk_config *gk_conf,
	uint32_t *num_entries);

/* TODO Customize the hash function for IPv4. */

static inline struct ether_cache *
lookup_ether_cache(struct neighbor_hash_table *neigh_tbl, void *key)
{
	int ret = rte_hash_lookup(neigh_tbl->hash_table, key);
	if (ret < 0)
		return NULL;

	return &neigh_tbl->cache_tbl[ret];
}

#endif /* _GATEKEEPER_GK_FIB_H_ */

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

#ifndef _GATEKEEPER_NET_H_
#define _GATEKEEPER_NET_H_

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

#include <rte_acl.h>
#include <rte_eth_bond.h>
#include <rte_ethdev.h>
#include <rte_timer.h>

#include "gatekeeper_flow.h"

/* To mark whether Gatekeeper server configures IPv4 or IPv6. */
#define GK_CONFIGURED_IPV4 (1)
#define GK_CONFIGURED_IPV6 (2)

#define IPv6_DEFAULT_VTC_FLOW   (0x60000000)
#define IPv6_DEFAULT_HOP_LIMITS (0xFF)

struct ipaddr {
	/* The network layer protocol of the nexthop. */
	uint16_t proto;

	/* The IP address of the nexthop. */
	union {
		struct in_addr  v4;
		struct in6_addr v6;
	} ip;
};

/* Size of the secret key of the RSS hash. */
#define GATEKEEPER_RSS_KEY_LEN (40)

/*
 * The maximum number of "rte_eth_rss_reta_entry64" structures can be used to
 * configure the Redirection Table of the Receive Side Scaling (RSS) feature.
 * Notice, each "rte_eth_rss_reta_entry64" structure can configure 64 entries 
 * of the table. To configure more than 64 entries supported by hardware,
 * an array of this structure is needed.
 */
#define GATEKEEPER_RETA_MAX_SIZE (ETH_RSS_RETA_SIZE_512 / RTE_RETA_GROUP_SIZE)

struct gatekeeper_rss_config {
	uint16_t reta_size;
	struct rte_eth_rss_reta_entry64 reta_conf[GATEKEEPER_RETA_MAX_SIZE];
};

/* Maximum number of ACL classification types. */
#define GATEKEEPER_IPV6_ACL_MAX (8)

/*
 * Format of function called when a rule matches in the IPv6 ACL.
 * Need forward declaration because acl_cb_func and struct gatekeeper_if
 * are circularly defined.
 */
struct gatekeeper_if *iface;
typedef int (*acl_cb_func)(struct rte_mbuf **pkts, unsigned int num_pkts,
	struct gatekeeper_if *iface);
/* Format of function called when no rule matches in the IPv6 ACL. */
typedef int (*ext_cb_func)(struct rte_mbuf *pkt, struct gatekeeper_if *iface);

/*
 * A Gatekeeper interface is specified by a set of PCI addresses
 * that map to DPDK port numbers. If multiple ports are specified,
 * then the ports are bonded.
 */
struct gatekeeper_if {
	/* The ports (in PCI address format) that compose this interface. */
	char            **pci_addrs;

	/* The number of ports that in this interface (length of @pci_addrs). */
	uint8_t         num_ports;

	/* Name of the interface. Needed for setting/getting bonded port. */
	char            *name;

	/* Number of RX and TX queues for this interface. */
	uint16_t        num_rx_queues;
	uint16_t        num_tx_queues;

	/* Timeouts for cache entries (in seconds) for Link Layer Support. */
	uint32_t	arp_cache_timeout_sec;
	uint32_t	nd_cache_timeout_sec;

	/* The type of bonding used for this interface, if needed. */
	uint32_t        bonding_mode;

	/*
	 * The fields below are for internal use.
	 * Configuration files should not refer to them.
	 */

	/* Ethernet address of this interface. */
	struct ether_addr eth_addr;

	/* DPDK port IDs corresponding to each address in @pci_addrs. */
	uint8_t         *ports;

	/*
	 * The DPDK port ID for this interface.
	 *
	 * If @ports only has one element, then @id is that port.
	 * If @ports has multiple elements, then @id is the DPDK
	 * *bonded* port ID representing all of those ports.
	 */
	uint8_t         id;

	/* The RX and TX queue assignments on this interface for each lcore. */
	int16_t         rx_queues[RTE_MAX_LCORE];
	int16_t         tx_queues[RTE_MAX_LCORE];

	/*
	 * The next RX and TX queues to be assigned on this interface.
	 * We need atomic here in case multiple blocks are trying to
	 * configure their queues on the same interface at the same time.
	 */
	rte_atomic16_t  rx_queue_id;
	rte_atomic16_t  tx_queue_id;

	/*
	 * Specify the IPv4 and IPv6 addresses of this interface.
	 * Notice that, while one address must always be there,
	 * there may not be the second address.
	 */
	uint8_t         configured_proto;

	/* IPv4 address and network mask. */
	struct in_addr  ip4_addr;
	struct in_addr  ip4_mask;
	uint8_t         ip4_addr_plen;

	/*
	 * Global IPv6 address and network mask.
	 *
	 * This is the address/mask given by configuration
	 * and used for global routing.
	 */
	struct in6_addr ip6_addr;
	struct in6_addr ip6_mask;
	uint8_t         ip6_addr_plen;

	/*
	 * Addresses related to Neighbor Discovery.
	 */

	/*
	 * Link-local IPv6 address and network mask.
	 *
	 * ND messages can be sent from, and to, link-local IPv6
	 * addresses that are only routable inside the local
	 * network. We are also responsible for responding to
	 * resolution requests for the link-local address. It is
	 * automatically generated.
	 */
	struct in6_addr ll_ip6_addr;
	struct in6_addr ll_ip6_mask;

	/*
	 * IPv6 solicited-node multicast addresses.
	 *
	 * If a resolution is unknown, an ND Solicitation is sent
	 * to a solicited-node multicast address to reduce the
	 * number of hosts in the broadcast domain that receive
	 * the Solicitation. Two of these multicast addresses are
	 * automatically generated: one that covers the global IPv6
	 * address and one that covers the IPv6 link-local address.
	 */
	struct in6_addr ip6_mc_addr;
	struct in6_addr ll_ip6_mc_addr;

	/*
	 * IPv6 multicast Ethernet addresses.
	 *
	 * For packets that use a solicited-node multicast address
	 * for the IPv6 destination field, the Ethernet destination
	 * field should also use a special IPv6 multicast address.
	 * Two such addresses are automatically generated: they cover
	 * the global and link-local solicited-node multicast addresses.
	 */
	struct ether_addr eth_mc_addr;
	struct ether_addr ll_eth_mc_addr;

	/* Timer to transmit from LLS block to fulfill LACP TX requirement. */
	struct rte_timer  lacp_timer;

	/* Per-socket ACLs used for classifying IPv6 packets. */
	struct rte_acl_ctx *ipv6_acls[RTE_MAX_NUMA_NODES];

	/*
	 * Callback functions for each ACL rule type.
	 *
	 * On error, these functions should return a negative value
	 * and free all packets that have not already been handled.
	 */
	acl_cb_func        acl_funcs[GATEKEEPER_IPV6_ACL_MAX];

	/*
	 * Callback functions for each ACL rule type with
	 * IPv6 extension headers.
	 *
	 * Returning values: 0 means a match and a negative value
	 * means an error or that there was no match.
	 */
	ext_cb_func        ext_funcs[GATEKEEPER_IPV6_ACL_MAX];

	/* Number of ACL types installed in @acl_funcs. */
	unsigned int       acl_func_count;
};

/*
 * The atomic counters for @rx_queue_id and @tx_queue_id are
 * signed, so we get about 2^15 possible queues available for use,
 * which is much more than is needed.
 *
 * Use this constant as an out-of-band value to represent that
 * a queue has not been allocated; if one of the atomic counters
 * reaches this value, we have exceeded the number of possible
 * queues.
 */
#define GATEKEEPER_QUEUE_UNALLOCATED	(INT16_MIN)

enum queue_type {
	QUEUE_TYPE_RX,
	QUEUE_TYPE_TX,
	QUEUE_TYPE_MAX,
};

int get_queue_id(struct gatekeeper_if *iface, enum queue_type ty,
	unsigned int lcore);

/* Configuration for the Network. */
struct net_config {
	/*
	 * Set to zero (false) when a back interface is
	 * not needed, such as when running gatekeeper
	 * for Grantor.
	 */
	int                  back_iface_enabled;

	/* The parameter that alters the behavior of the call getrandom(). */
	unsigned int         random_flags;

	/*
	 * The fields below are for internal use.
	 * Configuration files should not refer to them.
	 */
	struct gatekeeper_if front;
	struct gatekeeper_if back;

	/* The total number of NUMA nodes in the host. */
	uint32_t             numa_nodes;

	/*
	 * There is a memory pool per NUMA node to be used for
	 * packet buffers in that node.
	 */
	struct rte_mempool   **gatekeeper_pktmbuf_pool;
};

extern uint8_t default_rss_key[GATEKEEPER_RSS_KEY_LEN];
extern uint8_t rss_key_be[RTE_DIM(default_rss_key)];

/*
 * Initializes an array of 16 bytes that represents the IPv6 solicited
 * node multicast address. Users of this macro need to pass the IPv6
 * address as an array of 16 bytes, the last three of which are used
 * as the last three bytes of the multicast address as well.
 */
#define IPV6_SN_MC_ADDR(ipv6) {				\
		0xFF, 0x02, 0x00, 0x00,			\
		0x00, 0x00, 0x00, 0x00,			\
		0x00, 0x00, 0x00, 0x01,			\
		0xFF, ipv6[13], ipv6[14], ipv6[15],	\
	}

static inline int
lacp_enabled(struct net_config *net, struct gatekeeper_if *iface)
{
	/* When @iface is the back, need to make sure it's enabled. */
	if (iface == &net->back)
		return net->back_iface_enabled &&
			iface->bonding_mode == BONDING_MODE_8023AD;

	/* @iface is the front interface. */
	return iface->bonding_mode == BONDING_MODE_8023AD;
}

int lua_init_iface(struct gatekeeper_if *iface, const char *iface_name,
	const char **pci_addrs, uint8_t num_pci_addrs,
	const char **ip_cidrs, uint8_t num_ip_cidrs);
void lua_free_iface(struct gatekeeper_if *iface);

int get_ip_type(const char *ip_addr);
int convert_str_to_ip(const char *ip_addr, struct ipaddr *res);
int ethertype_filter_add(uint8_t port_id, uint16_t ether_type,
	uint16_t queue_id);

int ntuple_filter_add(uint8_t portid, uint32_t dst_ip,
	uint16_t src_port, uint16_t src_port_mask,
	uint16_t dst_port, uint16_t dst_port_mask,
	uint8_t proto, uint16_t queue_id, int ipv4_only);
struct net_config *get_net_conf(void);
struct gatekeeper_if *get_if_front(struct net_config *net_conf);
struct gatekeeper_if *get_if_back(struct net_config *net_conf);
int gatekeeper_setup_rss(uint8_t portid, uint16_t *queues, uint16_t num_queues);
int gatekeeper_get_rss_config(uint8_t portid,
	struct gatekeeper_rss_config *rss_conf);
int gatekeeper_init_network(struct net_config *net_conf);
void gatekeeper_free_network(void);
bool ipv4_configured(struct net_config *net_conf);
bool ipv6_configured(struct net_config *net_conf);

static inline bool
ipv4_if_configured(struct gatekeeper_if *iface)
{
	return !!(iface->configured_proto & GK_CONFIGURED_IPV4);
}

static inline bool
ipv6_if_configured(struct gatekeeper_if *iface)
{
	return !!(iface->configured_proto & GK_CONFIGURED_IPV6);
}

static inline int
max_prefix_len(int ip_type)
{
	RTE_VERIFY(ip_type == AF_INET || ip_type == AF_INET6);
	return ip_type == AF_INET
		? sizeof(struct in_addr) * 8
		: sizeof(struct in6_addr) * 8;
}

/*
 * Postpone the execution of f(arg) until the Lua configuration finishes,
 * but before the network devices start.
 *
 * This initilization stage is perfect for allocation of queues in
 * the network devices.
 *
 * If you do not need to allocate any queue, you can may call
 * net_launch_at_stage1() instead.
 *
 * front_rx_queues, front_tx_queues, back_rx_queues, and back_tx_queues are
 * the number of queues on the front and back interfaces of the receiving and
 * transmitting types.
 *
 * If the back interface is not enabled, the parameters back_rx_queues and
 * back_tx_queues are ignored.
 *
 * RETURN
 *	Return 0 if success; otherwise -1.
 */
int
net_launch_at_stage1(struct net_config *net,
	int front_rx_queues, int front_tx_queues,
	int back_rx_queues, int back_tx_queues,
	lcore_function_t *f, void *arg);

/*
 * Do any processing necessary to end stage 2 -- the last part of the
 * network configuration that happens before individual lcores are
 * launched. This is useful for any network configuration that requires
 * input from the individual blocks in stage 2.
 */
int finalize_stage2(void *arg);

#endif /* _GATEKEEPER_NET_H_ */

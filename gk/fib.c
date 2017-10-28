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

#include "gatekeeper_fib.h"
#include "gatekeeper_gk.h"
#include "gatekeeper_main.h"

struct ip_prefix {
	const char    *str;
	struct ipaddr addr;
	int           len;
};

void
destroy_neigh_hash_table(struct neighbor_hash_table *neigh)
{
	if (neigh->cache_tbl != NULL) {
		rte_free(neigh->cache_tbl);
		neigh->cache_tbl = NULL;
	}

	if (neigh->hash_table != NULL) {
		rte_hash_free(neigh->hash_table);
		neigh->hash_table = NULL;
	}
}

static inline int
gk_lpm_add_ipv4_route(uint32_t ip,
	uint8_t depth, uint32_t nexthop, struct gk_lpm *ltbl)
{
	return rte_lpm_add(ltbl->lpm, ntohl(ip), depth, nexthop);
}

static inline int
gk_lpm_add_ipv6_route(uint8_t *ip,
	uint8_t depth, uint32_t nexthop, struct gk_lpm *ltbl)
{
	return rte_lpm6_add(ltbl->lpm6, ip, depth, nexthop);
}

/*
 * This function is only called on cache entries that are not being used,
 * so we don't need a concurrencty mechanism here. However,
 * callers must ensure that the entry is not being used.
 */
static inline int
initialize_ether_cache(struct ether_cache *eth_cache)
{
	memset(eth_cache, 0, sizeof(*eth_cache));

	return 0;
}

/*
 * Fill up the Ethernet cached header.
 * Note that the destination MAC address should be filled up by LLS.
 */
static inline void
fill_up_ether_cache_locked(struct ether_cache *eth_cache,
	struct ipaddr *addr, struct gatekeeper_if *iface)
{
	eth_cache->stale = true;
	rte_memcpy(&eth_cache->ip_addr, addr, sizeof(eth_cache->ip_addr));
	eth_cache->eth_hdr.ether_type = addr->proto;
	ether_addr_copy(&iface->eth_addr, &eth_cache->eth_hdr.s_addr);
	eth_cache->ref_cnt = 1;
}

static struct ether_cache *
neigh_get_ether_cache_locked(struct neighbor_hash_table *neigh,
	struct ipaddr *addr, struct gatekeeper_if *iface)
{
	int ret;
	struct ether_cache *eth_cache = lookup_ether_cache(neigh, &addr->ip);
	if (eth_cache != NULL) {
		eth_cache->ref_cnt++;
		return eth_cache;
	}

	ret = rte_hash_add_key(neigh->hash_table, &addr->ip);
	if (ret >= 0) {
		eth_cache = &neigh->cache_tbl[ret];
		fill_up_ether_cache_locked(eth_cache, addr, iface);
		/*
		 * Function fill_up_ether_cache_locked() already
		 * sets @ref_cnt to 1.
		 */
		return eth_cache;
	}

	RTE_LOG(ERR, HASH,
		"Failed to add a cache entry to the neighbor hash table at %s\n",
		__func__);

	return NULL;
}

/* Warning: avoid calling this function directly, prefer ether_cache_put(). */
static int
neigh_del_ether_cache(struct neighbor_hash_table *neigh, void *key)
{
	int ret = rte_hash_del_key(neigh->hash_table, key);
	if (ret >= 0)
		return initialize_ether_cache(&neigh->cache_tbl[ret]);

	RTE_LOG(ERR, HASH,
		"Failed to delete a cache entry to the neighbor hash table at %s - %s\n",
		__func__, strerror(-ret));

	return -1;
}

static int
parse_ip_prefix(const char *ip_prefix, struct ipaddr *res)
{
	/* Need to make copy to tokenize. */
	size_t ip_prefix_len = ip_prefix != NULL ? strlen(ip_prefix) : 0;
	char ip_prefix_copy[ip_prefix_len + 1];
	char *ip_addr;

	char *saveptr;
	char *prefix_len_str;
	char *end;
	long prefix_len;
	int ip_type;

	if (ip_prefix == NULL)
		return -1;

	strncpy(ip_prefix_copy, ip_prefix, ip_prefix_len + 1);

	ip_addr = strtok_r(ip_prefix_copy, "/", &saveptr);
	if (ip_addr == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to parse IP address in IP prefix %s at %s\n",
			ip_prefix, __func__);
		return -1;
	}

	ip_type = get_ip_type(ip_addr);
	if (ip_type != AF_INET && ip_type != AF_INET6)
		return -1;

	prefix_len_str = strtok_r(NULL, "\0", &saveptr);
	if (prefix_len_str == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to parse prefix length in IP prefix %s at %s\n",
			ip_prefix, __func__);
		return -1;
	}

	prefix_len = strtol(prefix_len_str, &end, 10);
	if (prefix_len_str == end || !*prefix_len_str || *end) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: prefix length \"%s\" is not a number\n",
			prefix_len_str);
		return -1;
	}

	if ((prefix_len == LONG_MAX || prefix_len == LONG_MIN) &&
			errno == ERANGE) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: prefix length \"%s\" caused underflow or overflow\n",
			prefix_len_str);
		return -1;
	}

	if (prefix_len < 0 || prefix_len > max_prefix_len(ip_type)) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: prefix length \"%s\" is out of range\n",
			prefix_len_str);
		return -1;
	}

	if (convert_str_to_ip(ip_addr, res) < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: the IP address part of the IP prefix %s is not valid\n",
			ip_prefix);
		return -1;
	}

	RTE_VERIFY((ip_type == AF_INET && res->proto == ETHER_TYPE_IPv4) ||
		(ip_type == AF_INET6 && res->proto == ETHER_TYPE_IPv6));

	return prefix_len;
}

/* Warning: avoid calling this function directly, prefer get_empty_fib_id(). */
static int
__get_empty_fib_id(struct gk_fib *fib_tbl,
	unsigned int num_fib_entries, struct gk_config *gk_conf)
{
	unsigned int i;

	RTE_VERIFY(fib_tbl == gk_conf->lpm_tbl.fib_tbl ||
		fib_tbl == gk_conf->lpm_tbl.fib_tbl6);

	for (i = 0; i < num_fib_entries; i++) {
		if (fib_tbl[i].action == GK_FIB_MAX)
			return i; 
	}

	if (fib_tbl == gk_conf->lpm_tbl.fib_tbl) {
		RTE_LOG(WARNING, GATEKEEPER,
			"gk: cannot find an empty fib entry in the IPv4 FIB table!\n");
	} else {
		RTE_LOG(WARNING, GATEKEEPER,
			"gk: cannot find an empty fib entry in the IPv6 FIB table!\n");
	}

	return -1;
}

/* This function will return an empty FIB entry. */
static inline int
get_empty_fib_id(uint16_t ip_proto, struct gk_config *gk_conf)
{
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	/* Find an empty FIB entry. */
	if (ip_proto == ETHER_TYPE_IPv4) {
		return __get_empty_fib_id(ltbl->fib_tbl,
			gk_conf->gk_max_num_ipv4_fib_entries, gk_conf);
	}

	if (likely(ip_proto == ETHER_TYPE_IPv6)) {
		return __get_empty_fib_id(ltbl->fib_tbl6,
			gk_conf->gk_max_num_ipv6_fib_entries, gk_conf);
	}

	rte_panic("Unexpected condition at %s: unknown IP type %hu\n",
		__func__, ip_proto);

	return -1;
}

/* Add a prefix into the LPM table. */
static inline int
lpm_add_route(struct ipaddr *ip_addr,
	int prefix_len, int fib_id, struct gk_lpm *ltbl)
{
	if (ip_addr->proto == ETHER_TYPE_IPv4) {
		return gk_lpm_add_ipv4_route(
			ip_addr->ip.v4.s_addr, prefix_len, fib_id, ltbl);
	}

	if (likely(ip_addr->proto == ETHER_TYPE_IPv6)) {
		return gk_lpm_add_ipv6_route(
			ip_addr->ip.v6.s6_addr, prefix_len, fib_id, ltbl);
	}

	rte_panic("Unexpected condition at %s: unknown IP type %hu\n",
		__func__, ip_addr->proto);

	return -1;
}

/* Delete a prefix from the LPM table. */
static inline int
lpm_del_route(struct ipaddr *ip_addr, int prefix_len, struct gk_lpm *ltbl)
{
	if (ip_addr->proto == ETHER_TYPE_IPv4) {
		return rte_lpm_delete(ltbl->lpm,
			ntohl(ip_addr->ip.v4.s_addr), prefix_len);
	}

	if (likely(ip_addr->proto == ETHER_TYPE_IPv6)) {
		return rte_lpm6_delete(ltbl->lpm6,
			ip_addr->ip.v6.s6_addr, prefix_len);
	}

	rte_panic("Unexpected condition at %s: unknown IP type %hu\n",
		__func__, ip_addr->proto);

	return -1;
}

static int
setup_neighbor_tbl(unsigned int socket_id, int identifier,
	int ip_ver, int ht_size, struct neighbor_hash_table *neigh)
{
	int  ret;
	char ht_name[64];
	int key_len = ip_ver == ETHER_TYPE_IPv4 ?
		sizeof(struct in_addr) : sizeof(struct in6_addr);

	struct rte_hash_parameters neigh_hash_params = {
		.entries = ht_size,
		.key_len = key_len,
		.hash_func = DEFAULT_HASH_FUNC,
		.hash_func_init_val = 0,
	};

	ret = snprintf(ht_name, sizeof(ht_name),
		"neighbor_hash_%u", identifier);
	RTE_VERIFY(ret > 0 && ret < (int)sizeof(ht_name));

	/* Setup the neighbor hash table. */
	neigh_hash_params.name = ht_name;
	neigh_hash_params.socket_id = socket_id;
	neigh->hash_table = rte_hash_create(&neigh_hash_params);
	if (neigh->hash_table == NULL) {
		RTE_LOG(ERR, HASH,
			"The GK block cannot create hash table for neighbor FIB!\n");

		ret = -1;
		goto out;
	}

	/* Setup the Ethernet header cache table. */
	neigh->cache_tbl = rte_calloc(NULL,
		ht_size, sizeof(struct ether_cache), 0);
	if (neigh->cache_tbl == NULL) {
		RTE_LOG(ERR, MALLOC,
			"The GK block cannot create Ethernet header cache table\n");

		ret = -1;
		goto neigh_hash;
	}

	ret = 0;
	goto out;

neigh_hash:
	rte_hash_free(neigh->hash_table);
	neigh->hash_table = NULL;
out:
	return ret;
}

/*
 * The caller is responsible for releasing any resource associated to @fib.
 * For example, if the FIB entry has action GK_FWD_NEIGHBOR_*_NET,
 * then the caller needs to first destroy the neighbor hash table before
 * calling this function.
 */
static inline void
initialize_fib_entry(struct gk_fib *fib)
{
	/* Reset the fields of the deleted FIB entry. */
	fib->action = GK_FIB_MAX;
	rte_atomic16_init(&fib->num_updated_instances);
	memset(&fib->u, 0, sizeof(fib->u));
}

/*
 * Setup the FIB entries for the network prefixes, for which @iface
 * is responsible.
 * These prefixes are configured when the Gatekeeper server starts.
 */
static int
setup_net_prefix_fib(int identifier,
	struct gk_fib **neigh_fib, struct gk_fib **neigh6_fib,
	struct gatekeeper_if *iface, struct gk_config *gk_conf)
{
	int ret;
	int fib_id;
	unsigned int socket_id = rte_lcore_to_socket_id(gk_conf->lcores[0]);
	struct net_config *net_conf = gk_conf->net;
	struct gk_fib *neigh_fib_ipv4 = NULL;
	struct gk_fib *neigh_fib_ipv6 = NULL;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	/* Set up the FIB entry for the IPv4 network prefix. */
	if (ipv4_if_configured(iface)) {
		fib_id = get_empty_fib_id(ETHER_TYPE_IPv4, gk_conf);
		if (fib_id < 0)
			goto out;

		neigh_fib_ipv4 = &ltbl->fib_tbl[fib_id];

		ret = setup_neighbor_tbl(socket_id, (identifier * 2),
			ETHER_TYPE_IPv4, (1 << (32 - iface->ip4_addr_plen)),
			&neigh_fib_ipv4->u.neigh);
		if (ret < 0)
			goto init_fib_ipv4;

		if (iface == &net_conf->front)
			neigh_fib_ipv4->action = GK_FWD_NEIGHBOR_FRONT_NET;
		else if (likely(iface == &net_conf->back))
			neigh_fib_ipv4->action = GK_FWD_NEIGHBOR_BACK_NET;
		else
			rte_panic("Unexpected condition at %s: invalid interface %s!\n",
				__func__, iface->name);

		ret = gk_lpm_add_ipv4_route(iface->ip4_addr.s_addr,
			iface->ip4_addr_plen, fib_id, ltbl);
		if (ret < 0)
			goto free_fib_ipv4_ht;

		*neigh_fib = neigh_fib_ipv4;
	}

	/* Set up the FIB entry for the IPv6 network prefix. */
	if (ipv6_if_configured(iface)) {
		fib_id = get_empty_fib_id(ETHER_TYPE_IPv6, gk_conf);
		if (fib_id < 0)
			goto free_fib_ipv4;

		neigh_fib_ipv6 = &ltbl->fib_tbl6[fib_id];

		ret = setup_neighbor_tbl(socket_id, (identifier * 2 + 1),
			ETHER_TYPE_IPv6, gk_conf->max_num_ipv6_neighbors,
			&neigh_fib_ipv6->u.neigh6);
		if (ret < 0)
			goto init_fib_ipv6;

		if (iface == &net_conf->front)
			neigh_fib_ipv6->action = GK_FWD_NEIGHBOR_FRONT_NET;
		else if (likely(iface == &net_conf->back))
			neigh_fib_ipv6->action = GK_FWD_NEIGHBOR_BACK_NET;
		else
			rte_panic("Unexpected condition at %s: invalid interface %s!\n",
				__func__, iface->name);

		ret = gk_lpm_add_ipv6_route(iface->ip6_addr.s6_addr,
			iface->ip6_addr_plen, fib_id, ltbl);
		if (ret < 0)
			goto free_fib_ipv6_ht;

		*neigh6_fib = neigh_fib_ipv6;
	}

	return 0;

free_fib_ipv6_ht:
	destroy_neigh_hash_table(&neigh_fib_ipv6->u.neigh);

init_fib_ipv6:
	initialize_fib_entry(neigh_fib_ipv6);

free_fib_ipv4:
	if (neigh_fib_ipv4 == NULL)
		return -1;

	*neigh_fib = NULL;

	RTE_VERIFY(rte_lpm_delete(ltbl->lpm, ntohl(iface->ip4_addr.s_addr),
		iface->ip4_addr_plen) == 0);

free_fib_ipv4_ht:
	destroy_neigh_hash_table(&neigh_fib_ipv4->u.neigh);

init_fib_ipv4:
	initialize_fib_entry(neigh_fib_ipv4);

out:
	return -1;
}

static int
init_fib_tbl(struct gk_config *gk_conf)
{
	int ret;
	unsigned int i;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gk_fib *neigh_fib_front = NULL, *neigh6_fib_front = NULL;
	struct gk_fib *neigh_fib_back = NULL, *neigh6_fib_back = NULL;

	rte_spinlock_init(&ltbl->lock);

	for (i = 0; i < gk_conf->gk_max_num_ipv4_fib_entries; i++) {
		ltbl->fib_tbl[i].action = GK_FIB_MAX;
		rte_atomic16_init(&ltbl->fib_tbl[i].num_updated_instances);
	}

	for (i = 0; i < gk_conf->gk_max_num_ipv6_fib_entries; i++) {
		ltbl->fib_tbl6[i].action = GK_FIB_MAX;
		rte_atomic16_init(&ltbl->fib_tbl6[i].num_updated_instances);
	}

	/* Set up the FIB entry for the front network prefixes. */
	ret = setup_net_prefix_fib(0, &neigh_fib_front,
		&neigh6_fib_front, &gk_conf->net->front, gk_conf);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to setup the FIB entry for the front network prefixes at %s\n",
			__func__);
		goto out;
	}

	/* Set up the FIB entry for the back network prefixes. */
	RTE_VERIFY(gk_conf->net->back_iface_enabled);
	ret = setup_net_prefix_fib(1, &neigh_fib_back,
		&neigh6_fib_back, &gk_conf->net->back, gk_conf);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to setup the FIB entry for the back network prefixes at %s\n",
			__func__);
		goto free_front_fibs;
	}

	return 0;

free_front_fibs:
	if (neigh_fib_front != NULL) {
		struct gatekeeper_if *iface = &gk_conf->net->front;
		RTE_VERIFY(rte_lpm_delete(gk_conf->lpm_tbl.lpm,
			ntohl(iface->ip4_addr.s_addr),
			iface->ip4_addr_plen) == 0);
		destroy_neigh_hash_table(&neigh_fib_front->u.neigh);
		initialize_fib_entry(neigh_fib_front);
		neigh_fib_front = NULL;
	}
	if (neigh6_fib_front != NULL) {
		struct gatekeeper_if *iface = &gk_conf->net->front;
		RTE_VERIFY(rte_lpm6_delete(gk_conf->lpm_tbl.lpm6,
			iface->ip6_addr.s6_addr, iface->ip6_addr_plen) == 0);
		destroy_neigh_hash_table(&neigh6_fib_front->u.neigh6);
		initialize_fib_entry(neigh6_fib_front);
		neigh6_fib_front = NULL;
	}

out:
	return ret;
}

int
setup_gk_lpm(struct gk_config *gk_conf, unsigned int socket_id)
{
	int ret;
	struct rte_lpm_config ipv4_lpm_config;
	struct rte_lpm6_config ipv6_lpm_config;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	if (ipv4_configured(gk_conf->net)) {
		ipv4_lpm_config.max_rules = gk_conf->max_num_ipv4_rules;
		ipv4_lpm_config.number_tbl8s = gk_conf->num_ipv4_tbl8s;

		/*
		 * The GK blocks only need to create one single
		 * IPv4 LPM table on the @socket_id, so the
		 * @lcore and @identifier are set to 0.
		 */
		ltbl->lpm = init_ipv4_lpm(
			"gk", &ipv4_lpm_config, socket_id, 0, 0);
		if (ltbl->lpm == NULL) {
			RTE_LOG(ERR, GATEKEEPER,
				"gk: failed to initialize the IPv4 LPM table at %s\n",
				__func__);
			ret = -1;
			goto out;
		}

		ltbl->fib_tbl = rte_calloc(NULL,
			gk_conf->gk_max_num_ipv4_fib_entries,
			sizeof(struct gk_fib), 0);
		if (ltbl->fib_tbl == NULL) {
			RTE_LOG(ERR, MALLOC,
				"gk: failed to allocate the IPv4 FIB table at %s\n",
				__func__);
			ret = -1;
			goto free_lpm;
		}
	}

	if (ipv6_configured(gk_conf->net)) {
		ipv6_lpm_config.max_rules = gk_conf->max_num_ipv6_rules;
		ipv6_lpm_config.number_tbl8s = gk_conf->num_ipv6_tbl8s;

		/*
		 * The GK blocks only need to create one single
		 * IPv6 LPM table on the @socket_id, so the
		 * @lcore and @identifier are set to 0.
		 */
		ltbl->lpm6 = init_ipv6_lpm(
			"gk", &ipv6_lpm_config, socket_id, 0, 0);
		if (ltbl->lpm6 == NULL) {
			RTE_LOG(ERR, GATEKEEPER,
				"gk: failed to initialize the IPv6 LPM table at %s\n",
				__func__);
			ret = -1;
			goto free_lpm_tbl;
		}

		ltbl->fib_tbl6 = rte_calloc(NULL,
			gk_conf->gk_max_num_ipv6_fib_entries,
			sizeof(struct gk_fib), 0);
		if (ltbl->fib_tbl6 == NULL) {
			RTE_LOG(ERR, MALLOC,
				"gk: failed to allocate the IPv6 FIB table at %s\n",
				__func__);
			ret = -1;
			goto free_lpm6;
		}
	}

	ret = init_fib_tbl(gk_conf);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to initialize the FIB table at %s\n",
			__func__);
		goto free_lpm_tbl6;
	}

	ret = 0;
	goto out;

free_lpm_tbl6:
	if (!ipv6_configured(gk_conf->net))
		goto free_lpm_tbl;

	rte_free(ltbl->fib_tbl6);
	ltbl->fib_tbl6 = NULL;

free_lpm6:
	destroy_ipv6_lpm(ltbl->lpm6);
	ltbl->lpm6 = NULL;

free_lpm_tbl:
	if (!ipv4_configured(gk_conf->net))
		goto out;

	rte_free(ltbl->fib_tbl);
	ltbl->fib_tbl = NULL;

free_lpm:
	destroy_ipv4_lpm(ltbl->lpm);
	ltbl->lpm = NULL;

out:
	return ret;
}

static int
notify_gk_instance(struct gk_fib *fib, struct gk_instance *instance)
{
	int ret;
	struct mailbox *mb = &instance->mb;
	struct gk_cmd_entry *entry = mb_alloc_entry(mb);
	if (entry == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to allocate a `struct gk_cmd_entry` entry at %s\n",
			__func__);
		return -1;
	}

	entry->op = GK_SYNCH_WITH_LPM;
	entry->u.fib = fib;

	ret = mb_send_entry(mb, entry);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to send a `struct gk_cmd_entry` entry at %s\n",
			__func__);
		return -1;
	}

	return 0;
}

/*
 * XXX What we are doing here is analogous to RCU's synchronize_rcu(),
 * what suggests that we may be able to profit from RCU. But we are going
 * to postpone that until we have a better case to bring RCU to Gatekeeper.
 */
static void
synchronize_gk_instances(struct gk_fib *fib, struct gk_config *gk_conf)
{
	int i, loop;
	int num_succ_notified_inst = 0;
	bool is_succ_notified[gk_conf->num_lcores];

	/* The maximum number of times to try to notify the GK instances. */
	const int MAX_NUM_NOTIFY_TRY = 3;

	rte_atomic16_init(&fib->num_updated_instances);

	memset(is_succ_notified, false, sizeof(is_succ_notified));

	for (loop = 0; loop < MAX_NUM_NOTIFY_TRY; loop++) {
		/* Send the FIB entry to the GK mailboxes. */
		for (i = 0; i < gk_conf->num_lcores; i++) {
			if (!is_succ_notified[i]) {
				int ret = notify_gk_instance(fib,
					&gk_conf->instances[i]);
				if (ret == 0) {
					is_succ_notified[i] = true;
					num_succ_notified_inst++;
					if (num_succ_notified_inst >=
							gk_conf->num_lcores)
						goto finish_notify;
				}
			}
		}
	}

finish_notify:

	if (num_succ_notified_inst != gk_conf->num_lcores) {
		RTE_LOG(WARNING, GATEKEEPER,
			"gk: %s successfully notifies only %d/%d instances\n",
			__func__, num_succ_notified_inst, gk_conf->num_lcores);
	}

	/* Wait for all GK instances to synchronize. */
	while (rte_atomic16_read(&fib->num_updated_instances)
			< num_succ_notified_inst)
		rte_pause();
}

/*
 * This function is called by del_fib_entry_locked().
 * Notice that, it doesn't stand on its own, and it's only
 * a construct to make del_fib_entry_locked() readable.
 */
static struct gk_fib *
remove_prefix_from_lpm_locked(
	struct ip_prefix *ip_prefix, struct gk_config *gk_conf)
{
	int ret = 0;
	int ip_prefix_present;
	struct gk_fib *ip_prefix_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	if (ip_prefix->addr.proto == ETHER_TYPE_IPv4) {
		uint32_t fib_id;

		ip_prefix_present = rte_lpm_is_rule_present(
			ltbl->lpm, ntohl(ip_prefix->addr.ip.v4.s_addr),
			ip_prefix->len, &fib_id);
		if (!ip_prefix_present) {
			RTE_LOG(WARNING, GATEKEEPER,
				"gk: delete an non-existent IP prefix (%s)\n",
				ip_prefix->str);
			return NULL;
		}

		ip_prefix_fib = &ltbl->fib_tbl[fib_id];
	} else if (likely(ip_prefix->addr.proto == ETHER_TYPE_IPv6)) {
		uint8_t fib_id;

		ip_prefix_present = rte_lpm6_is_rule_present(
			ltbl->lpm6, ip_prefix->addr.ip.v6.s6_addr,
			ip_prefix->len, &fib_id);
		if (!ip_prefix_present) {
			RTE_LOG(WARNING, GATEKEEPER,
				"gk: delete an non-existent IP prefix (%s)\n",
				ip_prefix->str);
			return NULL;
		}

		ip_prefix_fib = &ltbl->fib_tbl6[fib_id];
	} else {
		RTE_LOG(WARNING, GATEKEEPER,
			"gk: delete an IP prefix (%s) with unknown IP type %hu\n",
			ip_prefix->str, ip_prefix->addr.proto);
		return NULL;
	}

	ret = lpm_del_route(&ip_prefix->addr, ip_prefix->len, ltbl);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: cannot remove the IP prefix %s from LPM table\n",
			ip_prefix->str);
		return NULL;
	}

	return ip_prefix_fib;
}

/*
 * Note that, @action should be either GK_FWD_GATEWAY_FRONT_NET
 * or GK_FWD_GATEWAY_BACK_NET.
 */
static struct gk_fib *
find_fib_entry_for_neighbor_locked(struct ipaddr *gw_addr,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	int fib_id;
	struct gk_fib *neigh_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gatekeeper_if *iface;

	if (action == GK_FWD_GATEWAY_FRONT_NET)
		iface = &gk_conf->net->front;
	else if (likely(action == GK_FWD_GATEWAY_BACK_NET))
		iface = &gk_conf->net->back;
	else {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to delete a Gateway ethernet cache entry from neighbor hash table, since it has invalid action %d.\n",
			action);
		return NULL;
	}

	if (gw_addr->proto == ETHER_TYPE_IPv4 &&
			ipv4_if_configured(iface)) {
		fib_id = lpm_lookup_ipv4(ltbl->lpm, gw_addr->ip.v4.s_addr);
		/*
		 * Invalid gateway entry, since at least we should
		 * obtain the FIB entry for the neighbor table.
		 */
		if (fib_id < 0)
			return NULL;

		neigh_fib = &ltbl->fib_tbl[fib_id];
	} else if (likely(gw_addr->proto == ETHER_TYPE_IPv6)
			&& ipv6_if_configured(iface)) {
		fib_id = lpm_lookup_ipv6(ltbl->lpm6, gw_addr->ip.v6.s6_addr);
		/*
		 * Invalid gateway entry, since at least we should
		 * obtain the FIB entry for the neighbor table.
		 */
		if (fib_id < 0)
			return NULL;

		neigh_fib = &ltbl->fib_tbl6[fib_id];
	} else {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: unconfigued IP type %hu at interface %s!\n",
			gw_addr->proto, iface->name);
		return NULL;
	}

	/*
	 * Invalid gateway entry, since the neighbor entry
	 * and the gateway entry should be in the same network.
	 */
	if ((action == GK_FWD_GATEWAY_FRONT_NET &&
			neigh_fib->action != GK_FWD_NEIGHBOR_FRONT_NET)
			|| (action == GK_FWD_GATEWAY_BACK_NET &&
			neigh_fib->action !=
			GK_FWD_NEIGHBOR_BACK_NET))
		return NULL;

	return neigh_fib;
}

static int
ether_cache_put(struct gk_fib *neigh_fib,
	enum gk_fib_action action, struct ether_cache *eth_cache,
	struct ipaddr *addr, struct gk_config *gk_conf)
{
	struct gk_fib *neighbor_fib = neigh_fib;

	if (eth_cache->ref_cnt == 0) {
		rte_panic("Unexpected condition: the ref_cnt of the ether cache is 0 at %s\n",
			__func__);
	}

	if (eth_cache->ref_cnt > 1) {
		eth_cache->ref_cnt--;
		return 0;
	}

	/*
	 * Find the FIB entry for the @addr.
	 * We need to release the @eth_cache
	 * Ethernet header entry from the neighbor hash table.
	 */
	if (neighbor_fib == NULL) {
		neighbor_fib = find_fib_entry_for_neighbor_locked(
			addr, action, gk_conf);
		if (neighbor_fib == NULL)
			return -1;
	}

	if (addr->proto == ETHER_TYPE_IPv4) {
		return neigh_del_ether_cache(
			&neighbor_fib->u.neigh, &addr->ip.v4.s_addr);
	}

	if (likely(addr->proto == ETHER_TYPE_IPv6)) {
		return neigh_del_ether_cache(
			&neighbor_fib->u.neigh6, addr->ip.v6.s6_addr);
	}

	RTE_LOG(ERR, GATEKEEPER,
		"gk: remove an invalid FIB entry with IP type %hu at %s",
		addr->proto, __func__);

	return -1;
}

/*
 * This function is called by del_fib_entry_locked().
 * Notice that, it doesn't stand on its own, and it's only
 * a construct to make del_fib_entry_locked() readable.
 */
static int
del_gateway_from_neigh_table_locked(
	struct ip_prefix *ip_prefix, enum gk_fib_action action,
	struct ether_cache *eth_cache, struct gk_config *gk_conf)
{
	int ret;
	struct ipaddr *gw_addr = &eth_cache->ip_addr;

	ret = ether_cache_put(NULL, action, eth_cache, gw_addr, gk_conf);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to release the Ethernet cached header of the Grantor FIB entry for the IP prefix %s at %s\n",
			ip_prefix->str, __func__);
		return -1;
	}

	return 0;
}

/*
 * For removing FIB entries, it needs to notify the GK instances
 * about the removal of the FIB entry.
 */
static int
del_fib_entry_locked(struct ip_prefix *ip_prefix, struct gk_config *gk_conf)
{
	int ret = 0;
	struct gk_fib *ip_prefix_fib;

	ip_prefix_fib = remove_prefix_from_lpm_locked(ip_prefix, gk_conf);
	if (ip_prefix_fib == NULL)
		return -1;

	/*
	 * We need to notify the GK blocks whenever we remove
	 * a FIB entry that is accessible through a prefix.
	 */
	synchronize_gk_instances(ip_prefix_fib, gk_conf);

	/*
	 * From now on, GK blocks must not have a reference
	 * to @ip_prefix_fib.
	 */

	switch (ip_prefix_fib->action) {
	case GK_FWD_GRANTOR: {
		ret = del_gateway_from_neigh_table_locked(
			ip_prefix, GK_FWD_GATEWAY_BACK_NET,
			ip_prefix_fib->u.grantor.eth_cache, gk_conf);

		break;
	}

	case GK_FWD_GATEWAY_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_GATEWAY_BACK_NET: {
		ret = del_gateway_from_neigh_table_locked(
			ip_prefix, ip_prefix_fib->action,
			ip_prefix_fib->u.gateway.eth_cache, gk_conf);

		break;
	}

	case GK_DROP:
		break;

	/*
	 * For GK_FWD_NEIGHBOR_*_NET FIB entries, they are initialized
	 * when the Gatekeeper starts. These FIB entries are only reserved
	 * for the network prefixes, for which the Gatekeeper is responsible.
	 * If we change the network prefixes, then the Gatekeeper may
	 * need to restart, so one can ignore deletion of these FIB entries.
	 */
	case GK_FWD_NEIGHBOR_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_NEIGHBOR_BACK_NET:
		/* FALLTHROUGH */
	default:
		rte_panic("Unexpected condition at %s: unsupported prefix action %u\n",
			__func__, ip_prefix_fib->action);
		ret = -1;
		break;
	}

	/* Reset the fields of the deleted FIB entry. */
	initialize_fib_entry(ip_prefix_fib);

	return ret;
}

/*
 * Initialize a gateway FIB entry.
 * @gateway the gateway address informaiton.
 * @ip_prefix the IP prefix,
 * for which the gateway is responsible.
 */
static struct gk_fib *
init_gateway_fib_locked(struct ip_prefix *ip_prefix, enum gk_fib_action action,
	struct ipaddr *gw_addr, struct gk_config *gk_conf)
{
	int ret, fib_id;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gk_fib *gw_fib, *neigh_fib;
	struct ether_cache *eth_cache;
	struct neighbor_hash_table *neigh_ht;
	struct gatekeeper_if *iface;

	if (gw_addr->proto != ip_prefix->addr.proto) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to initialize a fib entry for gateway, since the gateway and its responsible IP prefix have different IP versions.\n");
		return NULL;
	}

	if (action == GK_FWD_GATEWAY_FRONT_NET)
		iface = &gk_conf->net->front;
	else if (likely(action == GK_FWD_GATEWAY_BACK_NET))
		iface = &gk_conf->net->back;
	else {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to initialize a fib entry for gateway, since it has invalid action %d.\n",
			action);
		return NULL;
	}

	/* Find the neighbor FIB entry for this gateway. */
	neigh_fib = find_fib_entry_for_neighbor_locked(
		gw_addr, action, gk_conf);
	if (neigh_fib == NULL)
		return NULL;

	/* Find the Ethernet cached header entry for this gateway. */
	neigh_ht = &neigh_fib->u.neigh;
	eth_cache = neigh_get_ether_cache_locked(neigh_ht, gw_addr, iface);
	if (eth_cache == NULL)
		return NULL;

	/* Find an empty FIB entry for the Gateway. */
	fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf);
	if (fib_id < 0)
		goto put_ether_cache;

	if (ip_prefix->addr.proto == ETHER_TYPE_IPv4)
		gw_fib = &ltbl->fib_tbl[fib_id];
	else
		gw_fib = &ltbl->fib_tbl6[fib_id];

	/* Fills up the Gateway FIB entry for the IP prefix. */
	gw_fib->action = action;
	gw_fib->u.gateway.eth_cache = eth_cache;

	ret = lpm_add_route(&ip_prefix->addr, ip_prefix->len, fib_id, ltbl);
	if (ret < 0)
		goto init_fib;

	return gw_fib;

init_fib:
	initialize_fib_entry(gw_fib);

put_ether_cache:
	ether_cache_put(neigh_fib, action, eth_cache, gw_addr, gk_conf);

	return NULL;
}

static struct gk_fib *
init_grantor_fib_locked(
	struct ip_prefix *ip_prefix, struct ipaddr *gt_addr,
	struct ipaddr *gw_addr, struct gk_config *gk_conf)
{
	int ret, fib_id;
	struct gk_fib *gt_fib;
	struct ether_cache *eth_cache;
	struct neighbor_hash_table *neigh_ht = NULL;
	struct gatekeeper_if *iface = &gk_conf->net->back;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gk_fib *neigh_fib;

	if (gt_addr->proto != ip_prefix->addr.proto) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to initialize a fib entry for grantor, since the grantor and its responsible IP prefix have different IP versions.\n");
		return NULL;
	}

	if (gw_addr->proto != ip_prefix->addr.proto) {
		RTE_LOG(ERR, GATEKEEPER,
			"gk: failed to initialize a fib entry for grantor, since the gateway and its responsible IP prefix have different IP versions.\n");
		return NULL;
	}

	/* Find the neighbor FIB entry for this gateway. */
	neigh_fib = find_fib_entry_for_neighbor_locked(
		gw_addr, GK_FWD_GATEWAY_BACK_NET, gk_conf);
	if (neigh_fib == NULL)
		return NULL;

	/* Find the Ethernet cached header entry for this gateway. */
	neigh_ht = &neigh_fib->u.neigh;
	eth_cache = neigh_get_ether_cache_locked(neigh_ht, gw_addr, iface);
	if (eth_cache == NULL)
		return NULL;

	fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf);
	if (fib_id < 0)
		goto put_ether_cache;

	if (ip_prefix->addr.proto == ETHER_TYPE_IPv4)
		gt_fib = &ltbl->fib_tbl[fib_id];
	else
		gt_fib = &ltbl->fib_tbl6[fib_id];

	gt_fib->action = GK_FWD_GRANTOR;
	rte_memcpy(&gt_fib->u.grantor.gt_addr,
		gt_addr, sizeof(gt_fib->u.grantor.gt_addr));
	gt_fib->u.grantor.eth_cache = eth_cache;

	ret = lpm_add_route(&ip_prefix->addr, ip_prefix->len, fib_id, ltbl);
	if (ret < 0)
		goto init_fib;

	return gt_fib;

init_fib:
	initialize_fib_entry(gt_fib);

put_ether_cache:
	ether_cache_put(neigh_fib,
		GK_FWD_GATEWAY_BACK_NET, eth_cache, gw_addr, gk_conf);

	return NULL;
}

static struct gk_fib *
init_drop_fib_locked(struct ip_prefix *ip_prefix, struct gk_config *gk_conf)
{
	int ret;
	struct gk_fib *ip_prefix_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	/* Initialize the fib entry for the IP prefix. */
	int fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf);
	if (fib_id < 0)
		return NULL;

	if (ip_prefix->addr.proto == ETHER_TYPE_IPv4)
		ip_prefix_fib = &ltbl->fib_tbl[fib_id];
	else if (likely(ip_prefix->addr.proto == ETHER_TYPE_IPv6))
		ip_prefix_fib = &ltbl->fib_tbl6[fib_id];
	else
		rte_panic("Unexpected condition at gk: unknown IP type %hu at %s",
			ip_prefix->addr.proto, __func__);

	ip_prefix_fib->action = GK_DROP;

	ret = lpm_add_route(&ip_prefix->addr, ip_prefix->len, fib_id, ltbl);
	if (ret < 0) {
		initialize_fib_entry(ip_prefix_fib);
		return NULL;
	}

	return ip_prefix_fib;
}

static int
add_fib_entry_locked(struct ip_prefix *prefix,
	struct ipaddr *gt_addr, struct ipaddr *gw_addr,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	switch (action) {
	case GK_FWD_GRANTOR: {
		struct gk_fib *gt_fib;

 		if (gt_addr == NULL || gw_addr == NULL)
			return -1;

		gt_fib = init_grantor_fib_locked(
			prefix, gt_addr, gw_addr, gk_conf);
		if (gt_fib == NULL)
			return -1;

		/*
	 	 * TODO The nexthop MAC address should be initialized.
	 	 */
		break;
	}

	case GK_FWD_GATEWAY_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_GATEWAY_BACK_NET: {
		struct gk_fib *gw_fib;

 		if (gt_addr != NULL || gw_addr == NULL)
			return -1;

		gw_fib = init_gateway_fib_locked(
			prefix, action, gw_addr, gk_conf);
		if (gw_fib == NULL)
			return -1;

		/*
	 	 * TODO The nexthop MAC address should be initialized.
	 	 */
		break;
	}

	case GK_DROP: {
		struct gk_fib *ip_prefix_fib;

		if (gt_addr != NULL || gw_addr != NULL)
			return -1;

		ip_prefix_fib = init_drop_fib_locked(prefix, gk_conf);
		if (ip_prefix_fib == NULL)
			return -1;

		break;
	}

	case GK_FWD_NEIGHBOR_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_NEIGHBOR_BACK_NET:
		/* FALLTHROUGH */
	default:
		RTE_LOG(ERR, GATEKEEPER,
			"Invalid fib action %u at %s\n", action, __func__);
		return -1;
		break;
	}

	return 0;
}

/* Return 0 when @gw_addr is not included in @prefix; otherwise return -1. */
static int
check_gateway_prefix(struct ip_prefix *prefix, struct ipaddr *gw_addr)
{
	if (gw_addr->proto == ETHER_TYPE_IPv4) {
		uint32_t ip4_mask =
			rte_cpu_to_be_32(~0ULL << (32 - prefix->len));
		if ((prefix->addr.ip.v4.s_addr ^
				gw_addr->ip.v4.s_addr) & ip4_mask)
			return 0;
	} else if (likely(gw_addr->proto == ETHER_TYPE_IPv6)) {
		uint64_t ip6_mask;
		uint64_t *pf = (uint64_t *)prefix->addr.ip.v6.s6_addr;
		uint64_t *gw = (uint64_t *)gw_addr->ip.v6.s6_addr;

		if (prefix->len == 0)
			return -1;
		else if (prefix->len <= 64) {
			ip6_mask = rte_cpu_to_be_64(
				~0ULL << (64 - prefix->len));
			if ((pf[0] ^ gw[0]) & ip6_mask)
				return 0;
		} else {
			ip6_mask = rte_cpu_to_be_64(
				~0ULL << (128 - prefix->len));
			if ((pf[0] != gw[0]) ||
					((pf[1] ^ gw[1]) & ip6_mask))
				return 0;
		}
	} else
		rte_panic("Unexpected condition at %s: unknown IP type %hu\n",
			__func__, gw_addr->proto);

	return -1;
}

/*
 * This function makes sure that only a drop or another grantor entry
 * must be able to be longer than a grantor or a drop prefix.
 */
static int
check_prefix_locked(struct ip_prefix *prefix,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	uint8_t i;
	int ip_prefix_present;
	struct gk_fib *ip_prefix_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	if (action == GK_DROP || action == GK_FWD_GRANTOR) {
		int num_rules;

		if (prefix->addr.proto == ETHER_TYPE_IPv4) {
			struct rte_lpm_rule_entry re_tbl[
				gk_conf->max_num_ipv4_rules];
			num_rules = rte_lpm_rule_iterator(
				ltbl->lpm, ntohl(prefix->addr.ip.v4.s_addr),
				prefix->len, re_tbl);
			for (i = 0; i < num_rules; i++) {
				struct rte_lpm_rule_entry *re = &re_tbl[i];
				ip_prefix_fib = &ltbl->fib_tbl[re->next_hop];
				if (ip_prefix_fib->action != GK_FWD_GRANTOR &&
						ip_prefix_fib->action !=
						GK_DROP)
					return -1;
			}
		} else {
			struct rte_lpm6_rule re_tbl6[
				gk_conf->max_num_ipv6_rules];
			num_rules = rte_lpm6_rule_iterator(
				ltbl->lpm6, prefix->addr.ip.v6.s6_addr,
				prefix->len, re_tbl6);
			for (i = 0; i < num_rules; i++) {
				struct rte_lpm6_rule *re = &re_tbl6[i];
				ip_prefix_fib = &ltbl->fib_tbl6[re->next_hop];
				if (ip_prefix_fib->action != GK_FWD_GRANTOR &&
						ip_prefix_fib->action !=
						GK_DROP)
					return -1;
			}
		}

		return 0;
	}

	if (prefix->addr.proto == ETHER_TYPE_IPv4) {
		for (i = 0; i < prefix->len; i++) {
			uint32_t fib_id;
			ip_prefix_present = rte_lpm_is_rule_present(
				ltbl->lpm, ntohl(prefix->addr.ip.v4.s_addr),
				i, &fib_id);
			if (!ip_prefix_present)
				continue;

			ip_prefix_fib = &ltbl->fib_tbl[fib_id];
			if (ip_prefix_fib->action == GK_FWD_GRANTOR ||
					ip_prefix_fib->action == GK_DROP)
				return -1;
		}
	} else {
		for (i = 0; i < prefix->len; i++) {
			uint8_t fib_id;
			ip_prefix_present = rte_lpm6_is_rule_present(
				ltbl->lpm6, prefix->addr.ip.v6.s6_addr,
				i, &fib_id);
			if (!ip_prefix_present)
				continue;

			ip_prefix_fib = &ltbl->fib_tbl6[fib_id];
			if (ip_prefix_fib->action == GK_FWD_GRANTOR ||
					ip_prefix_fib->action == GK_DROP)
				return -1;
		}
	}

	return 0;
}

int
add_fib_entry(const char *prefix, const char *gt_ip, const char *gw_ip,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	int ret;
	struct ip_prefix prefix_info;
	struct gk_fib *neigh_fib;
	struct ipaddr gt_addr, gw_addr;
	struct ipaddr *gt_para = NULL, *gw_para = NULL;

	prefix_info.str = prefix;
	prefix_info.len = parse_ip_prefix(prefix, &prefix_info.addr);
	if (prefix_info.len < 0)
		return -1;

	/*
	 * One can only look up, without the lock, the LPM table to verify that
	 * the adding prefix does not lead to a GK_FWD_NEIGHBOR_*_NET FIB entry
	 * because GK_FWD_NEIGHBOR_*_NET entries can only be added through
	 * a network interface.
	 * Otherwise, after the lookup, but before acquiring the lock,
	 * a concurrent thread could add a GK_FWD_NEIGHBOR_*_NET entry that
	 * would break the test.
	 */
	neigh_fib = find_fib_entry_for_neighbor_locked(
		&prefix_info.addr, GK_FWD_GATEWAY_FRONT_NET, gk_conf);
	if (neigh_fib != NULL)
		return -1;

	neigh_fib = find_fib_entry_for_neighbor_locked(
		&prefix_info.addr, GK_FWD_GATEWAY_BACK_NET, gk_conf);
	if (neigh_fib != NULL)
		return -1;

	if (gt_ip != NULL) {
		ret = convert_str_to_ip(gt_ip, &gt_addr);
		if (ret < 0)
			return -1;
		gt_para = &gt_addr;
	}

	if (gw_ip != NULL) {
		ret = convert_str_to_ip(gw_ip, &gw_addr);
		if (ret < 0)
			return -1;

		/*
		 * Verify that the IP addresses of gateways FIB entries
		 * are not included in their prefixes.
		 */
		ret = check_gateway_prefix(&prefix_info, &gw_addr);
		if (ret < 0)
			return -1;
		
		gw_para = &gw_addr;
	}

	/*
	 * Only a drop or another grantor entry must be able to be longer than
	 * a grantor or a drop prefix. This way we protect network operators of
	 * accidentally create a security hole.
	 */
	rte_spinlock_lock_tm(&gk_conf->lpm_tbl.lock);
	ret = check_prefix_locked(&prefix_info, action, gk_conf);
	if (ret < 0) {
		rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);
		return -1;
	}

	ret = add_fib_entry_locked(
		&prefix_info, gt_para, gw_para, action, gk_conf);
	rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);

	return ret;
}

int
del_fib_entry(const char *ip_prefix, struct gk_config *gk_conf)
{
	int ret;
	struct ip_prefix prefix_info;

	prefix_info.str = ip_prefix;
	prefix_info.len = parse_ip_prefix(ip_prefix, &prefix_info.addr);
	if (prefix_info.len < 0)
		return -1;

	rte_spinlock_lock_tm(&gk_conf->lpm_tbl.lock);
	ret = del_fib_entry_locked(&prefix_info, gk_conf);
	rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);

	return ret;
}

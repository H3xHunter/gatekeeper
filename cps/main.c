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

#include <rte_tcp.h>
#include <rte_cycles.h>

#include "gatekeeper_acl.h"
#include "gatekeeper_cps.h"
#include "gatekeeper_launch.h"
#include "gatekeeper_lls.h"
#include "gatekeeper_varip.h"
#include "kni.h"

/*
 * To capture BGP packets with source port 179 or destination port 179
 * on a global IPv6 address, we need two rules (per interface).
 */
#define NUM_ACL_BGP_RULES (2)

/* XXX Sample parameters, need to be tested for better performance. */
#define CPS_REQ_BURST_SIZE (32)

/* Period between scans of the outstanding resolution requests from KNIs. */
#define CPS_SCAN_INTERVAL_SEC (5)

static struct cps_config cps_conf;

struct cps_config *
get_cps_conf(void)
{
	return &cps_conf;
}

static int
cleanup_cps(void)
{
	/*
	 * route_event_sock_close() can be called even when the netlink
	 * socket is not open, and rte_kni_release() can be passed NULL.
	 */
	route_event_sock_close(&cps_conf);
	rte_kni_release(cps_conf.back_kni);
	rte_kni_release(cps_conf.front_kni);
	rte_timer_stop(&cps_conf.scan_timer);
	destroy_mailbox(&cps_conf.mailbox);
	rm_kni();
	return 0;
}

/*
 * Responding to ARP and ND packets from the KNI. If responding to
 * an ARP/ND packet fails, we remove the request from the linked list
 * anyway, forcing the KNI to issue another resolution request.
 */

static void
send_arp_reply_kni(struct cps_config *cps_conf, struct cps_arp_req *arp)
{
	struct gatekeeper_if *iface = arp->iface;
	struct rte_mbuf *created_pkt;
	struct ether_hdr *eth_hdr;
	struct arp_hdr *arp_hdr;
	size_t pkt_size;
	struct rte_kni *kni;
	struct rte_mempool *mp;
	int ret;

	mp = cps_conf->net->gatekeeper_pktmbuf_pool[
		rte_lcore_to_socket_id(cps_conf->lcore_id)];
	created_pkt = rte_pktmbuf_alloc(mp);
	if (created_pkt == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: could not allocate an ARP reply on the %s KNI\n",
			iface->name);
		return;
	}

	pkt_size = sizeof(struct ether_hdr) + sizeof(struct arp_hdr);
	created_pkt->data_len = pkt_size;
	created_pkt->pkt_len = pkt_size;

	/*
	 * Set-up Ethernet header. The Ethernet address of the KNI is the
	 * same as that of the Gatekeeper interface, so we use that in
	 * the Ethernet and ARP headers.
	 */
	eth_hdr = rte_pktmbuf_mtod(created_pkt, struct ether_hdr *);
	ether_addr_copy(&arp->ha, &eth_hdr->s_addr);
	ether_addr_copy(&iface->eth_addr, &eth_hdr->d_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_ARP);

	/* Set-up ARP header. */
	arp_hdr = (struct arp_hdr *)&eth_hdr[1];
	arp_hdr->arp_hrd = rte_cpu_to_be_16(ARP_HRD_ETHER);
	arp_hdr->arp_pro = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
	arp_hdr->arp_hln = ETHER_ADDR_LEN;
	arp_hdr->arp_pln = sizeof(struct in_addr);
	arp_hdr->arp_op = rte_cpu_to_be_16(ARP_OP_REPLY);
	ether_addr_copy(&arp->ha, &arp_hdr->arp_data.arp_sha);
	rte_memcpy(&arp_hdr->arp_data.arp_sip, &arp->ip,
		sizeof(arp_hdr->arp_data.arp_sip));
	ether_addr_copy(&iface->eth_addr, &arp_hdr->arp_data.arp_tha);
	arp_hdr->arp_data.arp_tip = iface->ip4_addr.s_addr;

	if (iface == &cps_conf->net->front)
		kni = cps_conf->front_kni;
	else
		kni = cps_conf->back_kni;

	ret = rte_kni_tx_burst(kni, &created_pkt, 1);
	if (ret <= 0) {
		rte_pktmbuf_free(created_pkt);
		RTE_LOG(ERR, GATEKEEPER,
			"cps: could not transmit an ARP reply to the %s KNI\n",
			iface->name);
		return;
	}
}

static void
send_nd_reply_kni(struct cps_config *cps_conf, struct cps_nd_req *nd)
{
	struct gatekeeper_if *iface = nd->iface;
	struct rte_mbuf *created_pkt;
	struct ether_hdr *eth_hdr;
	struct ipv6_hdr *ipv6_hdr;
	struct icmpv6_hdr *icmpv6_hdr;
	struct nd_neigh_msg *nd_msg;
	struct nd_opt_lladdr *nd_opt;
	struct rte_kni *kni;
	struct rte_mempool *mp;
	int ret;

	mp = cps_conf->net->gatekeeper_pktmbuf_pool[
		rte_lcore_to_socket_id(cps_conf->lcore_id)];
	created_pkt = rte_pktmbuf_alloc(mp);
	if (created_pkt == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: could not allocate an ND advertisement on the %s KNI\n",
			iface->name);
		return;
	}

	/* Advertisement will include target link layer address. */
	created_pkt->data_len = ND_NEIGH_PKT_LLADDR_MIN_LEN;
	created_pkt->pkt_len = ND_NEIGH_PKT_LLADDR_MIN_LEN;

	/*
	 * Set-up Ethernet header. The Ethernet address of the KNI is the
	 * same as that of the Gatekeeper interface, so we use that in
	 * the Ethernet header.
	 */
	eth_hdr = rte_pktmbuf_mtod(created_pkt, struct ether_hdr *);
	ether_addr_copy(&nd->ha, &eth_hdr->s_addr);
	ether_addr_copy(&iface->eth_addr, &eth_hdr->d_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv6);

	/* Set-up IPv6 header. */
	ipv6_hdr = (struct ipv6_hdr *)&eth_hdr[1];
	ipv6_hdr->vtc_flow = rte_cpu_to_be_32(IPv6_DEFAULT_VTC_FLOW);
	ipv6_hdr->payload_len = rte_cpu_to_be_16(ND_NEIGH_PKT_LLADDR_MIN_LEN -
		(sizeof(*eth_hdr) + sizeof(*ipv6_hdr)));
	ipv6_hdr->proto = IPPROTO_ICMPV6;
	ipv6_hdr->hop_limits = IPv6_DEFAULT_HOP_LIMITS;
	rte_memcpy(ipv6_hdr->src_addr, nd->ip, sizeof(ipv6_hdr->dst_addr));
	rte_memcpy(ipv6_hdr->dst_addr, iface->ll_ip6_addr.s6_addr,
		sizeof(ipv6_hdr->dst_addr));

	/* Set-up ICMPv6 header. */
	icmpv6_hdr = (struct icmpv6_hdr *)&ipv6_hdr[1];
	icmpv6_hdr->type = ND_NEIGHBOR_ADVERTISEMENT;
	icmpv6_hdr->code = 0;
	icmpv6_hdr->cksum = 0; /* Calculated below. */

	/* Set up ND Advertisement header with target LL addr option. */
	nd_msg = (struct nd_neigh_msg *)&icmpv6_hdr[1];
	nd_msg->flags =
		rte_cpu_to_be_32(LLS_ND_NA_OVERRIDE|LLS_ND_NA_SOLICITED);
	rte_memcpy(nd_msg->target, nd->ip, sizeof(nd_msg->target));
	nd_opt = (struct nd_opt_lladdr *)&nd_msg[1];
	nd_opt->type = ND_OPT_TARGET_LL_ADDR;
	nd_opt->len = 1;
	ether_addr_copy(&nd->ha, &nd_opt->ha);

	icmpv6_hdr->cksum = rte_ipv6_icmpv6_cksum(ipv6_hdr, icmpv6_hdr);

	if (iface == &cps_conf->net->front)
		kni = cps_conf->front_kni;
	else
		kni = cps_conf->back_kni;

	ret = rte_kni_tx_burst(kni, &created_pkt, 1);
	if (ret <= 0) {
		rte_pktmbuf_free(created_pkt);
		RTE_LOG(ERR, GATEKEEPER,
			"cps: could not transmit an ND advertisement to the %s KNI\n",
			iface->name);
		return;
	}
}

static void
process_reqs(struct cps_config *cps_conf)
{
	struct cps_request *reqs[CPS_REQ_BURST_SIZE];
	unsigned int count = mb_dequeue_burst(&cps_conf->mailbox,
		(void **)reqs, CPS_REQ_BURST_SIZE);
	unsigned int i;

	for (i = 0; i < count; i++) {
		switch (reqs[i]->ty) {
		case CPS_REQ_BGP: {
			struct cps_bgp_req *bgp = &reqs[i]->u.bgp;
			unsigned int num_tx = rte_kni_tx_burst(bgp->kni,
				bgp->pkts, bgp->num_pkts);
			if (unlikely(num_tx < bgp->num_pkts)) {
				uint16_t j;
				for (j = num_tx; j < bgp->num_pkts; j++)
					rte_pktmbuf_free(bgp->pkts[j]);
			}
			break;
		}
		case CPS_REQ_ARP: {
			struct cps_arp_req *arp = &reqs[i]->u.arp;
			struct arp_request *entry, *next;

			send_arp_reply_kni(cps_conf, arp);

			list_for_each_entry_safe(entry, next,
					&cps_conf->arp_requests, list) {
				if (arp->ip == entry->addr) {
					list_del(&entry->list);
					rte_free(entry);
					break;
				}
			}
			break;
		}
		case CPS_REQ_ND: {
			struct cps_nd_req *nd = &reqs[i]->u.nd;
			struct nd_request *entry, *next;

			send_nd_reply_kni(cps_conf, nd);

			list_for_each_entry_safe(entry, next,
					&cps_conf->nd_requests, list) {
				if (ipv6_addrs_equal(nd->ip, entry->addr)) {
					list_del(&entry->list);
					rte_free(entry);
					break;
				}
			}
			break;
		}
		default:
			RTE_LOG(ERR, GATEKEEPER,
				"cps: unrecognized request type (%d)\n",
				reqs[i]->ty);
			break;
		}
		mb_free_entry(&cps_conf->mailbox, reqs[i]);
	}
}

static void
process_ingress(struct gatekeeper_if *iface, struct rte_kni *kni,
	uint16_t rx_queue)
{
	uint16_t gatekeeper_max_pkt_burst =
		get_gatekeeper_conf()->gatekeeper_max_pkt_burst;
	struct rte_mbuf *bufs[gatekeeper_max_pkt_burst];
	uint16_t num_rx = rte_eth_rx_burst(iface->id, rx_queue, bufs,
		gatekeeper_max_pkt_burst);
	unsigned int num_tx = rte_kni_tx_burst(kni, bufs, num_rx);

	if (unlikely(num_tx < num_rx)) {
		uint16_t i;
		for (i = num_tx; i < num_rx; i++)
			rte_pktmbuf_free(bufs[i]);
	}

	/*
	 * Userspace requests to change the device MTU or configure the
	 * device up/down are forwarded from the kernel back to userspace
	 * for DPDK to handle. rte_kni_handle_request() receives those
	 * requests and allows them to be processed.
	 */
	if (rte_kni_handle_request(kni) < 0)
		RTE_LOG(WARNING, KNI,
			"%s: error in handling userspace request on KNI %s\n",
			__func__, rte_kni_get_name(kni));
}

static int
pkt_is_nd(struct gatekeeper_if *iface, struct ether_hdr *eth_hdr,
	uint16_t pkt_len)
{
	struct ipv6_hdr *ipv6_hdr;
	struct icmpv6_hdr *icmpv6_hdr;

	if (pkt_len < (sizeof(*eth_hdr) + sizeof(*ipv6_hdr) +
			sizeof(icmpv6_hdr)))
		return false;

	ipv6_hdr = (struct ipv6_hdr *)&eth_hdr[1];
	if (ipv6_hdr->proto != IPPROTO_ICMPV6)
		return false;

	/*
	 * Make sure this is an ND neighbor message and that it was
	 * sent by us (our global address, link-local address, or
	 * either of the solicited-node multicast addresses.
	 */
	icmpv6_hdr = (struct icmpv6_hdr *)&ipv6_hdr[1];
	return (icmpv6_hdr->type == ND_NEIGHBOR_SOLICITATION ||
			icmpv6_hdr->type == ND_NEIGHBOR_ADVERTISEMENT) &&
		(ipv6_addrs_equal(ipv6_hdr->src_addr,
			iface->ll_ip6_addr.s6_addr) ||
		ipv6_addrs_equal(ipv6_hdr->src_addr,
			iface->ip6_addr.s6_addr) ||
		ipv6_addrs_equal(ipv6_hdr->src_addr,
			iface->ip6_mc_addr.s6_addr) ||
		ipv6_addrs_equal(ipv6_hdr->src_addr,
			iface->ll_ip6_mc_addr.s6_addr));
}

static void
process_egress(struct cps_config *cps_conf, struct gatekeeper_if *iface,
	struct rte_kni *kni, uint16_t tx_queue)
{
	uint16_t gatekeeper_max_pkt_burst =
		get_gatekeeper_conf()->gatekeeper_max_pkt_burst;
	struct rte_mbuf *bufs[gatekeeper_max_pkt_burst];
	struct rte_mbuf *forward_bufs[gatekeeper_max_pkt_burst];
	uint16_t num_rx = rte_kni_rx_burst(
		kni, bufs, gatekeeper_max_pkt_burst);
	uint16_t num_forward = 0;
	unsigned int num_tx;
	unsigned int i;

	if (num_rx == 0)
		return;

	for (i = 0; i < num_rx; i++) {
		struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(bufs[i],
			struct ether_hdr *);
		switch (rte_be_to_cpu_16(eth_hdr->ether_type)) {
		case ETHER_TYPE_ARP:
			/* Intercept ARP packet and handle it. */
			kni_process_arp(cps_conf, iface, bufs[i], eth_hdr);
			break;
		case ETHER_TYPE_IPv6: {
			uint16_t pkt_len = rte_pktmbuf_data_len(bufs[i]);
			if (pkt_is_nd(iface, eth_hdr, pkt_len)) {
				/* Intercept ND packet and handle it. */
				kni_process_nd(cps_conf, iface,
					bufs[i], eth_hdr, pkt_len);
				break;
			}
		}
			/* FALLTHROUGH */
		default:
			/* Forward all other packets to the interface. */
			forward_bufs[num_forward++] = bufs[i];
			break;
		}
	}

	num_tx = rte_eth_tx_burst(iface->id, tx_queue,
		forward_bufs, num_forward);
	if (unlikely(num_tx < num_forward)) {
		uint16_t i;
		for (i = num_tx; i < num_forward; i++)
			rte_pktmbuf_free(forward_bufs[i]);
	}
}

static int
cps_proc(void *arg)
{
	struct cps_config *cps_conf = (struct cps_config *)arg;
	struct net_config *net_conf = cps_conf->net;

	struct gatekeeper_if *front_iface = &net_conf->front;
	struct gatekeeper_if *back_iface = &net_conf->back;
	struct rte_kni *front_kni = cps_conf->front_kni;
	struct rte_kni *back_kni = cps_conf->back_kni;

	RTE_LOG(NOTICE, GATEKEEPER,
		"cps: the CPS block is running at lcore = %u\n",
		cps_conf->lcore_id);

	while (likely(!exiting)) {
		/*
		 * Read in IPv4 BGP packets that arrive directly
		 * on the Gatekeeper interfaces.
		 */
		process_ingress(front_iface, front_kni,
			cps_conf->rx_queue_front);
		if (net_conf->back_iface_enabled)
			process_ingress(back_iface, back_kni,
				cps_conf->rx_queue_back);

		/*
		 * Process any requests made to the CPS block, including
		 * IPv6 BGP packets that arrived via an ACL.
		 */
		process_reqs(cps_conf);

		/*
		 * Read in packets from KNI interfaces, and
		 * transmit to respective Gatekeeper interfaces.
		 */
		process_egress(cps_conf, front_iface, front_kni,
			cps_conf->tx_queue_front);
		if (net_conf->back_iface_enabled)
			process_egress(cps_conf, back_iface, back_kni,
				cps_conf->tx_queue_back);

		/* Periodically scan resolution requests from KNIs. */
		rte_timer_manage();

		/* Read in routing table updates and update LPM table. */
		kni_cps_route_event(cps_conf);
	}

	RTE_LOG(NOTICE, GATEKEEPER,
		"cps: the CPS block at lcore = %u is exiting\n",
		cps_conf->lcore_id);

	return cleanup_cps();
}

static int
submit_bgp(struct rte_mbuf **pkts, unsigned int num_pkts,
	struct gatekeeper_if *iface)
{
	struct cps_config *cps_conf = get_cps_conf();
	struct cps_request *req = mb_alloc_entry(&cps_conf->mailbox);
	int ret;
	unsigned int i;
	uint16_t gatekeeper_max_pkt_burst =
		get_gatekeeper_conf()->gatekeeper_max_pkt_burst;

	RTE_VERIFY(num_pkts <= gatekeeper_max_pkt_burst);

	if (req == NULL) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: %s: allocation of mailbox message failed\n",
			__func__);
		ret = -ENOMEM;
		goto free_pkts;
	}

	req->ty = CPS_REQ_BGP;
	req->u.bgp.num_pkts = num_pkts;
	req->u.bgp.kni = iface == &cps_conf->net->front
		? cps_conf->front_kni
		: cps_conf->back_kni;
	req->u.bgp.pkts = pkts;

	ret = mb_send_entry(&cps_conf->mailbox, req);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: %s: failed to enqueue message to mailbox\n",
			__func__);
		goto free_pkts;
	}

	return 0;

free_pkts:
	for (i = 0; i < num_pkts; i++)
		rte_pktmbuf_free(pkts[i]);
	return ret;
}

static int
assign_cps_queue_ids(struct cps_config *cps_conf)
{
	int ret = get_queue_id(&cps_conf->net->front, QUEUE_TYPE_RX,
		cps_conf->lcore_id);
	if (ret < 0)
		goto fail;
	cps_conf->rx_queue_front = ret;

	ret = get_queue_id(&cps_conf->net->front, QUEUE_TYPE_TX,
		cps_conf->lcore_id);
	if (ret < 0)
		goto fail;
	cps_conf->tx_queue_front = ret;

	if (cps_conf->net->back_iface_enabled) {
		ret = get_queue_id(&cps_conf->net->back, QUEUE_TYPE_RX,
			cps_conf->lcore_id);
		if (ret < 0)
			goto fail;
		cps_conf->rx_queue_back = ret;

		ret = get_queue_id(&cps_conf->net->back, QUEUE_TYPE_TX,
			cps_conf->lcore_id);
		if (ret < 0)
			goto fail;
		cps_conf->tx_queue_back = ret;
	}

	return 0;

fail:
	RTE_LOG(ERR, GATEKEEPER, "cps: cannot assign queues\n");
	return ret;
}

/*
 * We create the KNIs in stage 1 because creating a KNI seems to
 * restart the PCI device on which the KNI is based, which removes
 * some (but not all) device-specific configuration that has already
 * happened (RETA, multicast Ethernet addresses, etc). Therefore, if
 * we put the KNI creation in stage 2 (after the devices are started),
 * we will have to re-do some of the configuration.
 *
 * Following the documentation strictly, the call to
 * rte_eth_dev_info_get() here should take place *after* the NIC is
 * started. However, this rule is widely broken throughout DPDK, and
 * breaking it here makes configuration much easier due to this
 * problem of restarting the devices. 
 */
static int
kni_create(struct rte_kni **kni, struct rte_mempool *mp,
	struct gatekeeper_if *iface)
{
	struct rte_kni_conf conf;
	struct rte_eth_dev_info dev_info;
	struct rte_kni_ops ops;
	int ret;

	memset(&conf, 0, sizeof(conf));
	ret = snprintf(conf.name, RTE_KNI_NAMESIZE, "kni_%s", iface->name);
	RTE_VERIFY(ret > 0 && ret < RTE_KNI_NAMESIZE);
	conf.mbuf_size = rte_pktmbuf_data_room_size(mp);

	/* If the interface is bonded, take PCI info from the primary slave. */
	if (iface->num_ports > 1 || iface->bonding_mode == BONDING_MODE_8023AD)
		conf.group_id = rte_eth_bond_primary_get(iface->id);
	else
		conf.group_id = iface->id;
	rte_eth_dev_info_get(conf.group_id, &dev_info);
	conf.addr = dev_info.pci_dev->addr;
	conf.id = dev_info.pci_dev->id;

	memset(&ops, 0, sizeof(ops));
	ops.port_id = conf.group_id;
	ops.change_mtu = kni_change_mtu;
	ops.config_network_if = kni_change_if;

	*kni = rte_kni_alloc(mp, &conf, &ops);
	if (*kni == NULL) {
		RTE_LOG(ERR, KNI, "Could not allocate KNI for %s iface\n",
			iface->name);
		return -1;
	}

	return 0;
}

static void
cps_scan(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
	struct cps_config *cps_conf = (struct cps_config *)arg;
	if (arp_enabled(cps_conf->lls)) {
		struct arp_request *entry, *next;
		list_for_each_entry_safe(entry, next, &cps_conf->arp_requests,
				list) {
			if (entry->stale) {
				/*
				 * It's possible that if this request
				 * was recently satisfied the callback
				 * has already been disabled, but it's
				 * safe to issue an extra put_arp() here.
				 */
				put_arp((struct in_addr *)&entry->addr,
					cps_conf->lcore_id);
				list_del(&entry->list);
				rte_free(entry);
			} else
				entry->stale = true;
		}
	}
	if (nd_enabled(cps_conf->lls)) {
		struct nd_request *entry, *next;
		list_for_each_entry_safe(entry, next, &cps_conf->nd_requests,
				list) {
			if (entry->stale) {
				/* Same as above -- this may be unnecessary. */
				put_nd((struct in6_addr *)entry->addr,
					cps_conf->lcore_id);
				list_del(&entry->list);
				rte_free(entry);
			} else
				entry->stale = true;
		}
	}
}

static int
cps_stage1(void *arg)
{
	struct cps_config *cps_conf = arg;
	unsigned int socket_id = rte_lcore_to_socket_id(cps_conf->lcore_id);
	int ret;

	ret = assign_cps_queue_ids(cps_conf);
	if (ret < 0)
		goto error;

	ret = kni_create(&cps_conf->front_kni,
		cps_conf->net->gatekeeper_pktmbuf_pool[socket_id],
		&cps_conf->net->front);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: failed to create KNI for the front iface\n");
		goto error;
	}

	if (cps_conf->net->back_iface_enabled) {
		ret = kni_create(&cps_conf->back_kni,
			cps_conf->net->gatekeeper_pktmbuf_pool[socket_id],
			&cps_conf->net->back);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: failed to create KNI for the back iface\n");
			goto error;
		}
	}

	return 0;

error:
	cleanup_cps();
	return ret;
}

static void
fill_bgp_rule(struct ipv6_acl_rule *rule, struct gatekeeper_if *iface,
	int filter_source_port, uint16_t tcp_port_bgp)
{
	uint32_t *ptr32 = (uint32_t *)&iface->ip6_addr.s6_addr;
	int i;

	rule->data.category_mask = 0x1;
	rule->data.priority = 1;
	/* Userdata is filled in in register_ipv6_acl(). */

	rule->field[PROTO_FIELD_IPV6].value.u8 = IPPROTO_TCP;
	rule->field[PROTO_FIELD_IPV6].mask_range.u8 = 0xFF;

	for (i = DST1_FIELD_IPV6; i <= DST4_FIELD_IPV6; i++) {
		rule->field[i].value.u32 = rte_be_to_cpu_32(*ptr32);
		rule->field[i].mask_range.u32 = 32;
		ptr32++;
	}

	if (filter_source_port) {
		rule->field[SRCP_FIELD_IPV6].value.u16 = tcp_port_bgp;
		rule->field[SRCP_FIELD_IPV6].mask_range.u16 = 0xFFFF;
	} else {
		rule->field[DSTP_FIELD_IPV6].value.u16 = tcp_port_bgp;
		rule->field[DSTP_FIELD_IPV6].mask_range.u16 = 0xFFFF;
	}
}

/*
 * Match the packet if it fails to be classifed by ACL rules.
 * If it's a bgp packet, then submit it to the LLS block.
 *
 * Return values: 0 for successful match, and -ENOENT for no matching.
 */
static int
match_bgp(struct rte_mbuf *pkt, struct gatekeeper_if *iface)
{
	/*
	 * The TCP header offset in terms of the
	 * beginning of the IPv6 header.
	 */
	int tcp_offset;
	uint8_t nexthdr;
	const uint16_t BE_ETHER_TYPE_IPv6 = rte_cpu_to_be_16(ETHER_TYPE_IPv6);
	struct ether_hdr *eth_hdr =
		rte_pktmbuf_mtod(pkt, struct ether_hdr *);
	struct ipv6_hdr *ip6hdr;
	struct tcp_hdr *tcp_hdr;
	uint16_t minimum_size = sizeof(*eth_hdr) +
		sizeof(struct ipv6_hdr) + sizeof(struct tcp_hdr);
	uint16_t cps_bgp_port = rte_cpu_to_be_16(get_cps_conf()->tcp_port_bgp);

	if (unlikely(eth_hdr->ether_type != BE_ETHER_TYPE_IPv6))
		return -ENOENT;

	if (pkt->data_len < minimum_size) {
		RTE_LOG(NOTICE, GATEKEEPER, "cps: BGP packet received is %"PRIx16" bytes but should be at least %hu bytes\n",
			pkt->data_len, minimum_size);
		return -ENOENT;
	}

 	ip6hdr = (struct ipv6_hdr *)&eth_hdr[1];

	if ((memcmp(ip6hdr->dst_addr, &iface->ip6_addr,
			sizeof(iface->ip6_addr)) != 0))
		return -ENOENT;

	tcp_offset = ipv6_skip_exthdr(ip6hdr, pkt->data_len -
		sizeof(*eth_hdr), &nexthdr);
	if (tcp_offset < 0 || nexthdr != IPPROTO_TCP)
		return -ENOENT;

	minimum_size += tcp_offset - sizeof(*ip6hdr);
	if (pkt->data_len < minimum_size) {
		RTE_LOG(NOTICE, GATEKEEPER, "cps: BGP packet received is %"PRIx16" bytes but should be at least %hu bytes\n",
			pkt->data_len, minimum_size);
		return -ENOENT;
	}

	tcp_hdr = (struct tcp_hdr *)((uint8_t *)ip6hdr + tcp_offset);
	if (tcp_hdr->src_port != cps_bgp_port &&
			tcp_hdr->dst_port != cps_bgp_port)
		return -ENOENT;

	return 0;
}

static int
add_bgp_filters(struct gatekeeper_if *iface, uint16_t tcp_port_bgp,
	uint16_t rx_queue)
{
	if (ipv4_if_configured(iface)) {
		int ret;
		/* Capture pkts for connections started by our BGP speaker. */
		ret = ntuple_filter_add(iface->id, iface->ip4_addr.s_addr,
			rte_cpu_to_be_16(tcp_port_bgp), UINT16_MAX, 0, 0,
			IPPROTO_TCP, rx_queue, true);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: could not add source BGP filter on %s iface\n",
				iface->name);
			return ret;
		}
		/* Capture pkts for connections remote BGP speakers started. */
		ret = ntuple_filter_add(iface->id, iface->ip4_addr.s_addr,
			0, 0, rte_cpu_to_be_16(tcp_port_bgp), UINT16_MAX,
			IPPROTO_TCP, rx_queue, true);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: could not add destination BGP filter on %s iface\n",
				iface->name);
			return ret;
		}
	}

	if (ipv6_if_configured(iface)) {
		struct ipv6_acl_rule ipv6_rules[NUM_ACL_BGP_RULES];
		int ret;

		memset(&ipv6_rules, 0, sizeof(ipv6_rules));

		/* Capture pkts for connections started by our BGP speaker. */
		fill_bgp_rule(&ipv6_rules[0], iface, true, tcp_port_bgp);
		/* Capture pkts for connections remote BGP speakers started. */
		fill_bgp_rule(&ipv6_rules[1], iface, false, tcp_port_bgp);

		ret = register_ipv6_acl(ipv6_rules, NUM_ACL_BGP_RULES,
			submit_bgp, match_bgp, iface);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: could not register BGP IPv6 ACL on %s iface\n",
				iface->name);
			return ret;
		}
	}

	return 0;
}

static int
cps_stage2(void *arg)
{
	struct cps_config *cps_conf = arg;
	int ret;

	ret = add_bgp_filters(&cps_conf->net->front,
		cps_conf->tcp_port_bgp, cps_conf->rx_queue_front);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: failed to add BGP filters on the front iface");
		goto error;
	}

	ret = kni_config(cps_conf->front_kni, &cps_conf->net->front);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: failed to configure KNI on the front iface\n");
		goto error;
	}

	if (cps_conf->net->back_iface_enabled) {
		ret = add_bgp_filters(&cps_conf->net->back,
			cps_conf->tcp_port_bgp, cps_conf->rx_queue_back);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: failed to add BGP filters on the back iface");
			goto error;
		}

		ret = kni_config(cps_conf->back_kni, &cps_conf->net->back);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"cps: failed to configure KNI on the back iface\n");
			goto error;
		}
	}

	ret = route_event_sock_open(cps_conf);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER,
			"cps: failed to open route event socket\n");
		goto error;
	}

	return 0;

error:
	cleanup_cps();
	return ret;
}

int
run_cps(struct net_config *net_conf, struct cps_config *cps_conf,
	struct lls_config *lls_conf, const char *kni_kmod_path)
{
	int ret;

	if (net_conf == NULL || cps_conf == NULL || lls_conf == NULL) {
		ret = -1;
		goto out;
	}

	ret = net_launch_at_stage1(net_conf, 1, 1, 1, 1, cps_stage1, cps_conf);
	if (ret < 0)
		goto out;

	ret = launch_at_stage2(cps_stage2, cps_conf);
	if (ret < 0)
		goto stage1;

	ret = launch_at_stage3("cps", cps_proc, cps_conf, cps_conf->lcore_id);
	if (ret < 0)
		goto stage2;

	cps_conf->net = net_conf;
	cps_conf->lls = lls_conf;

	ret = init_kni(kni_kmod_path, net_conf->back_iface_enabled ? 2 : 1);
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER, "cps: couldn't initialize KNI\n");
		goto stage3;
	}

	ret = init_mailbox("cps_mb", MAILBOX_MAX_ENTRIES,
		sizeof(struct cps_request), cps_conf->lcore_id,
		&cps_conf->mailbox);
	if (ret < 0)
		goto kni;

	if (arp_enabled(cps_conf->lls))
		INIT_LIST_HEAD(&cps_conf->arp_requests);
	if (nd_enabled(cps_conf->lls))
		INIT_LIST_HEAD(&cps_conf->nd_requests);

	rte_timer_init(&cps_conf->scan_timer);
	ret = rte_timer_reset(&cps_conf->scan_timer,
		CPS_SCAN_INTERVAL_SEC * rte_get_timer_hz(), PERIODICAL,
		cps_conf->lcore_id, cps_scan, cps_conf);
	if (ret < 0) {
		RTE_LOG(ERR, TIMER, "Cannot set CPS scan timer\n");
		goto mailbox;
	}

	return 0;
mailbox:
	destroy_mailbox(&cps_conf->mailbox);
kni:
	rm_kni();
stage3:
	pop_n_at_stage3(1);
stage2:
	pop_n_at_stage2(1);
stage1:
	pop_n_at_stage1(1);
out:
	return ret;
}

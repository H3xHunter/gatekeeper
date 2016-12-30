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

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_byteorder.h>

#include "gatekeeper_ipip.h"

int
encapsulate(struct rte_mbuf *pkt, uint8_t priority,
	struct ipip_tunnel_info *info)
{
	struct ether_hdr *new_eth;
	struct ipv4_hdr *outer_ip4hdr;
	struct ipv6_hdr *outer_ip6hdr;

	if (info->flow.proto == ETHER_TYPE_IPv4) {
		/* Allocate space for outer IPv4 header. */
		new_eth = (struct ether_hdr *)rte_pktmbuf_prepend(pkt,
			sizeof(struct ipv4_hdr));
		if (new_eth == NULL) {
			RTE_LOG(ERR, MBUF,
				"Not enough headroom space in the first segment!\n");
			return -1;
		}

		outer_ip4hdr = (struct ipv4_hdr *)&new_eth[1];

		/* Fill up the new Ethernet header. */
		rte_memcpy(&new_eth->s_addr, &info->source_mac,
			sizeof(new_eth->s_addr));
		/* Fill up the destination MAC address via Gateway MAC. */
		rte_memcpy(&new_eth->d_addr, &info->nexthop_mac,
			sizeof(new_eth->d_addr));

		new_eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

		/* Fill up the outer IP header. */
		outer_ip4hdr->version_ihl = IP_VHL_DEF;
		outer_ip4hdr->type_of_service = (priority << 2);
		outer_ip4hdr->packet_id = 0;
		outer_ip4hdr->fragment_offset = IP_DN_FRAGMENT_FLAG;
		outer_ip4hdr->time_to_live = IP_DEFTTL;
		outer_ip4hdr->next_proto_id = IPPROTO_IPIP;
		/* The source address is the Gatekeeper server IP address. */
		outer_ip4hdr->src_addr = info->flow.f.v4.src;
		/* The destination address is the Grantor server IP address. */
		outer_ip4hdr->dst_addr = info->flow.f.v4.dst;

		outer_ip4hdr->total_length = rte_cpu_to_be_16(pkt->data_len
			- sizeof(struct ether_hdr));

		/*
		 * The IP header checksum filed must be set to 0
		 * in order to offload the checksum calculation.
		 */
		outer_ip4hdr->hdr_checksum = 0;

		pkt->outer_l2_len = sizeof(struct ether_hdr);
		pkt->outer_l3_len = sizeof(struct ipv4_hdr);
		/* Offload checksum computation for the outer IPv4 header. */
		pkt->ol_flags |= (PKT_TX_IPV4 |
			PKT_TX_IP_CKSUM | PKT_TX_OUTER_IPV4);
	} else if (info->flow.proto == ETHER_TYPE_IPv6) {
		/* Allocate space for new IPv6 header. */
		new_eth = (struct ether_hdr *)rte_pktmbuf_prepend(pkt,
			sizeof(struct ipv6_hdr));
		if (new_eth == NULL) {
			RTE_LOG(ERR, MBUF,
				"Not enough headroom space in the first segment!\n");
			return -1;
		}

		outer_ip6hdr = (struct ipv6_hdr *)&new_eth[1];

		/* Fill up the new Ethernet header. */
		rte_memcpy(&new_eth->s_addr, &info->source_mac,
			sizeof(new_eth->s_addr));
		/* Fill up the destination MAC address via Gateway MAC. */
		rte_memcpy(&new_eth->d_addr, &info->nexthop_mac,
			sizeof(new_eth->d_addr));

		new_eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv6);

		/* Fill up the outer IP header. */
		outer_ip6hdr->vtc_flow = rte_cpu_to_be_32(
			IPv6_DEFAULT_VTC_FLOW | (priority << 22));
		outer_ip6hdr->proto = IPPROTO_IPIP; 
		outer_ip6hdr->hop_limits = IPv6_DEFAULT_HOP_LIMITS;

		rte_memcpy(outer_ip6hdr->src_addr, info->flow.f.v6.src,
			sizeof(info->flow.f.v6.src));
		rte_memcpy(outer_ip6hdr->dst_addr, info->flow.f.v6.dst,
			sizeof(info->flow.f.v6.dst));

		outer_ip6hdr->payload_len = rte_cpu_to_be_16(pkt->data_len
			- sizeof(struct ether_hdr) - sizeof(struct ipv6_hdr));

		pkt->outer_l2_len = sizeof(struct ether_hdr);
		pkt->outer_l3_len = sizeof(struct ipv6_hdr);
	} else 
		return -1;

	return 0;
}

int
decapsulate(struct rte_mbuf *pkt, uint8_t *priority,
	struct ipip_tunnel_info *info)
{
	uint16_t ethertype;
	uint8_t l4_proto;
	uint16_t outer_header_len = 0;
	struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
	struct ipv4_hdr *ipv4_hdr;
	struct ipv6_hdr *ipv6_hdr;

	outer_header_len += sizeof(struct ether_hdr);
	ethertype = rte_be_to_cpu_16(eth_hdr->ether_type);

	switch (ethertype) {
	case ETHER_TYPE_IPv4:
		ipv4_hdr = (struct ipv4_hdr *)
			((char *)eth_hdr + outer_header_len);
		outer_header_len += sizeof(struct ipv4_hdr);
		*priority = (ipv4_hdr->type_of_service >> 2);
		l4_proto = ipv4_hdr->next_proto_id;
		if (*priority >= 2) {
			info->flow.proto = ETHER_TYPE_IPv4;
			info->flow.f.v4.src = ipv4_hdr->src_addr;
			info->flow.f.v4.dst = ipv4_hdr->dst_addr;
		}
		break;
	case ETHER_TYPE_IPv6:
		ipv6_hdr = (struct ipv6_hdr *)
			((char *)eth_hdr + outer_header_len);
		*priority = (((ipv6_hdr->vtc_flow >> 20) & 0xFF) >> 2);
		outer_header_len += sizeof(struct ipv6_hdr);
		l4_proto = ipv6_hdr->proto;
		if (*priority >= 2) {
			info->flow.proto = ETHER_TYPE_IPv6;
			rte_memcpy(info->flow.f.v6.src,
				ipv6_hdr->src_addr,
				sizeof(info->flow.f.v6.src));
			rte_memcpy(info->flow.f.v6.dst,
				ipv6_hdr->dst_addr,
				sizeof(info->flow.f.v6.dst));
		}
		break;
	default:
		l4_proto = 0;
		*priority = 0;
		outer_header_len += sizeof(struct ipv6_hdr);
		info->flow.proto = ethertype;
		break;
	}

	if (l4_proto != IPPROTO_IPIP)
		return -1;

	if (*priority >= 2) {
		ether_addr_copy(&eth_hdr->s_addr, &info->source_mac);
		ether_addr_copy(&eth_hdr->d_addr, &info->nexthop_mac);
	}

	rte_pktmbuf_adj(pkt, outer_header_len);

	return 0;
}

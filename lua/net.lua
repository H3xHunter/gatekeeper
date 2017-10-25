require "gatekeeper"
return function (gatekeeper_server)

	--
	-- Change these parameters to configure the network.
	--

	local num_attempts_link_get = 5
	local ipv6_default_hop_limits = 255

	local front_ports = {"enp133s0f0"}
	-- Each interface should have at most two ip addresses:
	-- 1 IPv4, 1 IPv6.
	local front_ips  = {"10.0.0.1/24", "2001:db8::1/32"}
	local front_arp_cache_timeout_sec = 7200
	local front_nd_cache_timeout_sec = 7200
	local front_bonding_mode = gatekeeper.c.BONDING_MODE_ROUND_ROBIN

	local back_iface_enabled = gatekeeper_server
	local back_ports = {"enp133s0f1"}
	local back_ips  = {"10.0.1.1/24", "2002:db8::1/32"}
	local back_arp_cache_timeout_sec = 7200
	local back_nd_cache_timeout_sec = 7200
	local back_bonding_mode = gatekeeper.c.BONDING_MODE_ROUND_ROBIN

	-- XXX Sample parameters, need to be tested for better performance.
	local mailbox_max_entries = 128
	local mailbox_mem_cache_size = 64
	local mailbox_burst_size = 32

	--
	-- Code below this point should not need to be changed.
	--

	local net_conf = gatekeeper.c.get_net_conf()
	net_conf.mailbox_max_entries = mailbox_max_entries
	net_conf.mailbox_mem_cache_size = mailbox_mem_cache_size
	net_conf.mailbox_burst_size = mailbox_burst_size
	net_conf.num_attempts_link_get = num_attempts_link_get
	net_conf.ipv6_default_hop_limits = ipv6_default_hop_limits

	local front_iface = gatekeeper.c.get_if_front(net_conf)
	front_iface.arp_cache_timeout_sec = front_arp_cache_timeout_sec
	front_iface.nd_cache_timeout_sec = front_nd_cache_timeout_sec
	front_iface.bonding_mode = front_bonding_mode
	local ret = gatekeeper.init_iface(front_iface, "front",
		front_ports, front_ips)

	net_conf.back_iface_enabled = back_iface_enabled
	if back_iface_enabled then
		local back_iface = gatekeeper.c.get_if_back(net_conf)
		back_iface.arp_cache_timeout_sec = back_arp_cache_timeout_sec
		back_iface.nd_cache_timeout_sec = back_nd_cache_timeout_sec
		back_iface.bonding_mode = back_bonding_mode
		ret = gatekeeper.init_iface(back_iface, "back",
			back_ports, back_ips)
	end

	-- Initialize the network.
	ret = gatekeeper.c.gatekeeper_init_network(net_conf)
	if ret < 0 then
		error("Failed to initilize the network")
	end

	return net_conf
end

local ffi = require("ffi")

local M = {}

--[[
This file defines simple policies with LPM for Grantor.

General format of the simple policies should be:
	IPv4 tables: lpm table
	IPv6 tables: lpm table

Here, I assume that each group has specific capability parameters,
including speed limit, expiration time, actions - DENY or ACCEPT, etc.
--]]

function M.setup_policy(socket, lcore)

	local IPV4 = policylib.c.IPV4

	-- Write simple policies below.

	-- Protocol numbers.
	local TCP = policylib.c.TCP
	local UDP = policylib.c.UDP

	-- Flow directions.
	local PKT_IN = 0
	local PKT_OUT = 1

	local default = {
		["params"] = {
        		["expire_sec"] = 120,
        		["action"] = policylib.c.GK_DECLINED,
    		},
	}

	local group1 = {
		[TCP] = {
        		[PKT_IN] = {
            			[22] = true,
            			[80] = true,
        		},
    		},

    		[UDP] = {
        		[PKT_IN] = {
            			[1194] = true,
            			[694] = true,
        		},
    		},

    		["params"] = {
        		["tx_rate_kb_sec"] = 10,
        		["cap_expire_sec"] = 20,
			["next_renewal_ms"] = 30,
			["renewal_step_ms"] = 40,
        		["action"] = policylib.c.GK_GRANTED,
    		},
	}

	local groups = {
		[1] = group1,
		[255] = default,
	}

	local lpm_policies = {
		[IPV4] = {
			{
				{
					["ip_addr"] = "10.0.0.1",
					["prefix_len"] = 32,
    					["policy_id"] = 1,
				},
			},
		},
	}

	local policies = {
		[IPV4] = {},
	}

	local lpm_conf =
		ffi.new("struct rte_lpm_config")
	lpm_conf.max_rules = 1024
	lpm_conf.number_tbl8s = 256

	local num_tbl = 0;

	-- TODO Add IPv6 policies.
	for key, value in ipairs(lpm_policies[IPV4]) do
		local ret = -1
		local table = {}
		table["lpm"] = policylib.c.init_ipv4_lpm(
			"gt", lpm_conf, socket, lcore, num_tbl)
		if table["lpm"] == nil then return nil end

		local routes =
			ffi.new("struct lua_ip_routes [" .. #value.. "]")
		for i, v in ipairs(value) do
			routes[i - 1].ip_addr = v["ip_addr"]
			routes[i - 1].prefix_len = v["prefix_len"]
			routes[i - 1].policy_id = v["policy_id"]
		end

		ret = policylib.c.lua_update_ipv4_lpm(
			table["lpm"], routes, #value)
		if ret < 0 then return nil end

		table["groups"] = groups
		policies[IPV4][#policies[IPV4] + 1] = table
		num_tbl = num_tbl + 1
	end

	return policies
end

function M.lookup_policy(policies, fields)

	local IPV4 = policylib.c.IPV4
	local IPV6 = policylib.c.IPV6
	local next_hop = -1
	local pf = ffi.cast("struct gt_packet_fields *", fields)

	for i, v in ipairs(policies[pf.inner_ip_ver]) do
		if pf.inner_ip_ver == IPV4 then
			local ipv4_hdr = ffi.cast("struct ipv4_hdr *", fields.l3_hdr)
			next_hop = policylib.c.lpm_lookup_ipv4(
			v["lpm"], ipv4_hdr.dst_addr)
		else
			local ipv6_hdr = ffi.cast("struct ipv6_hdr *", fields.l3_hdr)
			next_hop = policylib.c.lpm_lookup_ipv6(
			v["lpm6"], ipv6_hdr.dst_addr)
		end

		if next_hop ~= -1 then return v["groups"][next_hop] end
	end

	return nil
end

return M

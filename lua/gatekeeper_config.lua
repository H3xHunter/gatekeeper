-- The gatekeeper module is already loaded, but it only contains
-- C functions statically linked.
-- Unsetting its package.loaded entry allows Lua to load
-- the Lua functions of the module.
package.loaded["gatekeeper"] = nil
require "gatekeeper"

function gatekeeper_init()

	-- When gatekeeper_server is true,
	-- Gatekeeper will run as a Gatekeeper server.
	-- Otherwise, it will run as a grantor server.
	local gatekeeper_server = true

	local numa_table = gatekeeper.get_numa_table()

	local netf = require("net")
	local net_conf = netf(gatekeeper_server)

	local llsf = require("lls")
	local lls_conf = llsf(net_conf, numa_table)

	local gk_conf

	if gatekeeper_server == true then
		local gkf = require("gk")
		local ggu_lcore
		gk_conf, ggu_lcore = gkf(net_conf, numa_table)

		local gguf = require("ggu")
		local ggu_conf = gguf(net_conf, gk_conf, ggu_lcore)
	else
		local gtf = require("gt")
		local gt_conf = gtf(net_conf, numa_table)
	end

	-- Allocate CPS after to increase the change that the LLS block is
	-- allocated in the same NUMA node as the GK/GT/GK-GT-unit blocks.
	local cpsf = require("cps")
	local cps_conf = cpsf(net_conf, lls_conf, numa_table)

	local dyf = require("dyn_cfg")
	local dy_conf = dyf(gk_conf, numa_table)

	return 0
end

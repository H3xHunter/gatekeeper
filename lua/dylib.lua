module("dylib", package.seeall)

package.loaded["gatekeeper"] = nil
require "gatekeeper"

--
-- C functions exported through FFI
--

local ffi = require("ffi")

-- Structs
ffi.cdef[[

enum gk_fib_action {
	GK_FWD_GRANTOR,
	GK_FWD_GATEWAY_FRONT_NET,
	GK_FWD_GATEWAY_BACK_NET,
	GK_FWD_NEIGHBOR_FRONT_NET,
	GK_FWD_NEIGHBOR_BACK_NET,
	GK_DROP,
	GK_FIB_MAX,
};

]]

-- Functions and wrappers
ffi.cdef[[

int add_fib_entry(const char *ip_prefix, enum gk_fib_action action,
	const char *grantor_or_gateway, struct gk_config *gk_conf);
int del_fib_entry(const char *ip_prefix, struct gk_config *gk_conf);

]]

c = ffi.C

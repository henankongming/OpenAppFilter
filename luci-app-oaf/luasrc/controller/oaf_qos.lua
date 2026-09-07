module("luci.controller.oaf_qos", package.seeall)

function index()
	local fs = require "nixio.fs"
	if not fs.access("/etc/config/appqos") then
		return
	end

	entry({"admin", "services", "oaf", "qos"}, cbi("oaf/qos"), _("QoS"), 35).dependent = true
end

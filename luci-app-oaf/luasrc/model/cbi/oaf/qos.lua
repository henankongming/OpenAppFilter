local uci = require "luci.model.uci".cursor()
local m, s, o

m = Map("appqos", translate("Application QoS"), translate("Attach a bandwidth profile to an existing AppFilter rule. The AppFilter rule remains the single source of truth for application, user and time conditions."))

s = m:section(NamedSection, "global", "global", translate("QoS service"))
s.anonymous = true

o = s:option(Flag, "enabled", translate("Enable QoS"))
o.default = "0"
o.rmempty = false

s = m:section(TypedSection, "rule", translate("QoS profiles"))
s.anonymous = true
s.addremove = true
s.sortable = true
s.template = "cbi/tblsection"

o = s:option(Flag, "enabled", translate("Enable"))
o.default = "1"
o.rmempty = false

o = s:option(Value, "name", translate("Profile name"))
o.placeholder = translate("Evening 512K")

o = s:option(Value, "priority", translate("Priority"))
o.default = "100"
o.datatype = "uinteger"
o.description = translate("Lower values win when multiple QoS profiles match.")

o = s:option(ListValue, "source_rule_id", translate("AppFilter rule"))
o:value("0", translate("Manual conditions"))
uci:foreach("appfilter", "rule", function(rule)
	if rule.id then
		local label = tostring(rule.name or "")
		if label == "" then label = translate("Unnamed rule") end
		o:value(tostring(rule.id), string.format("%s (#%s)", label, tostring(rule.id)))
	end
end)
o.default = "0"
o.description = translate("Recommended: select an existing AppFilter rule so its app, client and time windows are reused automatically.")

o = s:option(ListValue, "mode", translate("Client"))
o:value("1", translate("All clients"))
o:value("2", translate("Single client"))
o.default = "1"
o:depends("source_rule_id", "0")

o = s:option(Value, "user_mac", translate("Client MAC"))
o.placeholder = "AA:BB:CC:DD:EE:FF"
o.datatype = "macaddr"
o:depends("source_rule_id", "0")

o = s:option(DynamicList, "app_id", translate("Application ID"))
o.description = translate("Manual mode only. Enter IDs such as 1001 or ranges such as 1001-1009.")
o:depends("source_rule_id", "0")

o = s:option(DynamicList, "time_rule", translate("Time windows"))
o.description = translate("Manual mode only. Example: 1,2,3,18:00,22:00.")
o:depends("source_rule_id", "0")

local function validate_rate(section, value)
	local n = tonumber(value)
	if not n or n < 0 or n ~= math.floor(n) then
		return nil, translate("Enter a non-negative integer.")
	end
	return value
end

o = s:option(Value, "download_kbps", translate("Download (Kbit/s)"))
o.default = "1024"
o.validate = validate_rate

o = s:option(Value, "upload_kbps", translate("Upload (Kbit/s)"))
o.default = "256"
o.validate = validate_rate

return m

local m, s, o

m = Map("appqos", translate("Application QoS"), translate("Set different upload/download limits for different applications, clients and time windows. Rules are evaluated every few seconds and only the currently active schedules are installed in the traffic shaper."))

s = m:section(NamedSection, "global", "global", translate("QoS service"))
s.anonymous = true

o = s:option(Flag, "enabled", translate("Enable QoS"))
o.default = o.disabled

o.rmempty = false

s = m:section(TypedSection, "rule", translate("QoS rules"))
s.anonymous = true
s.addremove = true
s.sortable = true
s.template = "cbi/tblsection"

local function validate_positive(section, value)
	local n = tonumber(value)
	if not n or n < 0 or n ~= math.floor(n) then
		return nil, translate("Enter a non-negative integer.")
	end
	return value
end

o = s:option(Flag, "enabled", translate("Enable"))
o.default = o.enabled

o.rmempty = false

o = s:option(Value, "name", translate("Name"))
o.placeholder = translate("YouTube evening limit")

o = s:option(Value, "priority", translate("Priority"))
o.default = "100"
o.datatype = "uinteger"
o.description = translate("Lower values win when schedules overlap.")

o = s:option(ListValue, "mode", translate("Client"))
o:value("1", translate("All clients"))
o:value("2", translate("Single client"))
o.default = "1"

o = s:option(Value, "user_mac", translate("Client MAC"))
o.placeholder = "AA:BB:CC:DD:EE:FF"
o:depends("mode", "2")
o.datatype = "macaddr"

o = s:option(DynamicList, "app_id", translate("Application ID"))
o.description = translate("Enter application IDs or ranges, e.g. 1001 or 1001-1009.")
o.datatype = "uinteger"

o = s:option(DynamicList, "time_rule", translate("Time windows"))
o.description = translate("Example: 1,2,3,18:00,22:00. Weekday 0=Sunday, 6=Saturday. Overnight windows are supported.")

-- Keep the two directions explicit; 0 is rejected by the runtime because a usable rule needs download > 0.
o = s:option(Value, "download_kbps", translate("Download (Kbit/s)"))
o.default = "1024"
o.datatype = "uinteger"
o.validate = validate_positive

o = s:option(Value, "upload_kbps", translate("Upload (Kbit/s)"))
o.default = "256"
o.datatype = "uinteger"
o.validate = validate_positive

return m

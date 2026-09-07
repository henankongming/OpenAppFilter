module("luci.controller.oaf_qos", package.seeall)

local utl = require "luci.util"
local uci = require "luci.model.uci".cursor()
local json = require "luci.jsonc"

local function reply(obj)
    luci.http.prepare_content("application/json")
    luci.http.write_json(obj)
end

local function normalize_action(value)
    value = tostring(value or "block")
    if value ~= "normal" and value ~= "block" and value ~= "limit" then
        return "block"
    end
    return value
end

local function normalize_rate(value)
    local n = tonumber(value) or 0
    if n < 0 or n ~= math.floor(n) or n > 10000000 then
        return nil
    end
    return math.floor(n)
end

function index()
    -- QoS configuration is intentionally edited inside the existing App Filter rule dialog.
    entry({"admin", "services", "oaf", "api", "app_filter", "get_rule_actions"}, call("get_rule_actions"), nil).leaf = true
    entry({"admin", "services", "oaf", "api", "app_filter", "set_rule_action"}, call("set_rule_action"), nil).leaf = true
    entry({"admin", "services", "oaf", "api", "app_filter", "del_rule_action"}, call("del_rule_action"), nil).leaf = true
end

function get_rule_actions()
    local result = {}
    uci:foreach("appqos", "rule", function(section)
        local id = tonumber(section.rule_id)
        if id and id > 0 then
            result[tostring(id)] = {
                enabled = tonumber(section.enabled) ~= 0 and 1 or 0,
                action = normalize_action(section.action),
                upload_kbps = normalize_rate(section.upload_kbps) or 0,
                download_kbps = normalize_rate(section.download_kbps) or 0
            }
        end
    end)
    reply({code = 0, data = result})
end

local function find_section_name(rule_id)
    local found
    uci:foreach("appqos", "rule", function(section)
        if tonumber(section.rule_id) == rule_id then
            found = section[".name"]
        end
    end)
    return found
end

function set_rule_action()
    local data_str = luci.http.formvalue("data")
    local data = data_str and json.parse(data_str) or nil
    if type(data) ~= "table" then
        return reply({code = 1, message = "Invalid request data"})
    end

    local rule_id = tonumber(data.rule_id)
    if not rule_id or rule_id <= 0 then
        return reply({code = 1, message = "Invalid rule id"})
    end

    local upload = normalize_rate(data.upload_kbps)
    local download = normalize_rate(data.download_kbps)
    if upload == nil or download == nil then
        return reply({code = 1, message = "Invalid rate"})
    end

    local action = normalize_action(data.action)
    local enabled = tonumber(data.enabled) == 0 and 0 or 1
    local section_name = find_section_name(rule_id)
    if not section_name then
        local ok = uci:add("appqos", "rule")
        if not ok then
            return reply({code = 1, message = "Failed to create QoS action"})
        end
        section_name = ok
        uci:set("appqos", section_name, "rule_id", tostring(rule_id))
    end

    uci:set("appqos", section_name, "action", action)
    uci:set("appqos", section_name, "enabled", tostring(enabled))
    uci:set("appqos", section_name, "upload_kbps", tostring(upload))
    uci:set("appqos", section_name, "download_kbps", tostring(download))
    if not uci:commit("appqos") then
        return reply({code = 1, message = "Failed to save QoS action"})
    end

    reply({code = 0, message = "QoS action saved"})
end

function del_rule_action()
    local rule_id = tonumber(luci.http.formvalue("rule_id"))
    if not rule_id or rule_id <= 0 then
        return reply({code = 1, message = "Invalid rule id"})
    end
    local section_name = find_section_name(rule_id)
    if section_name then
        uci:delete("appqos", section_name)
        uci:commit("appqos")
    end
    reply({code = 0})
end

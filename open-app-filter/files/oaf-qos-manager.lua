#!/usr/bin/lua

local uci = require "uci"

local CHECK_INTERVAL = 5
local MAX_ENTRIES = 1800
local MAX_APP_ID = 32000
local MAX_CLASS_ID = 4094
local PROC_FILE = "/proc/oaf_qos_rules"
local KERNEL_STATE = "/tmp/oaf_qos_kernel.rules"
local TC_RULE_FILE = "/tmp/oaf_qos_tc.rules"
local LOG_FILE = "/tmp/log/oaf_qos.log"

local function log(message)
    local fd = io.open(LOG_FILE, "a")
    if fd then
        fd:write(os.date("[%Y-%m-%d %H:%M:%S] ") .. message .. "\n")
        fd:close()
    end
end

local function trim(value)
    return tostring(value or ""):match("^%s*(.-)%s*$") or ""
end

local function normalize_mac(value)
    local mac = trim(value):upper()
    if mac:match("^%x%x:%x%x:%x%x:%x%x:%x%x:%x%x$") then
        return mac
    end
    return nil
end

local function minutes(value)
    local hour, min = tostring(value or ""):match("^(%d%d?):(%d%d)$")
    hour, min = tonumber(hour), tonumber(min)
    if not hour or not min or hour > 23 or min > 59 then
        return nil
    end
    return hour * 60 + min
end

local function schedule_active(time_rules, current_wd, current_min)
    for _, rule in ipairs(time_rules or {}) do
        local matched_day = false
        for _, wd in ipairs(rule.weekdays or {}) do
            if tonumber(wd) == current_wd then
                matched_day = true
                break
            end
        end
        if matched_day then
            local start_min = minutes(rule.start_time)
            local end_min = minutes(rule.end_time)
            if start_min and end_min then
                if start_min == end_min then
                    return true
                elseif start_min < end_min then
                    if current_min >= start_min and current_min < end_min then return true end
                else
                    if current_min >= start_min or current_min < end_min then return true end
                end
            end
        end
    end
    return false
end

local function parse_time_rule(value)
    local parts = {}
    for token in tostring(value or ""):gmatch("[^,]+") do
        table.insert(parts, trim(token))
    end
    if #parts < 3 then return nil end
    local times = {}
    local weekdays = {}
    for _, token in ipairs(parts) do
        if token:match("^%d%d?:%d%d$") then
            table.insert(times, token)
        else
            local wd = tonumber(token)
            if wd and wd >= 0 and wd <= 6 then table.insert(weekdays, wd) end
        end
    end
    if #times < 2 or #weekdays == 0 then return nil end
    return { weekdays = weekdays, start_time = times[1], end_time = times[2] }
end

local function parse_app_token(token)
    token = trim(token)
    local start_id, end_id = token:match("^(%d+)%-(%d+)$")
    if start_id and end_id then
        start_id, end_id = tonumber(start_id), tonumber(end_id)
        if start_id > end_id then start_id, end_id = end_id, start_id end
        if start_id < 1 then start_id = 1 end
        if end_id > MAX_APP_ID then end_id = MAX_APP_ID end
        if end_id - start_id > 4096 then return nil end
        local ids = {}
        for id = start_id, end_id do table.insert(ids, id) end
        return ids
    end
    local app_id = tonumber(token)
    if app_id and app_id > 0 and app_id <= MAX_APP_ID then return { app_id } end
    return nil
end

local function load_actions()
    local actions = {}
    local cursor = uci.cursor()
    cursor:foreach("appqos", "rule", function(section)
        local id = tonumber(section.rule_id)
        if id and id > 0 then
            local action = tostring(section.action or "block")
            if action ~= "normal" and action ~= "block" and action ~= "limit" then action = "block" end
            actions[id] = {
                enabled = tonumber(section.enabled) ~= 0,
                action = action,
                upload_kbps = math.max(0, math.floor(tonumber(section.upload_kbps) or 0)),
                download_kbps = math.max(0, math.floor(tonumber(section.download_kbps) or 0))
            }
        end
    end)
    cursor:unload("appqos")
    return actions
end

local function load_active_limit_rules(now_wd, now_min, actions)
    local cursor = uci.cursor()
    local rules = {}
    cursor:foreach("appfilter", "rule", function(section)
        local id = tonumber(section.id) or 0
        local state = actions[id]
        if id > 0 and state and state.enabled and state.action == "limit" then
            local time_rules = {}
            local raw_time = section.time_rule
            if type(raw_time) == "string" then raw_time = { raw_time } end
            if type(raw_time) == "table" then
                for _, token in ipairs(raw_time) do
                    local parsed = parse_time_rule(token)
                    if parsed then table.insert(time_rules, parsed) end
                end
            end
            if schedule_active(time_rules, now_wd, now_min) then
                local app_ids = {}
                local raw_apps = section.app_id
                if type(raw_apps) == "string" then raw_apps = { raw_apps } end
                if type(raw_apps) == "table" then
                    for _, token in ipairs(raw_apps) do
                        local ids = parse_app_token(token)
                        if ids then
                            for _, app_id in ipairs(ids) do table.insert(app_ids, app_id) end
                        end
                    end
                end
                if #app_ids > 0 then
                    local mode = tonumber(section.mode) or 1
                    local mac = normalize_mac(section.user_mac)
                    if mode == 1 or (mode == 2 and mac) then
                        table.insert(rules, {
                            id = id,
                            mode = mode,
                            mac = mac,
                            apps = app_ids,
                            upload = state.upload_kbps,
                            download = state.download_kbps
                        })
                    end
                end
            end
        end
    end)
    cursor:unload("appfilter")
    return rules
end

local function choose_rate(current, requested)
    if requested <= 0 then return current end
    if not current or current <= 0 then return requested end
    return math.min(current, requested)
end

local function build_profiles(rules)
    local profiles = {}
    local assignments = {}
    for _, rule in ipairs(rules) do
        local mac_key = rule.mode == 2 and rule.mac or "*"
        for _, app_id in ipairs(rule.apps) do
            local key = tostring(app_id) .. "|" .. mac_key
            local p = assignments[key]
            if not p then
                p = { app_id = app_id, mac = mac_key, upload = rule.upload, download = rule.download }
                assignments[key] = p
            else
                p.upload = choose_rate(p.upload, rule.upload)
                p.download = choose_rate(p.download, rule.download)
            end
        end
    end

    local by_rate = {}
    local next_class = 2
    local entries = {}
    for _, item in pairs(assignments) do
        local rate_key = tostring(item.download) .. ":" .. tostring(item.upload)
        local class_id = by_rate[rate_key]
        if not class_id then
            if next_class > MAX_CLASS_ID then return nil, nil, "too many QoS rate profiles" end
            class_id = next_class
            next_class = next_class + 1
            by_rate[rate_key] = class_id
            profiles[class_id] = { download = item.download, upload = item.upload }
        end
        table.insert(entries, { class_id = class_id, app_id = item.app_id, mac = item.mac })
    end

    table.sort(entries, function(a, b)
        if a.app_id ~= b.app_id then return a.app_id < b.app_id end
        return a.mac < b.mac
    end)
    if #entries > MAX_ENTRIES then return nil, nil, "too many active QoS classifiers" end

    local kernel = { "clear\n" }
    for _, item in ipairs(entries) do
        table.insert(kernel, string.format("%d %d %s\n", item.class_id, item.app_id, item.mac))
    end

    local tc = {}
    for class_id, profile in pairs(profiles) do
        table.insert(tc, string.format("%d %d %d\n", class_id, profile.download, profile.upload))
    end
    table.sort(tc)
    return table.concat(kernel), table.concat(tc), nil
end

local function read_file(path)
    local fd = io.open(path, "r")
    if not fd then return nil end
    local data = fd:read("*a") or ""
    fd:close()
    return data
end

local function write_file(path, data)
    local tmp = path .. ".tmp"
    local fd = io.open(tmp, "w")
    if not fd then return false end
    fd:write(data)
    fd:close()
    return os.rename(tmp, path) ~= nil
end

local function apply_kernel_rules(data)
    if read_file(KERNEL_STATE) == data then return true end
    local fd = io.open(PROC_FILE, "w")
    if not fd then log("QoS procfs unavailable: " .. PROC_FILE); return false end
    fd:write(data)
    fd:close()
    write_file(KERNEL_STATE, data)
    return true
end

local function apply_tc(data)
    if read_file(TC_RULE_FILE) == data then return true end
    write_file(TC_RULE_FILE, data)
    local rc = os.execute("/usr/bin/oaf-qos.sh apply " .. TC_RULE_FILE)
    return rc == true or rc == 0
end

local function tick()
    local actions = load_actions()
    local now = os.date("*t")
    local current_wd = (tonumber(now.wday) or 1) - 1
    local current_min = (tonumber(now.hour) or 0) * 60 + (tonumber(now.min) or 0)
    local rules = load_active_limit_rules(current_wd, current_min, actions)
    local kernel_rules, tc_rules, err = build_profiles(rules)
    if err then
        log(err)
        apply_kernel_rules("clear\n")
        apply_tc("")
        return
    end
    apply_kernel_rules(kernel_rules)
    apply_tc(tc_rules)
end

log("QoS manager started")
while true do
    local ok, err = pcall(tick)
    if not ok then log("QoS manager error: " .. tostring(err)) end
    os.execute("sleep " .. tostring(CHECK_INTERVAL))
end

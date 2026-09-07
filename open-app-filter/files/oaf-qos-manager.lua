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

local function sleep(seconds)
    os.execute("sleep " .. tostring(seconds))
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

local function parse_weekdays_and_times(value)
    local weekdays = {}
    local start_time = nil
    local end_time = nil
    if not value or value == "" then
        return nil
    end

    local weekday_part, range_start, range_end = value:match("^([^;]+);([^;]+);([^;]+)$")
    if weekday_part and range_start and range_end then
        for token in weekday_part:gmatch("[^,]+") do
            local wd = tonumber(trim(token))
            if wd and wd >= 0 and wd <= 6 then
                weekdays[wd] = true
            end
        end
        start_time = trim(range_start)
        end_time = trim(range_end)
    else
        local parts = {}
        for token in value:gmatch("[^,]+") do
            table.insert(parts, trim(token))
        end
        for _, token in ipairs(parts) do
            if token:match("^%d%d?:%d%d$") then
                if not start_time then
                    start_time = token
                elseif not end_time then
                    end_time = token
                end
            else
                local wd = tonumber(token)
                if wd and wd >= 0 and wd <= 6 then
                    weekdays[wd] = true
                end
            end
        end
    end

    if not start_time or not end_time or next(weekdays) == nil then
        return nil
    end
    return { weekdays = weekdays, start_time = start_time, end_time = end_time }
end

local function minutes(value)
    local hour, min = tostring(value):match("^(%d%d?):(%d%d)$")
    hour = tonumber(hour)
    min = tonumber(min)
    if not hour or not min or hour > 23 or min > 59 then
        return nil
    end
    return hour * 60 + min
end

local function schedule_active(time_rules, current_wd, current_min)
    for _, rule in ipairs(time_rules or {}) do
        if rule.weekdays[current_wd] then
            local start_min = minutes(rule.start_time)
            local end_min = minutes(rule.end_time)
            if start_min and end_min then
                if start_min == end_min then
                    return true
                elseif start_min < end_min then
                    if current_min >= start_min and current_min < end_min then
                        return true
                    end
                else
                    if current_min >= start_min or current_min < end_min then
                        return true
                    end
                end
            end
        end
    end
    return false
end

local function parse_app_token(token)
    token = trim(token)
    local a, b = token:match("^(%d+)%-(%d+)$")
    if a and b then
        a, b = tonumber(a), tonumber(b)
        if a > b then a, b = b, a end
        if a < 1 then a = 1 end
        if b > MAX_APP_ID then b = MAX_APP_ID end
        local ids = {}
        if b - a > 2048 then
            return nil, "application range is too large"
        end
        for id = a, b do
            table.insert(ids, id)
        end
        return ids
    end

    local app_id = tonumber(token)
    if app_id and app_id > 0 and app_id <= MAX_APP_ID then
        return { app_id }
    end
    return nil, "invalid application id"
end

local function load_rules(current_wd, current_min)
    local cursor = uci.cursor()
    local rules = {}
    cursor:foreach("appqos", "rule", function(section)
        local enabled = tonumber(section.enabled) or 1
        local down = tonumber(section.download_kbps) or 0
        local up = tonumber(section.upload_kbps) or 0
        if enabled == 1 and down > 0 and up >= 0 then
            local time_rules = {}
            local raw_times = section.time_rule
            if type(raw_times) == "string" then raw_times = { raw_times } end
            if type(raw_times) == "table" then
                for _, raw in ipairs(raw_times) do
                    local parsed = parse_weekdays_and_times(raw)
                    if parsed then table.insert(time_rules, parsed) end
                end
            end
            if schedule_active(time_rules, current_wd, current_min) then
                local apps = {}
                local raw_apps = section.app_id
                if type(raw_apps) == "string" then raw_apps = { raw_apps } end
                if type(raw_apps) == "table" then
                    for _, raw in ipairs(raw_apps) do
                        local ids, err = parse_app_token(raw)
                        if ids then
                            for _, id in ipairs(ids) do table.insert(apps, id) end
                        else
                            log("ignoring rule " .. tostring(section[".name"]) .. ": " .. tostring(err))
                        end
                    end
                end
                if #apps > 0 then
                    local mode = tonumber(section.mode) or 1
                    local mac = normalize_mac(section.user_mac)
                    if mode == 2 and not mac then
                        log("ignoring rule " .. tostring(section[".name"]) .. ": invalid MAC")
                    else
                        table.insert(rules, {
                            section_id = tostring(section[".name"] or ""),
                            priority = tonumber(section.priority) or 100,
                            mode = mode,
                            mac = mac,
                            apps = apps,
                            down = math.floor(down),
                            up = math.floor(up)
                        })
                    end
                end
            end
        end
    end)
    cursor:unload("appqos")
    table.sort(rules, function(a, b)
        if a.priority ~= b.priority then return a.priority < b.priority end
        if a.mode ~= b.mode then return a.mode > b.mode end
        return a.section_id < b.section_id
    end)
    return rules
end

local function assign_classes(rules)
    local profile_to_class = {}
    local next_class = 2
    for _, rule in ipairs(rules) do
        local key = tostring(rule.down) .. ":" .. tostring(rule.up)
        if not profile_to_class[key] then
            if next_class > MAX_CLASS_ID then
                return nil, "too many distinct QoS rate profiles"
            end
            profile_to_class[key] = next_class
            next_class = next_class + 1
        end
        rule.class_id = profile_to_class[key]
    end
    return profile_to_class
end

local function add_unique_entry(entries, seen, class_id, app_id, mac, priority)
    local key = string.format("%d|%d|%s", app_id, mac or "*", priority)
    if seen[key] then return end
    seen[key] = true
    table.insert(entries, {
        class_id = class_id,
        app_id = app_id,
        mac = mac or "*",
        priority = priority
    })
end

local function build_output(rules)
    local entries = {}
    local seen = {}
    local tc_profiles = {}
    for _, rule in ipairs(rules) do
        tc_profiles[rule.class_id] = { down = rule.down, up = rule.up }
        for _, app_id in ipairs(rule.apps) do
            add_unique_entry(entries, seen, rule.class_id, app_id, rule.mode == 2 and rule.mac or "*", rule.priority)
        end
    end
    table.sort(entries, function(a, b)
        if a.priority ~= b.priority then return a.priority < b.priority end
        if a.mac == "*" and b.mac ~= "*" then return false end
        if a.mac ~= "*" and b.mac == "*" then return true end
        if a.app_id ~= b.app_id then return a.app_id < b.app_id end
        return a.class_id < b.class_id
    end)
    if #entries > MAX_ENTRIES then
        return nil, nil, "too many active QoS classifiers"
    end

    local kernel = {}
    table.insert(kernel, "clear\n")
    for _, item in ipairs(entries) do
        table.insert(kernel, string.format("%d %d %s\n", item.class_id, item.app_id, item.mac))
    end

    local tc = {}
    for class_id, profile in pairs(tc_profiles) do
        table.insert(tc, string.format("%d %d %d\n", class_id, profile.down, profile.up))
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
    os.rename(tmp, path)
    return true
end

local function apply_kernel_rules(data)
    if read_file(KERNEL_STATE) == data then return true end
    local fd = io.open(PROC_FILE, "w")
    if not fd then
        log("QoS procfs is unavailable: " .. PROC_FILE)
        return false
    end
    fd:write(data)
    fd:close()
    write_file(KERNEL_STATE, data)
    return true
end

local function apply_tc(data)
    if read_file(TC_RULE_FILE) == data then return true end
    write_file(TC_RULE_FILE, data)
    local result = os.execute("/usr/bin/oaf-qos.sh apply " .. TC_RULE_FILE)
    if result == true or result == 0 then
        return true
    end
    log("tc apply failed")
    return false
end

local function tick()
    local base = uci.cursor()
    local enabled = tonumber(base:get("appqos", "global", "enabled")) or 0
    base:unload("appqos")
    if enabled ~= 1 then
        apply_kernel_rules("clear\n")
        apply_tc("")
        return
    end

    local now = os.date("*t")
    local current_wd = tonumber(os.date("%w")) or 0
    local current_min = (now.hour * 60) + now.min
    local rules = load_rules(current_wd, current_min)
    local _, profile_error = assign_classes(rules)
    if profile_error then
        log(profile_error)
        apply_kernel_rules("clear\n")
        apply_tc("")
        return
    end
    local kernel_rules, tc_rules, err = build_output(rules)
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
    sleep(CHECK_INTERVAL)
end

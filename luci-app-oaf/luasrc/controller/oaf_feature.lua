module("luci.controller.oaf_feature", package.seeall)

function index()
	entry({"admin", "services", "oaf", "feature"},
		template("oaf/feature"),
		_("Feature Library"), 80).dependent = true
	entry({"admin", "services", "oaf", "feature", "info"}, call("get_feature_info"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "class_list"}, call("get_feature_class_list"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "online_config"}, call("get_feature_online_config"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "online_save"}, call("set_feature_online_config"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "online_list"}, call("get_feature_online_list"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "online_start"}, call("start_feature_online_update"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "online_status"}, call("get_feature_online_update_status"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "manual_upload"}, call("manual_feature_upload"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "custom_list"}, call("get_custom_feature_list"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "custom_class_list"}, call("get_custom_feature_class_list"), nil).leaf = true
	entry({"admin", "services", "oaf", "feature", "custom_save"}, call("set_custom_feature_list"), nil).leaf = true
end

function get_feature_info()
	local json = require "luci.jsonc"
	local util = require "luci.util"
	local http = require "luci.http"
	local resp = util.ubus("fwx", "common", {api = "get_feature_info", data = {}})

	http.prepare_content("application/json")
	http.write(json.stringify(resp or {code = 4000}))
end

local function write_fwx_response(api, data)
	local json = require "luci.jsonc"
	local util = require "luci.util"
	local http = require "luci.http"
	local resp = util.ubus("fwx", "common", {api = api, data = data or {}})

	http.prepare_content("application/json")
	http.write(json.stringify(resp or {code = 4000}))
end

local function shell_quote(value)
	return "'" .. tostring(value or ""):gsub("'", "'\\''") .. "'"
end

function manual_feature_upload()
	local http = require "luci.http"
	local sys = require "luci.sys"
	local json = require "luci.jsonc"
	local upload_path = "/tmp/oaf_manual_feature_upload.tar.gz"
	local fp = nil
	local file_error = nil
	local file_name = nil
	local file_size = 0

	http.setfilehandler(function(meta, chunk, eof)
		if meta then
			if meta.name ~= "feature_file" then
				file_error = "invalid upload field"
				return
			end
			file_name = meta.filename or "feature_package.bin"
			fp = io.open(upload_path, "w")
			if not fp then
				file_error = "unable to create temporary upload file"
				return
			end
		end

		if chunk and fp and not file_error then
			file_size = file_size + #chunk
			if file_size > (20 * 1024 * 1024) then
				file_error = "feature package is too large"
			else
				local ok = fp:write(chunk)
				if not ok then
					file_error = "failed to write uploaded feature package"
				end
			end
		end

		if eof and fp then
			fp:close()
			fp = nil
		end
	end)

	http.formvalue("feature_file")
	if fp then
		fp:close()
		fp = nil
	end

	local response = {code = 4000, data = {}}
	if file_error then
		response.data.error = file_error
		http.prepare_content("application/json")
		http.write(json.stringify(response))
		return
	end
	if not file_name or file_size <= 0 then
		response.data.error = "please select a feature package"
		http.prepare_content("application/json")
		http.write(json.stringify(response))
		return
	end

	local output = sys.exec("/usr/bin/oaf_manual_feature_update " .. shell_quote(upload_path) .. " 2>&1") or ""
	os.remove(upload_path)
	output = output:gsub("%s+$", "")

	if output:match("^OK:") then
		response.code = 2000
		response.data.message = output:gsub("^OK:", "")
	else
		response.data.error = output:gsub("^ERROR:", "")
		if response.data.error == "" then
			response.data.error = "manual feature library update failed"
		end
	end

	http.prepare_content("application/json")
	http.write(json.stringify(response))
end

function get_feature_online_config()
	write_fwx_response("get_feature_online_config", {})
end

function set_feature_online_config()
	local http = require "luci.http"
	write_fwx_response("set_feature_online_config", {
		token = http.formvalue("token") or ""
	})
end

function get_feature_online_list()
	local http = require "luci.http"
	local lang = http.formvalue("lang") or "cn"
	local refresh = tonumber(http.formvalue("refresh") or "0") or 0
	if lang ~= "cn" and lang ~= "en" then
		lang = "cn"
	end
	write_fwx_response("get_feature_online_list", {
		lang = lang,
		device_lang = http.formvalue("device_lang") or "",
		refresh = refresh
	})
end

function start_feature_online_update()
	local http = require "luci.http"
	local lang = http.formvalue("lang") or "cn"
	if lang ~= "cn" and lang ~= "en" then
		lang = "cn"
	end
	write_fwx_response("start_feature_online_update", {
		id = http.formvalue("id") or "",
		lang = lang,
		md5 = http.formvalue("md5") or ""
	})
end

function get_feature_online_update_status()
	write_fwx_response("get_feature_online_update_status", {})
end

function get_custom_feature_list()
	write_fwx_response("get_custom_feature", {})
end

function get_custom_feature_class_list()
	write_fwx_response("get_custom_feature_class_list", {})
end

function set_custom_feature_list()
	local json = require "luci.jsonc"
	local http = require "luci.http"
	local data_str = http.formvalue("data")
	local ok, data_obj = pcall(json.parse, data_str or "")

	if not ok then
		data_obj = nil
	end
	if type(data_obj) ~= "table" or type(data_obj.app_list) ~= "table" then
		http.prepare_content("application/json")
		http.write(json.stringify({code = 4000, data = {error = "invalid request data"}}))
		return
	end
	write_fwx_response("set_custom_feature", data_obj)
end

function get_feature_class_list()
	local json = require "luci.jsonc"
	local util = require "luci.util"
	local http = require "luci.http"
	local resp = util.ubus("fwx", "common", {CopyRight = "www.fanchmwrt.com", api = "class_list", data = {}})

	http.prepare_content("application/json")
	if resp and resp.code == 2000 and resp.data then
		http.write(json.stringify(resp.data))
	else
		http.write(json.stringify({class_list = {}}))
	end
end

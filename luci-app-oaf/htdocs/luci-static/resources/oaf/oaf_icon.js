(function(window, $) {
    'use strict';

    var iconColors = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#06b6d4', '#f97316', '#ec4899', '#84cc16', '#6366f1'];
    var iconStatusCache = {};

    function hashColor(value) {
        var text = value ? String(value) : '';
        var hash = 0;

        if (!text) {
            return iconColors[0];
        }

        for (var i = 0; i < text.length; i++) {
            hash = text.charCodeAt(i) + ((hash << 5) - hash);
        }

        return iconColors[Math.abs(hash) % iconColors.length];
    }

    function firstLetter(value) {
        var text = value ? String(value).trim() : '';
        return text ? text.charAt(0).toUpperCase() : '?';
    }

    function appIconSrc(resourceBase, appId) {
        var id = appId === undefined || appId === null ? '' : String(appId).trim();
        return id ? resourceBase + '/oaf/app_icons/' + encodeURIComponent(id) + '.png' : '';
    }

    function createLetterIcon(name, options) {
        options = options || {};
        return $('<span>')
            .addClass(options.className || 'oaf-generated-app-icon')
            .css({
                width: options.size || '20px',
                height: options.size || '20px',
                borderRadius: options.radius || '5px',
                display: 'inline-flex',
                alignItems: 'center',
                justifyContent: 'center',
                background: hashColor(name),
                color: '#fff',
                fontSize: options.fontSize || '11px',
                fontWeight: '700',
                flexShrink: 0,
                lineHeight: 1
            })
            .text(firstLetter(name));
    }

    function createAppIcon(appId, name, resourceBase, options) {
        options = options || {};
        var appName = name || '';
        var id = appId === undefined || appId === null ? '' : String(appId).trim();
        var src = appIconSrc(resourceBase, id);
        var iconDisabled = options.icon === 0 || options.icon === '0' || options.hasIcon === false;
        var $icon;

        if (id && iconDisabled) {
            iconStatusCache[id] = 'failed';
        }

        if (id && !iconDisabled && iconStatusCache[id] === 'loaded') {
            return $('<img>')
                .attr({ src: src, alt: appName })
                .css({
                    width: options.size || '20px',
                    height: options.size || '20px',
                    borderRadius: options.radius || '5px',
                    objectFit: options.objectFit || 'cover',
                    display: 'block',
                    flexShrink: 0
                });
        }

        $icon = createLetterIcon(appName, options);
        if (id && !iconDisabled && iconStatusCache[id] !== 'failed') {
            var loader = new Image();
            loader.onload = function() {
                iconStatusCache[id] = 'loaded';
                $icon.replaceWith($('<img>')
                    .attr({ src: src, alt: appName })
                    .css({
                        width: options.size || '20px',
                        height: options.size || '20px',
                        borderRadius: options.radius || '5px',
                        objectFit: options.objectFit || 'cover',
                        display: 'block',
                        flexShrink: 0,
                        border: 'none',
                        boxShadow: 'none'
                    }));
            };
            loader.onerror = function() {
                iconStatusCache[id] = 'failed';
            };
            loader.src = src;
        }

        return $icon;
    }

    window.OAFIcon = {
        colors: iconColors,
        hashColor: hashColor,
        appIconSrc: appIconSrc,
        createLetterIcon: createLetterIcon,
        createAppIcon: createAppIcon
    };

    /* Unified AppFilter rule action UI: Normal / Block / Limit. */
    $(function() {
        if (!/\/app_filter\/rules\/?$/.test(window.location.pathname || '')) {
            return;
        }

        var ACTION_DEFAULT = 'block';
        var actions = {};
        var originalPost = $.post;
        var api = function(name) {
            if (window.L && L.url) {
                return L.url('admin/services/oaf/api/app_filter/' + name);
            }
            return '/cgi-bin/luci/admin/services/oaf/api/app_filter/' + name;
        };

        function normalizeAction(value) {
            value = String(value || ACTION_DEFAULT);
            return (value === 'normal' || value === 'block' || value === 'limit') ? value : ACTION_DEFAULT;
        }

        function normalizeRate(value) {
            var n = parseInt(value, 10);
            if (isNaN(n) || n < 0 || n > 10000000) return 0;
            return n;
        }

        function actionLabel(action) {
            action = normalizeAction(action);
            return action === 'normal' ? '正常' : (action === 'limit' ? '限速' : '禁止');
        }

        function ensureStyles() {
            if ($('#oaf-rule-action-styles').length) return;
            $('head').append('<style id="oaf-rule-action-styles">' +
                '#oaf-rule-action-group{border-top:1px solid var(--border-color-low,#d1d5db);padding-top:12px;margin-top:8px}' +
                '#oaf-rule-action-group .oaf-rate-row{display:flex;gap:12px;flex-wrap:wrap;margin-top:8px}' +
                '#oaf-rule-action-group .oaf-rate-field{display:flex;flex-direction:column;gap:4px;min-width:180px}' +
                '#oaf-rule-action-group .oaf-rate-field input{max-width:180px}' +
                '.oaf-rule-action-cell{white-space:nowrap}' +
                '</style>');
        }

        function ensureFormFields() {
            if ($('#oaf-rule-action-group').length) return;
            var $anchor = $('#user-selection-group');
            if (!$anchor.length) $anchor = $('#rule-name').closest('.form-group');
            var $group = $('<div>').addClass('form-group').attr('id', 'oaf-rule-action-group');
            var $label = $('<label>').attr('for', 'oaf-rule-action').text('动作');
            var $select = $('<select>').attr({ id: 'oaf-rule-action', name: 'rule-action' });
            $select.append($('<option>').val('normal').text('正常'));
            $select.append($('<option>').val('block').text('禁止'));
            $select.append($('<option>').val('limit').text('限速'));
            var $hint = $('<div>').css({marginTop:'6px',color:'#777',fontSize:'12px'}).text('禁止沿用原有时间限制功能；正常不执行限制；限速仅对当前规则匹配的应用/用户/时间生效。');
            var $rates = $('<div>').addClass('oaf-rate-row');
            var $up = $('<div>').addClass('oaf-rate-field');
            $up.append($('<label>').attr('for', 'oaf-rule-upload-kbps').text('上传速度 (Kbit/s)'));
            $up.append($('<input>').attr({type:'text',inputmode:'numeric',id:'oaf-rule-upload-kbps',value:'0'}));
            var $down = $('<div>').addClass('oaf-rate-field');
            $down.append($('<label>').attr('for', 'oaf-rule-download-kbps').text('下载速度 (Kbit/s)'));
            $down.append($('<input>').attr({type:'text',inputmode:'numeric',id:'oaf-rule-download-kbps',value:'0'}));
            $rates.append($up).append($down);
            $group.append($label).append($select).append($hint).append($rates);
            $anchor.after($group);

            function refreshRates() {
                var limit = $('#oaf-rule-action').val() === 'limit';
                $rates.toggle(limit);
            }
            $select.on('change', refreshRates);
            refreshRates();
        }

        function resetActionForm() {
            ensureFormFields();
            $('#oaf-rule-action').val('block').trigger('change');
            $('#oaf-rule-upload-kbps').val('0');
            $('#oaf-rule-download-kbps').val('0');
        }

        function applyActionToForm(ruleId) {
            ensureFormFields();
            var item = actions[String(ruleId)] || { enabled: 1, action: ACTION_DEFAULT, upload_kbps: 0, download_kbps: 0 };
            $('#oaf-rule-action').val(normalizeAction(item.action)).trigger('change');
            $('#oaf-rule-upload-kbps').val(normalizeRate(item.upload_kbps));
            $('#oaf-rule-download-kbps').val(normalizeRate(item.download_kbps));
            setTimeout(function() {
                var enabled = item.enabled !== 0;
                $('#rule-enabled').prop('checked', enabled);
            }, 0);
        }

        function loadActions(callback) {
            $.get(api('get_rule_actions'), function(resp) {
                actions = (resp && resp.data) || {};
                if (callback) callback();
                refreshActionColumn();
            }).fail(function() {
                actions = {};
                if (callback) callback();
            });
        }

        function saveAction(ruleId, action, enabled, upload, download, callback) {
            ruleId = parseInt(ruleId, 10);
            if (!ruleId) { if (callback) callback(false); return; }
            $.post(api('set_rule_action'), {
                data: JSON.stringify({
                    rule_id: ruleId,
                    action: normalizeAction(action),
                    enabled: enabled ? 1 : 0,
                    upload_kbps: normalizeRate(upload),
                    download_kbps: normalizeRate(download)
                })
            }, function(resp) {
                var ok = !!(resp && resp.code === 0);
                if (ok) {
                    actions[String(ruleId)] = {
                        enabled: enabled ? 1 : 0,
                        action: normalizeAction(action),
                        upload_kbps: normalizeRate(upload),
                        download_kbps: normalizeRate(download)
                    };
                }
                if (callback) callback(ok);
                refreshActionColumn();
            }).fail(function() { if (callback) callback(false); });
        }

        function deleteAction(ruleId) {
            $.post(api('del_rule_action'), {rule_id: parseInt(ruleId, 10)}, function() {
                delete actions[String(ruleId)];
                refreshActionColumn();
            });
        }

        function findNewRuleId(ruleData, callback) {
            $.get(api('get_filter_rules'), function(resp) {
                var list = (resp && resp.data && resp.data.list) || [];
                var candidates = list.filter(function(rule) {
                    return String(rule.name || '') === String(ruleData.name || '') &&
                        parseInt(rule.mode || 1, 10) === parseInt(ruleData.mode || 1, 10);
                });
                candidates.sort(function(a, b) { return parseInt(b.id || 0, 10) - parseInt(a.id || 0, 10); });
                callback(candidates.length ? parseInt(candidates[0].id, 10) : null);
            }).fail(function() { callback(null); });
        }

        function decorateRuleTable($container) {
            var $table = $container.find('table.common-table').first();
            if (!$table.length) return;
            var $head = $table.find('thead tr').first();
            var $rows = $table.find('tbody tr');
            if (!$head.length) return;

            if (!$head.find('.oaf-action-head').length) {
                var statusIndex = -1;
                $head.children('th').each(function(i) {
                    if ($(this).text().trim() === 'Status') statusIndex = i;
                });
                var $th = $('<th>').addClass('oaf-action-head').text('动作');
                if (statusIndex >= 0) $head.children('th').eq(statusIndex).before($th); else $head.append($th);
            }

            $rows.each(function() {
                var $row = $(this);
                var $edit = $row.find('.btn-edit').first();
                if (!$edit.length) return;
                var ruleId = $edit.data('rule-id');
                var item = actions[String(ruleId)] || {enabled: 1, action: ACTION_DEFAULT, upload_kbps: 0, download_kbps: 0};
                var text = actionLabel(item.action);
                if (normalizeAction(item.action) === 'limit') {
                    text += ' ' + normalizeRate(item.upload_kbps) + '/' + normalizeRate(item.download_kbps) + ' Kbit/s';
                }
                var $cell = $row.find('.oaf-action-cell');
                if (!$cell.length) $cell = $('<td>').addClass('oaf-action-cell');
                $cell.text(text);
                var statusCell = $row.children('td').filter(function() { return $(this).text().trim() === 'Enabled' || $(this).text().trim() === 'Disabled'; }).first();
                if (statusCell.length) statusCell.before($cell); else $row.append($cell);
            });
        }

        function refreshActionColumn() {
            decorateRuleTable($('#rules-list-container'));
        }

        ensureStyles();
        ensureFormFields();
        loadActions(refreshActionColumn);

        var observerTarget = document.getElementById('rules-list-container');
        if (observerTarget && window.MutationObserver) {
            new MutationObserver(function() { refreshActionColumn(); }).observe(observerTarget, {childList:true,subtree:true});
        }

        document.addEventListener('click', function(event) {
            var target = event.target;
            if (!target) return;
            var $target = $(target);

            if ($target.closest('#btn-add-rule').length) {
                setTimeout(resetActionForm, 0);
                return;
            }

            var $edit = $target.closest('.btn-edit');
            if ($edit.length) {
                var editId = $edit.data('rule-id');
                setTimeout(function() { applyActionToForm(editId); }, 20);
                return;
            }
        }, false);

        /* Inject action/rate data into the existing AppFilter API payload without replacing saveRule(). */
        $.post = function(url, data, success, dataType) {
            var isRuleWrite = /\/api\/app_filter\/(add|update|delete)_filter_rule(?:$|[?#])/.test(String(url));
            if (!isRuleWrite) return originalPost.apply($, arguments);

            var parsed = null;
            var clonedData = data;
            if (data && typeof data.data === 'string') {
                try { parsed = JSON.parse(data.data); } catch (e) { parsed = null; }
            }

            var action = normalizeAction($('#oaf-rule-action').val());
            var enabledWanted = $('#rule-enabled').is(':checked') ? 1 : 0;
            var upload = normalizeRate($('#oaf-rule-upload-kbps').val());
            var download = normalizeRate($('#oaf-rule-download-kbps').val());
            if (action === 'limit' && upload === 0 && download === 0) {
                /* zero/zero is intentionally accepted as unlimited; it remains a valid limit profile. */
            }
            if (parsed) {
                parsed.enabled = action === 'block' ? enabledWanted : 0;
                data = $.extend({}, data, {data: JSON.stringify(parsed)});
            }

            var wrappedSuccess = function(resp) {
                if (success) success.apply(this, arguments);
                if (!resp || resp.code !== 0) return;

                var ruleId = parsed && parsed.id ? parseInt(parsed.id, 10) : null;
                var persist = function(id) {
                    if (!id) return;
                    saveAction(id, action, enabledWanted, upload, download);
                };
                if (ruleId) persist(ruleId);
                else findNewRuleId(parsed || {}, persist);

                if (/\/api\/app_filter\/delete_filter_rule(?:$|[?#])/.test(String(url))) {
                    var deleteId = parsed && parsed.id ? parsed.id : (data && data.rule_id);
                    if (deleteId) deleteAction(deleteId);
                }
            };

            var newArgs = [url, data, wrappedSuccess, dataType];
            return originalPost.apply($, newArgs);
        };

        /* Ensure newly created/updated action state is restored after the original form reset. */
        $(document).on('DOMNodeInserted', '#add-rule-modal', function() { ensureFormFields(); });
    });
})(window, window.jQuery);

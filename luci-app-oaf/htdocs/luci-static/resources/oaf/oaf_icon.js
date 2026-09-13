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
})(window, window.jQuery);

(function(window) {
    'use strict';

    function initManualFeatureUpload() {
        if (document.getElementById('oaf-manual-feature-upload') || !document.getElementById('tab-online')) {
            return;
        }

        var panel = document.querySelector('#tab-online .online-control-stack');
        if (!panel) return;

        var wrapper = document.createElement('div');
        wrapper.id = 'oaf-manual-feature-upload';
        wrapper.style.marginTop = '14px';
        wrapper.style.paddingTop = '14px';
        wrapper.style.borderTop = '1px solid var(--border-color-low,#e5e7eb)';
        wrapper.innerHTML =
            '<div class="online-control-row">' +
            '<label for="manual-feature-file">Manual Upload / 手动上传</label>' +
            '<input type="file" id="manual-feature-file" accept=".bin,.tar.gz,.tgz" style="max-width:360px;">' +
            '<div class="online-actions"><button type="button" class="cbi-button cbi-button-apply" id="manual-feature-upload-btn">Upload & Apply / 上传并应用</button></div>' +
            '</div>' +
            '<div class="feature-hint">Upload the official feature package (.tar.gz/.tgz, including feature.bin). The existing library is backed up before replacement. / 上传官方特征库压缩包（.tar.gz/.tgz，内含 feature.bin），替换前会自动备份当前特征库。</div>' +
            '<div id="manual-feature-upload-status" class="feature-status" style="display:none;"></div>';

        panel.appendChild(wrapper);

        var input = document.getElementById('manual-feature-file');
        var button = document.getElementById('manual-feature-upload-btn');
        var status = document.getElementById('manual-feature-upload-status');

        button.addEventListener('click', function() {
            var file = input.files && input.files[0];
            if (!file) {
                status.style.display = 'block';
                status.className = 'feature-status error';
                status.textContent = 'Please select a feature package. / 请选择特征库文件。';
                return;
            }
            if (file.size > 20 * 1024 * 1024) {
                status.style.display = 'block';
                status.className = 'feature-status error';
                status.textContent = 'Feature package is larger than 20 MB. / 特征库压缩包不能超过 20 MB。';
                return;
            }

            var form = new FormData();
            form.append('feature_file', file, file.name);
            button.disabled = true;
            status.style.display = 'block';
            status.className = 'feature-status';
            status.textContent = 'Uploading and applying... / 正在上传并应用...';

            var base = window.location.pathname.replace(/\/$/, '');
            fetch(base + '/manual_upload', {
                method: 'POST',
                body: form,
                credentials: 'same-origin'
            }).then(function(response) {
                return response.json();
            }).then(function(result) {
                if (result && result.code === 2000) {
                    status.className = 'feature-status success';
                    status.textContent = result.data && result.data.message ? result.data.message : 'Feature library updated successfully. / 特征库更新成功。';
                    window.setTimeout(function() { window.location.reload(); }, 1200);
                } else {
                    status.className = 'feature-status error';
                    status.textContent = result && result.data && result.data.error ? result.data.error : 'Feature library update failed. / 特征库更新失败。';
                }
            }).catch(function() {
                status.className = 'feature-status error';
                status.textContent = 'Upload request failed. / 上传请求失败。';
            }).then(function() {
                button.disabled = false;
            });
        });
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initManualFeatureUpload);
    } else {
        initManualFeatureUpload();
    }
})(window);

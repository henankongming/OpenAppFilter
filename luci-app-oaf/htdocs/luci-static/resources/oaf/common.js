function showCommonModal(message, duration = 2000, type = 'info') {
    const existingModal = document.querySelector('.common-modal-mask');
    if (existingModal) {
        existingModal.remove();
    }

    const modalMask = document.createElement('div');
    modalMask.className = 'common-modal-mask';
    
    const modalContent = document.createElement('div');
    modalContent.className = 'common-modal-content';
    
    modalContent.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
    modalContent.style.color = 'white';
    
    modalContent.textContent = message;
    
    modalMask.appendChild(modalContent);
    document.body.appendChild(modalMask);
    
    setTimeout(() => {
        modalMask.classList.add('show');
    }, 10);
    
    setTimeout(() => {
        modalMask.classList.remove('show');
        setTimeout(() => {
            if (modalMask.parentNode) {
                modalMask.remove();
            }
        }, 300);
    }, duration);
}

function showSuccess(message, duration = 2000) {
    showCommonModal(message, duration);
}

function showWarning(message, duration = 2000) {
    showCommonModal(message, duration);
}

function showError(message, duration = 3000) {
    showCommonModal(message, duration);
}

function showInfo(message, duration = 2000) {
    showCommonModal(message, duration);
}

function showConfirmModal(message, onConfirm, onCancel, confirmText = '确定', cancelText = '取消') {
    const existingModal = document.querySelector('.common-modal-mask');
    if (existingModal) {
        existingModal.remove();
    }

    const modalMask = document.createElement('div');
    modalMask.className = 'common-modal-mask';
    
    const modalContent = document.createElement('div');
    modalContent.className = 'common-modal-content';
    modalContent.style.width = '300px';
    modalContent.style.height = 'auto';
    modalContent.style.minHeight = '120px';
    modalContent.style.padding = '20px';
    modalContent.style.flexDirection = 'column';
    modalContent.style.justifyContent = 'space-between';
    modalContent.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
    modalContent.style.color = 'white';
    
    const messageDiv = document.createElement('div');
    messageDiv.textContent = message;
    messageDiv.style.marginBottom = '20px';
    messageDiv.style.textAlign = 'center';
    
    const buttonContainer = document.createElement('div');
    buttonContainer.style.display = 'flex';
    buttonContainer.style.justifyContent = 'center';
    buttonContainer.style.gap = '10px';
    
    const confirmBtn = document.createElement('button');
    confirmBtn.textContent = confirmText;
    confirmBtn.style.padding = '8px 16px';
    confirmBtn.style.backgroundColor = '#2885e8';
    confirmBtn.style.color = 'white';
    confirmBtn.style.border = 'none';
    confirmBtn.style.borderRadius = '4px';
    confirmBtn.style.cursor = 'pointer';
    confirmBtn.onclick = () => {
        modalMask.remove();
        if (onConfirm) onConfirm();
    };
    
    const cancelBtn = document.createElement('button');
    cancelBtn.textContent = cancelText;
    cancelBtn.style.padding = '8px 16px';
    cancelBtn.style.backgroundColor = '#666';
    cancelBtn.style.color = 'white';
    cancelBtn.style.border = 'none';
    cancelBtn.style.borderRadius = '4px';
    cancelBtn.style.cursor = 'pointer';
    cancelBtn.onclick = () => {
        modalMask.remove();
        if (onCancel) onCancel();
    };
    
    buttonContainer.appendChild(confirmBtn);
    buttonContainer.appendChild(cancelBtn);
    
    modalContent.appendChild(messageDiv);
    modalContent.appendChild(buttonContainer);
    modalMask.appendChild(modalContent);
    document.body.appendChild(modalMask);
    
    setTimeout(() => {
        modalMask.classList.add('show');
    }, 10);
}

function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function debounce(func, wait) {
    let timeout;
    return function executedFunction(...args) {
        const later = () => {
            clearTimeout(timeout);
            func(...args);
        };
        clearTimeout(timeout);
        timeout = setTimeout(later, wait);
    };
}

function throttle(func, limit) {
    return function() {
        const args = arguments;
        const context = this;
        if (!this.inThrottle) {
            func.apply(context, args);
            this.inThrottle = true;
            setTimeout(() => this.inThrottle = false, limit);
        }
    };
}

function getUrlParameter(name) {
    const urlParams = new URLSearchParams(window.location.search);
    return urlParams.get(name);
}

function setUrlParameter(name, value) {
    const url = new URL(window.location);
    url.searchParams.set(name, value);
    window.history.replaceState({}, '', url);
}

function deepClone(obj) {
    if (obj === null || typeof obj !== 'object') return obj;
    if (obj instanceof Date) return new Date(obj.getTime());
    if (obj instanceof Array) return obj.map(item => deepClone(item));
    if (typeof obj === 'object') {
        const clonedObj = {};
        for (const key in obj) {
            if (obj.hasOwnProperty(key)) {
                clonedObj[key] = deepClone(obj[key]);
            }
        }
        return clonedObj;
    }
}

function validateIP(ip) {
    if (!ip || ip.trim() === '') return true; 
    const ipRegex = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    return ipRegex.test(ip.trim());
}

function validateNetmask(mask) {
    if (!mask || mask.trim() === '') return true; 
    const maskRegex = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    if (!maskRegex.test(mask.trim())) return false;
    const parts = mask.split('.');
    const binary = parts.map(p => parseInt(p).toString(2).padStart(8, '0')).join('');
    return /^1+0*$/.test(binary);
}

function initManualFeatureUpload() {
    if (document.getElementById('oaf-manual-feature-upload') || !document.getElementById('tab-online')) {
        return;
    }

    const panel = document.querySelector('#tab-online .online-control-stack');
    if (!panel) return;

    const wrapper = document.createElement('div');
    wrapper.id = 'oaf-manual-feature-upload';
    wrapper.style.marginTop = '14px';
    wrapper.style.paddingTop = '14px';
    wrapper.style.borderTop = '1px solid var(--border-color-low,#e5e7eb)';

    wrapper.innerHTML = `
        <div class="online-control-row">
            <label for="manual-feature-file">Manual Upload / 手动上传</label>
            <input type="file" id="manual-feature-file" accept=".bin,.tar.gz,.tgz" style="max-width:360px;">
            <div class="online-actions">
                <button type="button" class="cbi-button cbi-button-apply" id="manual-feature-upload-btn">Upload & Apply / 上传并应用</button>
            </div>
        </div>
        <div class="feature-hint">Upload the official feature package (.tar.gz/.tgz, including feature.bin). The existing library is backed up before replacement. / 上传官方特征库压缩包（.tar.gz/.tgz，内含 feature.bin），替换前会自动备份当前特征库。</div>
        <div id="manual-feature-upload-status" class="feature-status" style="display:none;"></div>
    `;

    panel.appendChild(wrapper);

    const input = document.getElementById('manual-feature-file');
    const button = document.getElementById('manual-feature-upload-btn');
    const status = document.getElementById('manual-feature-upload-status');

    button.addEventListener('click', async () => {
        const file = input.files && input.files[0];
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

        const form = new FormData();
        form.append('feature_file', file, file.name);
        button.disabled = true;
        status.style.display = 'block';
        status.className = 'feature-status';
        status.textContent = 'Uploading and applying... / 正在上传并应用...';

        try {
            const base = window.location.pathname.replace(/\/$/, '');
            const response = await fetch(base + '/manual_upload', {
                method: 'POST',
                body: form,
                credentials: 'same-origin'
            });
            const result = await response.json();
            if (result && result.code === 2000) {
                status.className = 'feature-status success';
                status.textContent = result.data && result.data.message
                    ? result.data.message
                    : 'Feature library updated successfully. / 特征库更新成功。';
                setTimeout(() => window.location.reload(), 1200);
            } else {
                status.className = 'feature-status error';
                status.textContent = (result && result.data && result.data.error)
                    ? result.data.error
                    : 'Feature library update failed. / 特征库更新失败。';
            }
        } catch (e) {
            status.className = 'feature-status error';
            status.textContent = 'Upload request failed. / 上传请求失败。';
        } finally {
            button.disabled = false;
        }
    });
}

window.showCommonModal = showCommonModal;
window.showSuccess = showSuccess;
window.showWarning = showWarning;
window.showError = showError;
window.showInfo = showInfo;
window.showConfirmModal = showConfirmModal;
window.formatFileSize = formatFileSize;
window.debounce = debounce;
window.throttle = throttle;
window.getUrlParameter = getUrlParameter;
window.setUrlParameter = setUrlParameter;
window.deepClone = deepClone;
window.validateIP = validateIP;
window.validateNetmask = validateNetmask;

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initManualFeatureUpload);
} else {
    initManualFeatureUpload();
}

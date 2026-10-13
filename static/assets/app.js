/* ==========================================================================
   匿名意见箱 — 提交页脚本
   约束：服务端文本一律用 textContent 写入；不写浏览器持久化存储；
        所有 fetch 使用 credentials: 'omit' 与同源相对路径。
   ========================================================================== */
(function () {
  'use strict';

  var API = '/api/v1';
  var DEFAULT_MAX = 5000;
  var maxChars = DEFAULT_MAX;

  var els = {
    form: document.getElementById('feedback-form'),
    section: document.getElementById('form-section'),
    content: document.getElementById('content'),
    counter: document.getElementById('content-counter'),
    category: document.getElementById('category'),
    submit: document.getElementById('submit-btn'),
    error: document.getElementById('form-error'),
    receipt: document.getElementById('receipt'),
    receiptId: document.getElementById('receipt-id'),
    receiptTime: document.getElementById('receipt-time'),
    copy: document.getElementById('copy-btn'),
    again: document.getElementById('again-btn'),
    copyStatus: document.getElementById('copy-status')
  };

  var lastReceiptId = '';

  /* ---------------------------------------------------------------- 工具 */

  function setText(node, text) {
    if (!node) { return; }
    while (node.firstChild) { node.removeChild(node.firstChild); }
    node.appendChild(document.createTextNode(text == null ? '' : String(text)));
  }

  function showError(message) {
    setText(els.error, message);
    els.error.hidden = !message;
  }

  function clearError() {
    showError('');
  }

  function setBusy(busy) {
    els.submit.disabled = busy;
    setText(els.submit, busy ? '提交中…' : '匿名提交');
  }

  /* ------------------------------------------------------------ 字符计数 */

  function updateCounter() {
    var len = els.content.value.length;
    setText(els.counter, '已输入 ' + len + ' / ' + maxChars);
    els.counter.classList.toggle('at-limit', len >= maxChars);
  }

  /* ------------------------------------------------------------ 本地校验 */

  function validateLocal() {
    var value = els.content.value;
    if (value.trim().length === 0) {
      showError('请输入内容后再提交。');
      els.content.focus();
      return false;
    }
    if (value.length > maxChars) {
      showError('内容超过 ' + maxChars + ' 字符，请精简后重试。');
      els.content.focus();
      return false;
    }
    return true;
  }

  /* ---------------------------------------------------------------- 网络 */

  function readJson(res) {
    return res.text().then(function (text) {
      if (!text) { return null; }
      try {
        return JSON.parse(text);
      } catch (err) {
        return null;
      }
    });
  }

  function errorMessageFor(status, data) {
    if (status === 429) {
      return '提交过于频繁，请稍后再试。';
    }
    if (data && data.error && data.error.message) {
      return data.error.message;
    }
    if (status === 413) { return '内容过长，服务器拒绝接收。'; }
    if (status === 415) { return '请求格式不受支持。'; }
    return '提交失败，请稍后重试。';
  }

  function submitFeedback(event) {
    event.preventDefault();
    clearError();
    if (!validateLocal()) { return; }

    var payload = { content: els.content.value };
    var category = els.category.value;
    payload.category = category === '' ? null : category;

    setBusy(true);

    fetch(API + '/feedback', {
      method: 'POST',
      credentials: 'omit',
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json'
      },
      body: JSON.stringify(payload)
    })
      .then(function (res) {
        return readJson(res).then(function (data) {
          return { status: res.status, ok: res.ok, data: data };
        });
      })
      .then(function (result) {
        setBusy(false);
        if (!result.ok) {
          showError(errorMessageFor(result.status, result.data));
          return;
        }
        showReceipt(result.data);
      })
      .catch(function () {
        setBusy(false);
        showError('网络请求失败，请检查网络后重试。');
      });
  }

  /* ------------------------------------------------------------ 回执面板 */

  function showReceipt(data) {
    var id = data && data.id ? String(data.id) : '';
    var bucket = data && data.created_at_bucket ? String(data.created_at_bucket) : '';

    lastReceiptId = id;
    setText(els.receiptId, id);
    setText(els.receiptTime, bucket ? '提交时间（UTC 小时）：' + bucket : '');
    setText(els.copyStatus, '');
    els.section.hidden = true;
    els.receipt.hidden = false;
    els.receipt.scrollIntoView({ block: 'start' });
    els.copy.focus();
  }

  function resetForm() {
    els.form.reset();
    els.content.value = '';
    els.category.value = '';
    clearError();
    updateCounter();
    lastReceiptId = '';
    setText(els.receiptId, '');
    setText(els.receiptTime, '');
    setText(els.copyStatus, '');
    els.receipt.hidden = true;
    els.section.hidden = false;
    els.content.focus();
  }

  /* -------------------------------------------------------------- 复制 */

  function legacyCopy(text) {
    var helper = document.createElement('textarea');
    helper.className = 'copy-helper';
    helper.value = text;
    helper.setAttribute('readonly', 'readonly');
    helper.setAttribute('aria-hidden', 'true');
    document.body.appendChild(helper);
    helper.select();
    helper.setSelectionRange(0, text.length);
    var ok = false;
    try {
      ok = document.execCommand('copy');
    } catch (err) {
      ok = false;
    }
    document.body.removeChild(helper);
    return ok;
  }

  function selectReceiptText() {
    var node = els.receiptId;
    if (!node || !window.getSelection || !document.createRange) { return; }
    var range = document.createRange();
    range.selectNodeContents(node);
    var selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
  }

  function copyReceipt() {
    if (!lastReceiptId) { return; }

    if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
      navigator.clipboard.writeText(lastReceiptId).then(function () {
        setText(els.copyStatus, '已复制到剪贴板。');
      }).catch(function () {
        if (legacyCopy(lastReceiptId)) {
          setText(els.copyStatus, '已复制到剪贴板。');
        } else {
          selectReceiptText();
          setText(els.copyStatus, '自动复制不可用，回执 ID 已选中，请手动复制。');
        }
      });
      return;
    }

    if (legacyCopy(lastReceiptId)) {
      setText(els.copyStatus, '已复制到剪贴板。');
    } else {
      selectReceiptText();
      setText(els.copyStatus, '自动复制不可用，回执 ID 已选中，请手动复制。');
    }
  }

  /* -------------------------------------------------------------- 元数据 */

  function loadMeta() {
    fetch(API + '/meta', {
      method: 'GET',
      credentials: 'omit',
      headers: { 'Accept': 'application/json' }
    })
      .then(function (res) {
        if (!res.ok) { return null; }
        return readJson(res);
      })
      .then(function (data) {
        var max = data && typeof data.content_max_chars === 'number' ? data.content_max_chars : 0;
        if (max > 0) {
          maxChars = max;
          els.content.maxLength = max;
        }
        updateCounter();
      })
      .catch(function () {
        updateCounter();
      });
  }

  /* ---------------------------------------------------------------- 绑定 */

  if (!els.form) { return; }

  els.form.addEventListener('submit', submitFeedback);
  els.content.addEventListener('input', function () {
    clearError();
    updateCounter();
  });
  els.copy.addEventListener('click', copyReceipt);
  els.again.addEventListener('click', resetForm);

  updateCounter();
  loadMeta();
})();

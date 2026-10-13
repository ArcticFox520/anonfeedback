(function () {
  'use strict';

  var API = '/api/v1';
  var DEFAULT_MAX = 5000;
  var maxChars = DEFAULT_MAX;

  var els = {
    form: document.getElementById('feedback-form'),
    compose: document.getElementById('compose-section'),
    content: document.getElementById('content'),
    counter: document.getElementById('compose-counter'),
    category: document.getElementById('category'),
    submit: document.getElementById('submit-btn'),
    error: document.getElementById('form-error'),
    receipt: document.getElementById('receipt'),
    receiptId: document.getElementById('receipt-id'),
    copy: document.getElementById('copy-btn'),
    again: document.getElementById('again-btn'),
    copyStatus: document.getElementById('copy-status')
  };

  var lastReceiptId = '';
  var copyTimer = 0;

  function setText(node, text) {
    if (node) { node.textContent = text == null ? '' : String(text); }
  }

  function setNotice(node, message) {
    if (!node) { return; }
    setText(node, message || '');
    node.hidden = !message;
  }

  function readJson(res) {
    return res.text().then(function (text) {
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (err) { return null; }
    });
  }

  function updateCounter() {
    var len = els.content.value.length;
    setText(els.counter, len + ' / ' + maxChars);
    els.counter.classList.toggle('is-limit', len >= maxChars);
  }

  function syncActive() {
    els.compose.classList.toggle('is-active', els.content.value.length > 0);
  }

  function validate() {
    var value = els.content.value;
    if (value.trim().length === 0) {
      setNotice(els.error, '还没有写内容');
      els.content.focus();
      return false;
    }
    if (value.length > maxChars) {
      setNotice(els.error, '内容超过 ' + maxChars + ' 字');
      els.content.focus();
      return false;
    }
    return true;
  }

  function setBusy(busy) {
    els.submit.disabled = busy;
    setText(els.submit, busy ? '提交中…' : '提交');
  }

  function messageFor(status, data) {
    if (status === 429) { return '提交太频繁，稍后再试'; }
    if (data && data.error && data.error.message) { return data.error.message; }
    if (status === 413) { return '内容过长'; }
    return '提交失败，请重试';
  }

  function onSubmit(event) {
    event.preventDefault();
    setNotice(els.error, '');
    if (!validate()) { return; }

    var category = els.category.value;
    setBusy(true);

    fetch(API + '/feedback', {
      method: 'POST',
      credentials: 'omit',
      headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
      body: JSON.stringify({
        content: els.content.value,
        category: category === '' ? null : category
      })
    })
      .then(function (res) {
        return readJson(res).then(function (data) {
          return { status: res.status, ok: res.ok, data: data };
        });
      })
      .then(function (result) {
        setBusy(false);
        if (!result.ok) {
          setNotice(els.error, messageFor(result.status, result.data));
          return;
        }
        showReceipt(result.data);
      })
      .catch(function () {
        setBusy(false);
        setNotice(els.error, '网络异常，请重试');
      });
  }

  function showReceipt(data) {
    var id = data && data.id ? String(data.id) : '';

    lastReceiptId = id;
    setText(els.receiptId, id);
    setNotice(els.copyStatus, '');
    setText(els.copy, '复制编号');

    els.compose.classList.remove('is-active');
    els.compose.hidden = true;
    els.receipt.hidden = false;
    window.scrollTo({ top: 0 });
    els.copy.focus();
  }

  function resetForm() {
    els.form.reset();
    els.content.value = '';
    els.category.value = '';
    setNotice(els.error, '');
    lastReceiptId = '';
    setText(els.receiptId, '');
    setNotice(els.copyStatus, '');
    els.receipt.hidden = true;
    els.compose.hidden = false;
    updateCounter();
    syncActive();
    els.content.focus();
  }

  function fallbackCopy(text) {
    var helper = document.createElement('textarea');
    helper.className = 'copy-helper';
    helper.value = text;
    helper.setAttribute('readonly', 'readonly');
    helper.setAttribute('aria-hidden', 'true');
    document.body.appendChild(helper);
    helper.select();
    helper.setSelectionRange(0, text.length);
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (err) { ok = false; }
    document.body.removeChild(helper);
    return ok;
  }

  function selectReceiptId() {
    if (!window.getSelection || !document.createRange) { return; }
    var range = document.createRange();
    range.selectNodeContents(els.receiptId);
    var selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
  }

  function copied() {
    if (copyTimer) { window.clearTimeout(copyTimer); }
    setText(els.copy, '已复制');
    setNotice(els.copyStatus, '');
    copyTimer = window.setTimeout(function () {
      setText(els.copy, '复制编号');
    }, 1600);
  }

  function copyReceipt() {
    if (!lastReceiptId) { return; }

    if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
      navigator.clipboard.writeText(lastReceiptId).then(copied).catch(function () {
        if (fallbackCopy(lastReceiptId)) {
          copied();
        } else {
          selectReceiptId();
          setNotice(els.copyStatus, '请手动复制选中的编号');
        }
      });
      return;
    }

    if (fallbackCopy(lastReceiptId)) {
      copied();
    } else {
      selectReceiptId();
      setNotice(els.copyStatus, '请手动复制选中的编号');
    }
  }

  function loadMeta() {
    fetch(API + '/meta', {
      method: 'GET',
      credentials: 'omit',
      headers: { 'Accept': 'application/json' }
    })
      .then(function (res) { return res.ok ? readJson(res) : null; })
      .then(function (data) {
        var max = data && typeof data.content_max_chars === 'number' ? data.content_max_chars : 0;
        if (max > 0) {
          maxChars = max;
          els.content.maxLength = max;
        }
        updateCounter();
      })
      .catch(function () { updateCounter(); });
  }

  els.form.addEventListener('submit', onSubmit);
  els.content.addEventListener('input', function () {
    setNotice(els.error, '');
    updateCounter();
    syncActive();
  });
  els.copy.addEventListener('click', copyReceipt);
  els.again.addEventListener('click', resetForm);

  updateCounter();
  loadMeta();
})();

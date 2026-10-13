(function () {
  'use strict';

  var API = '/api/v1';
  var TOKEN_KEY = 'anonfb_admin_token';
  var PAGE_SIZE = 20;
  var MAX_NOTE = 2000;

  var CATEGORY_LABELS = {
    bug: '缺陷',
    suggestion: '建议',
    complaint: '投诉',
    other: '其他'
  };

  var STATUS_LABELS = {
    new: '未读',
    read: '已读',
    archived: '已归档'
  };

  var state = {
    token: '',
    items: [],
    total: 0,
    offset: 0,
    filter: '',
    selectedId: null
  };

  var els = {
    admin: document.getElementById('admin'),
    gate: document.getElementById('gate'),
    gateForm: document.getElementById('gate-form'),
    gateBtn: document.getElementById('gate-btn'),
    gateError: document.getElementById('gate-error'),
    token: document.getElementById('token'),

    workspace: document.getElementById('workspace'),
    refresh: document.getElementById('refresh-btn'),
    logout: document.getElementById('logout-btn'),

    filter: document.getElementById('status-filter'),
    listStatus: document.getElementById('list-status'),
    totalCount: document.getElementById('total-count'),
    list: document.getElementById('list'),
    empty: document.getElementById('empty-state'),

    pager: document.getElementById('pager'),
    prev: document.getElementById('prev-btn'),
    next: document.getElementById('next-btn'),
    pagerLabel: document.getElementById('pager-label'),

    detailEmpty: document.getElementById('detail-empty'),
    detail: document.getElementById('detail'),
    back: document.getElementById('back-btn'),
    detailId: document.getElementById('detail-id'),
    detailContent: document.getElementById('detail-content'),
    detailCreated: document.getElementById('detail-created'),
    detailUpdated: document.getElementById('detail-updated'),
    detailStatus: document.getElementById('detail-status'),
    detailNote: document.getElementById('detail-note'),
    noteCounter: document.getElementById('note-counter'),
    saveNote: document.getElementById('save-note-btn'),
    clearNote: document.getElementById('clear-note-btn'),
    statusButtons: document.querySelectorAll('.status-group .btn[data-status]'),
    deleteBtn: document.getElementById('delete-btn'),
    deleteConfirm: document.getElementById('delete-confirm'),
    deleteConfirmBtn: document.getElementById('delete-confirm-btn'),
    deleteCancelBtn: document.getElementById('delete-cancel-btn')
  };

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

  function ApiError(code, message, status) {
    this.name = 'ApiError';
    this.code = code;
    this.message = message;
    this.status = status;
  }
  ApiError.prototype = Object.create(Error.prototype);

  function messageOf(err) {
    return err && err.message ? err.message : '请求失败';
  }

  function readToken() {
    try { return window.sessionStorage.getItem(TOKEN_KEY) || ''; } catch (err) { return ''; }
  }

  function storeToken(token) {
    try { window.sessionStorage.setItem(TOKEN_KEY, token); } catch (err) {  }
  }

  function clearToken() {
    try { window.sessionStorage.removeItem(TOKEN_KEY); } catch (err) {  }
  }

  function handleUnauthorized() {
    clearToken();
    state.token = '';
    closeDetail();
    closeWorkspace();
    showGate('登录已失效，请重新输入');
  }

  function api(path, options) {
    var opts = options || {};
    var headers = { 'Accept': 'application/json' };
    if (opts.body !== undefined) { headers['Content-Type'] = 'application/json'; }
    if (state.token) { headers['Authorization'] = 'Bearer ' + state.token; }

    return fetch(API + path, {
      method: opts.method || 'GET',
      credentials: 'omit',
      headers: headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body)
    }).then(function (res) {
      if (res.status === 401) {
        handleUnauthorized();
        throw new ApiError('UNAUTHORIZED', '登录已失效，请重新输入', 401);
      }
      if (res.status === 204) { return null; }
      return readJson(res).then(function (data) {
        if (!res.ok) {
          var code = data && data.error && data.error.code ? data.error.code : 'HTTP_' + res.status;
          var message = data && data.error && data.error.message
            ? data.error.message
            : '请求失败（' + res.status + '）';
          throw new ApiError(code, message, res.status);
        }
        return data;
      });
    });
  }

  function showGate(message) {
    els.gate.hidden = false;
    setNotice(els.gateError, message || '');
    setText(els.gateBtn, '进入');
    els.gateBtn.disabled = false;
    els.refresh.hidden = true;
    els.logout.hidden = true;
    if (els.token) { els.token.focus(); }
  }

  function closeWorkspace() {
    els.workspace.hidden = true;
    els.list.textContent = '';
    state.items = [];
    state.total = 0;
    state.offset = 0;
    setNotice(els.listStatus, '');
  }

  function showWorkspace() {
    els.gate.hidden = true;
    setNotice(els.gateError, '');
    els.workspace.hidden = false;
    els.refresh.hidden = false;
    els.logout.hidden = false;
  }

  function openDetail() {
    els.detailEmpty.hidden = true;
    els.detail.hidden = false;
    els.admin.classList.add('has-selection');
  }

  function closeDetail() {
    state.selectedId = null;
    els.detail.hidden = true;
    els.detailEmpty.hidden = false;
    els.admin.classList.remove('has-selection');
    resetDeleteConfirm();
  }

  function excerptOf(content) {
    var flat = typeof content === 'string' ? content.replace(/\s+/g, ' ').trim() : '';
    return flat.length <= 80 ? flat : flat.slice(0, 80) + '…';
  }

  function buildItem(item) {
    var li = document.createElement('li');

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'item';
    btn.setAttribute('data-status', item.status);
    btn.setAttribute('data-id', item.id);
    if (item.id === state.selectedId) {
      btn.classList.add('is-selected');
      btn.setAttribute('aria-current', 'true');
    }

    var dot = document.createElement('span');
    dot.className = 'item-dot';
    dot.setAttribute('aria-hidden', 'true');

    var main = document.createElement('span');
    main.className = 'item-main';

    var excerpt = document.createElement('span');
    excerpt.className = 'item-excerpt';
    excerpt.textContent = excerptOf(item.content);

    var meta = document.createElement('span');
    meta.className = 'item-meta';

    var cat = document.createElement('span');
    cat.textContent = CATEGORY_LABELS[item.category] || '未分类';

    var time = document.createElement('span');
    time.className = 'mono';
    time.textContent = item.created_at_bucket || '';

    meta.appendChild(cat);
    meta.appendChild(time);
    main.appendChild(excerpt);
    main.appendChild(meta);

    var status = document.createElement('span');
    status.className = 'item-status';
    status.textContent = STATUS_LABELS[item.status] || '';

    btn.appendChild(dot);
    btn.appendChild(main);
    btn.appendChild(status);
    btn.addEventListener('click', function () { openItem(item.id); });

    li.appendChild(btn);
    return li;
  }

  function renderList() {
    els.list.textContent = '';
    state.items.forEach(function (item) {
      els.list.appendChild(buildItem(item));
    });

    els.empty.hidden = state.items.length > 0;
    setText(els.totalCount, state.total + ' 条');

    var pages = Math.max(1, Math.ceil(state.total / PAGE_SIZE));
    var page = Math.floor(state.offset / PAGE_SIZE) + 1;
    if (page > pages) { page = pages; }

    setText(els.pagerLabel, page + ' / ' + pages);
    els.prev.disabled = state.offset <= 0;
    els.next.disabled = state.offset + PAGE_SIZE >= state.total;
    els.pager.hidden = state.total <= PAGE_SIZE;
  }

  function listQuery() {
    var parts = ['limit=' + PAGE_SIZE, 'offset=' + state.offset];
    if (state.filter) { parts.push('status=' + encodeURIComponent(state.filter)); }
    return '/admin/feedback?' + parts.join('&');
  }

  function loadList(message) {
    setNotice(els.listStatus, message || '载入中…');
    return api(listQuery())
      .then(function (data) {
        state.items = data && Array.isArray(data.items) ? data.items : [];
        state.total = data && typeof data.total === 'number' ? data.total : state.items.length;

        if (state.items.length === 0 && state.offset > 0) {
          state.offset = Math.max(0, state.offset - PAGE_SIZE);
          return loadList(message);
        }

        renderList();
        setNotice(els.listStatus, message || '');
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setNotice(els.listStatus, messageOf(err));
      });
  }

  function renderDetail(item) {
    setText(els.detailId, item.id || '');
    setText(els.detailContent, item.content || '');
    setText(els.detailCreated, item.created_at_bucket || '');
    setText(els.detailUpdated, item.updated_at || '—');

    els.detailNote.value = item.admin_note == null ? '' : String(item.admin_note);
    updateNoteCounter();

    Array.prototype.forEach.call(els.statusButtons, function (btn) {
      btn.setAttribute('aria-pressed',
        btn.getAttribute('data-status') === item.status ? 'true' : 'false');
    });

    setNotice(els.detailStatus, '');
    resetDeleteConfirm();
    openDetail();
  }

  function openItem(id) {
    if (!id) { return; }
    state.selectedId = id;
    setNotice(els.detailStatus, '');
    openDetail();
    renderList();

    api('/admin/feedback/' + encodeURIComponent(id))
      .then(function (item) {
        renderDetail(item);
        els.detail.scrollIntoView({ block: 'nearest' });
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        if (err && err.status === 404) {
          closeDetail();
          loadList('该条已不存在');
          return;
        }
        setNotice(els.detailStatus, messageOf(err));
      });
  }

  function updateNoteCounter() {
    var len = els.detailNote.value.length;
    setText(els.noteCounter, len + ' / ' + MAX_NOTE);
    els.noteCounter.classList.toggle('is-limit', len >= MAX_NOTE);
  }

  function afterMutation(item, message) {
    if (item) { renderDetail(item); }
    return loadList(message);
  }

  function changeStatus(status) {
    if (!state.selectedId) { return; }
    setNotice(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { status: status }
    })
      .then(function (item) { return afterMutation(item, ''); })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setNotice(els.detailStatus, messageOf(err));
      });
  }

  function saveNote() {
    if (!state.selectedId) { return; }
    var value = els.detailNote.value.trim();
    if (value.length === 0) {
      setNotice(els.detailStatus, '备注为空，可点「清空」');
      return;
    }
    if (value.length > MAX_NOTE) {
      setNotice(els.detailStatus, '备注超过 ' + MAX_NOTE + ' 字');
      return;
    }
    setNotice(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { admin_note: value }
    })
      .then(function (item) { return afterMutation(item, '已保存'); })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setNotice(els.detailStatus, messageOf(err));
      });
  }

  function clearNote() {
    if (!state.selectedId) { return; }
    setNotice(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { admin_note: null }
    })
      .then(function (item) { return afterMutation(item, '已清空'); })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setNotice(els.detailStatus, messageOf(err));
      });
  }

  function resetDeleteConfirm() {
    els.deleteBtn.hidden = false;
    els.deleteConfirm.hidden = true;
  }

  function requestDelete() {
    els.deleteBtn.hidden = true;
    els.deleteConfirm.hidden = false;
    els.deleteConfirmBtn.focus();
  }

  function cancelDelete() {
    resetDeleteConfirm();
    els.deleteBtn.focus();
  }

  function confirmDelete() {
    if (!state.selectedId) { return; }
    var id = state.selectedId;
    els.deleteConfirmBtn.disabled = true;
    api('/admin/feedback/' + encodeURIComponent(id), { method: 'DELETE' })
      .then(function () {
        els.deleteConfirmBtn.disabled = false;
        closeDetail();
        els.list.focus();
        return loadList('已删除');
      })
      .catch(function (err) {
        els.deleteConfirmBtn.disabled = false;
        if (err && err.status === 401) { return; }
        resetDeleteConfirm();
        setNotice(els.detailStatus, messageOf(err));
      });
  }

  function submitGate(event) {
    event.preventDefault();
    var token = els.token.value.trim();

    if (!token) {
      setNotice(els.gateError, '请输入令牌');
      els.token.focus();
      return;
    }

    els.gateBtn.disabled = true;
    setText(els.gateBtn, '验证中…');
    setNotice(els.gateError, '');
    state.token = token;

    api('/admin/feedback?limit=1')
      .then(function () {
        storeToken(token);
        state.offset = 0;
        els.token.value = '';
        showWorkspace();
        return loadList();
      })
      .catch(function (err) {
        state.token = '';
        setText(els.gateBtn, '进入');
        els.gateBtn.disabled = false;
        setNotice(els.gateError, err && err.status === 401 ? '令牌无效' : messageOf(err));
        els.token.focus();
      });
  }

  function logout() {
    clearToken();
    state.token = '';
    state.filter = '';
    els.filter.value = '';
    closeDetail();
    closeWorkspace();
    showGate('');
  }

  els.gateForm.addEventListener('submit', submitGate);
  els.logout.addEventListener('click', logout);
  els.refresh.addEventListener('click', function () { loadList('已刷新'); });

  els.filter.addEventListener('change', function () {
    state.filter = els.filter.value;
    state.offset = 0;
    loadList();
  });

  els.prev.addEventListener('click', function () {
    if (state.offset <= 0) { return; }
    state.offset = Math.max(0, state.offset - PAGE_SIZE);
    loadList();
  });

  els.next.addEventListener('click', function () {
    if (state.offset + PAGE_SIZE >= state.total) { return; }
    state.offset += PAGE_SIZE;
    loadList();
  });

  Array.prototype.forEach.call(els.statusButtons, function (btn) {
    btn.addEventListener('click', function () {
      changeStatus(btn.getAttribute('data-status'));
    });
  });

  els.detailNote.addEventListener('input', updateNoteCounter);
  els.saveNote.addEventListener('click', saveNote);
  els.clearNote.addEventListener('click', clearNote);
  els.back.addEventListener('click', function () {
    closeDetail();
    renderList();
  });
  els.deleteBtn.addEventListener('click', requestDelete);
  els.deleteCancelBtn.addEventListener('click', cancelDelete);
  els.deleteConfirmBtn.addEventListener('click', confirmDelete);

  var saved = readToken();
  if (saved) {
    state.token = saved;
    showWorkspace();
    loadList();
  } else {
    showGate('');
  }
})();

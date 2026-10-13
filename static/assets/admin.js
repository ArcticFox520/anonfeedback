/* ==========================================================================
   匿名意见箱 — 管理端脚本
   约束：服务端文本一律用 textContent 写入；令牌只存 sessionStorage('anonfb_admin_token')；
        所有 fetch 使用 credentials: 'omit' 与同源相对路径；
        任何接口返回 401 都清除令牌并退回闸门。
   ========================================================================== */
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

  var STATUS_CODES = {
    new: 'NEW',
    read: 'READ',
    archived: 'ARCHIVED'
  };

  var state = {
    token: '',
    items: [],
    total: 0,
    offset: 0,
    filter: '',
    selectedId: null,
    detail: null
  };

  var els = {
    gate: document.getElementById('gate'),
    gateForm: document.getElementById('gate-form'),
    gateBtn: document.getElementById('gate-btn'),
    gateError: document.getElementById('gate-error'),
    tokenInput: document.getElementById('token'),

    console: document.getElementById('console'),
    filter: document.getElementById('status-filter'),
    refresh: document.getElementById('refresh-btn'),
    logout: document.getElementById('logout-btn'),
    consoleStatus: document.getElementById('console-status'),
    totalCount: document.getElementById('total-count'),
    table: document.getElementById('feedback-table'),
    listBody: document.getElementById('list-body'),
    emptyState: document.getElementById('empty-state'),

    pagination: document.getElementById('pagination'),
    prev: document.getElementById('prev-btn'),
    next: document.getElementById('next-btn'),
    pageLabel: document.getElementById('page-label'),

    detailEmpty: document.getElementById('detail-empty'),
    detail: document.getElementById('detail'),
    detailId: document.getElementById('detail-id'),
    detailContent: document.getElementById('detail-content'),
    detailCreated: document.getElementById('detail-created'),
    detailUpdated: document.getElementById('detail-updated'),
    detailStatusText: document.getElementById('detail-status-text'),
    detailCategory: document.getElementById('detail-category'),
    detailStatus: document.getElementById('detail-status'),
    detailNote: document.getElementById('detail-note'),
    noteCounter: document.getElementById('note-counter'),
    saveNote: document.getElementById('save-note-btn'),
    clearNote: document.getElementById('clear-note-btn'),
    statusButtons: document.querySelectorAll('.status-buttons .btn[data-status]'),
    deleteBtn: document.getElementById('delete-btn'),
    deleteConfirm: document.getElementById('delete-confirm'),
    deleteConfirmBtn: document.getElementById('delete-confirm-btn'),
    deleteCancelBtn: document.getElementById('delete-cancel-btn')
  };

  /* ---------------------------------------------------------------- 工具 */

  function setText(node, text) {
    if (!node) { return; }
    while (node.firstChild) { node.removeChild(node.firstChild); }
    node.appendChild(document.createTextNode(text == null ? '' : String(text)));
  }

  function setStatusNode(node, message) {
    setText(node, message || '');
  }

  function clearChildren(node) {
    while (node.firstChild) { node.removeChild(node.firstChild); }
  }

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

  function ApiError(code, message, status) {
    this.name = 'ApiError';
    this.code = code;
    this.message = message;
    this.status = status;
  }
  ApiError.prototype = Object.create(Error.prototype);

  function messageOf(err) {
    if (err && err.message) { return err.message; }
    return '请求失败，请稍后重试。';
  }

  /* ------------------------------------------------------- sessionStorage */

  function readToken() {
    try {
      return window.sessionStorage.getItem(TOKEN_KEY) || '';
    } catch (err) {
      return '';
    }
  }

  function storeToken(token) {
    try {
      window.sessionStorage.setItem(TOKEN_KEY, token);
    } catch (err) {
      /* 隐私模式下可能不可用：仅本次会话内存中保留 */
    }
  }

  function clearToken() {
    try {
      window.sessionStorage.removeItem(TOKEN_KEY);
    } catch (err) {
      /* 忽略 */
    }
  }

  /* ---------------------------------------------------------------- 网络 */

  function handleUnauthorized() {
    clearToken();
    state.token = '';
    if (!els.console.hidden) {
      hideDetail();
      closeConsole();
      showGate('登录已失效，请重新输入令牌。');
    }
  }

  function api(path, options) {
    var opts = options || {};
    var headers = { 'Accept': 'application/json' };
    if (opts.body !== undefined) {
      headers['Content-Type'] = 'application/json';
    }
    if (state.token) {
      headers['Authorization'] = 'Bearer ' + state.token;
    }

    return fetch(API + path, {
      method: opts.method || 'GET',
      credentials: 'omit',
      headers: headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body)
    }).then(function (res) {
      if (res.status === 401) {
        handleUnauthorized();
        throw new ApiError('UNAUTHORIZED', '登录已失效，请重新输入令牌。', 401);
      }
      if (res.status === 204) {
        return null;
      }
      return readJson(res).then(function (data) {
        if (!res.ok) {
          var code = data && data.error && data.error.code ? data.error.code : 'HTTP_' + res.status;
          var message = data && data.error && data.error.message
            ? data.error.message
            : '请求失败（' + res.status + '）。';
          throw new ApiError(code, message, res.status);
        }
        return data;
      });
    });
  }

  /* ---------------------------------------------------------------- 视图 */

  function showGate(message) {
    els.gate.hidden = false;
    els.console.hidden = true;
    setStatusNode(els.gateError, message || '');
    els.gateError.hidden = !message;
    if (els.tokenInput) { els.tokenInput.focus(); }
  }

  function closeConsole() {
    els.console.hidden = true;
    clearChildren(els.listBody);
    state.items = [];
    state.total = 0;
    state.offset = 0;
  }

  function showConsole() {
    els.gate.hidden = true;
    els.console.hidden = false;
    els.gateError.hidden = true;
  }

  function hideDetail() {
    state.selectedId = null;
    state.detail = null;
    els.detail.hidden = true;
    els.detailEmpty.hidden = false;
    resetDeleteConfirm();
  }

  function setGateBusy(busy) {
    els.gateBtn.disabled = busy;
    setText(els.gateBtn, busy ? '验证中…' : '进入管理端');
  }

  /* ------------------------------------------------------------ 列表渲染 */

  function label(title, node) {
    var cell = document.createElement('td');
    cell.setAttribute('data-label', title);
    if (node) { cell.appendChild(node); }
    return cell;
  }

  function textSpan(className, text) {
    var span = document.createElement('span');
    if (className) { span.className = className; }
    span.textContent = text;
    return span;
  }

  function excerptOf(content) {
    var text = typeof content === 'string' ? content : '';
    var flat = text.replace(/\s+/g, ' ').trim();
    if (flat.length <= 80) { return flat; }
    return flat.slice(0, 80) + '…';
  }

  function statusCell(item) {
    var wrap = document.createElement('span');
    var code = document.createElement('span');
    code.className = 'status-code';
    code.setAttribute('data-status', item.status);
    code.textContent = STATUS_CODES[item.status] || String(item.status || '').toUpperCase();
    wrap.appendChild(code);
    wrap.appendChild(textSpan('status-text', STATUS_LABELS[item.status] || ''));
    return wrap;
  }

  function buildRow(item) {
    var tr = document.createElement('tr');
    tr.setAttribute('tabindex', '0');
    tr.setAttribute('data-id', item.id);

    var idText = String(item.id || '');
    var shortId = idText.slice(0, 8);
    var idSpan = textSpan('cell-id', shortId);
    idSpan.title = idText;

    var viewBtn = document.createElement('button');
    viewBtn.type = 'button';
    viewBtn.className = 'btn btn-secondary btn-sm';
    viewBtn.textContent = '查看';
    viewBtn.addEventListener('click', function (event) {
      event.stopPropagation();
      openDetail(item.id);
    });

    tr.appendChild(label('ID', idSpan));
    tr.appendChild(label('内容摘要', textSpan('cell-excerpt', excerptOf(item.content))));
    tr.appendChild(label('类别', textSpan('', CATEGORY_LABELS[item.category] || '未选择')));
    tr.appendChild(label('状态', statusCell(item)));
    tr.appendChild(label('提交时间', textSpan('cell-time', item.created_at_bucket || '')));
    tr.appendChild(label('操作', viewBtn));

    tr.addEventListener('click', function () { openDetail(item.id); });
    tr.addEventListener('keydown', function (event) {
      if (event.key === 'Enter' || event.key === ' ' || event.key === 'Spacebar') {
        event.preventDefault();
        openDetail(item.id);
      }
    });

    if (item.id === state.selectedId) {
      tr.classList.add('is-selected');
      tr.setAttribute('aria-current', 'true');
    }

    return tr;
  }

  function renderList() {
    clearChildren(els.listBody);

    state.items.forEach(function (item) {
      els.listBody.appendChild(buildRow(item));
    });

    els.emptyState.hidden = state.items.length > 0;
    els.table.hidden = state.items.length === 0;

    setText(els.totalCount, '共 ' + state.total + ' 条');

    var pages = Math.max(1, Math.ceil(state.total / PAGE_SIZE));
    var page = Math.floor(state.offset / PAGE_SIZE) + 1;
    if (page > pages) { page = pages; }

    setText(els.pageLabel, '第 ' + page + ' / ' + pages + ' 页');
    els.prev.disabled = state.offset <= 0;
    els.next.disabled = state.offset + PAGE_SIZE >= state.total;
    els.pagination.hidden = state.total <= PAGE_SIZE;
  }

  /* ---------------------------------------------------------------- 列表 */

  function listQuery() {
    var parts = ['limit=' + PAGE_SIZE, 'offset=' + state.offset];
    if (state.filter) {
      parts.push('status=' + encodeURIComponent(state.filter));
    }
    return '/admin/feedback?' + parts.join('&');
  }

  function loadList(message) {
    setStatusNode(els.consoleStatus, message || '载入中…');
    return api(listQuery())
      .then(function (data) {
        state.items = data && Array.isArray(data.items) ? data.items : [];
        state.total = data && typeof data.total === 'number' ? data.total : state.items.length;

        /* 当前页越界时回退一页 */
        if (state.items.length === 0 && state.offset > 0) {
          state.offset = Math.max(0, state.offset - PAGE_SIZE);
          return loadList(message);
        }

        renderList();
        if (message) {
          setStatusNode(els.consoleStatus, message);
        } else {
          setStatusNode(els.consoleStatus, '');
        }
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setStatusNode(els.consoleStatus, messageOf(err));
      });
  }

  /* -------------------------------------------------------------- 详情 */

  function renderDetail(item) {
    state.detail = item;

    setText(els.detailId, item.id || '');
    setText(els.detailContent, item.content || '');
    setText(els.detailCreated, item.created_at_bucket || '');
    setText(els.detailUpdated, item.updated_at || '未更新');
    setText(els.detailStatusText,
      (STATUS_CODES[item.status] || '') + ' ' + (STATUS_LABELS[item.status] || ''));

    els.detailCategory.value = item.category || '';

    els.detailNote.value = item.admin_note == null ? '' : String(item.admin_note);
    updateNoteCounter();

    Array.prototype.forEach.call(els.statusButtons, function (btn) {
      var active = btn.getAttribute('data-status') === item.status;
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    });

    setStatusNode(els.detailStatus, '');
    resetDeleteConfirm();

    els.detailEmpty.hidden = true;
    els.detail.hidden = false;
  }

  function openDetail(id) {
    if (!id) { return; }
    state.selectedId = id;
    setStatusNode(els.detailStatus, '载入中…');
    els.detailEmpty.hidden = true;
    els.detail.hidden = false;

    api('/admin/feedback/' + encodeURIComponent(id))
      .then(function (item) {
        renderDetail(item);
        renderList();
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setStatusNode(els.detailStatus, messageOf(err));
        if (err && err.status === 404) {
          hideDetail();
          loadList('该记录已不存在。');
        }
      });
  }

  function updateNoteCounter() {
    var len = els.detailNote.value.length;
    setText(els.noteCounter, '已输入 ' + len + ' / ' + MAX_NOTE);
    els.noteCounter.classList.toggle('at-limit', len >= MAX_NOTE);
  }

  function afterMutation(item, message) {
    if (item) { renderDetail(item); }
    return loadList(message);
  }

  function changeStatus(status) {
    if (!state.selectedId) { return; }
    setStatusNode(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { status: status }
    })
      .then(function (item) {
        return afterMutation(item, '已更新。');
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setStatusNode(els.detailStatus, messageOf(err));
      });
  }

  function saveNote() {
    if (!state.selectedId) { return; }
    var value = els.detailNote.value.trim();
    if (value.length === 0) {
      setStatusNode(els.detailStatus, '备注为空，如需清除请使用「清空备注」。');
      return;
    }
    if (value.length > MAX_NOTE) {
      setStatusNode(els.detailStatus, '备注超过 ' + MAX_NOTE + ' 字符。');
      return;
    }
    setStatusNode(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { admin_note: value }
    })
      .then(function (item) {
        return afterMutation(item, '已保存备注。');
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setStatusNode(els.detailStatus, messageOf(err));
      });
  }

  function clearNote() {
    if (!state.selectedId) { return; }
    setStatusNode(els.detailStatus, '保存中…');
    api('/admin/feedback/' + encodeURIComponent(state.selectedId), {
      method: 'PATCH',
      body: { admin_note: null }
    })
      .then(function (item) {
        return afterMutation(item, '已清空备注。');
      })
      .catch(function (err) {
        if (err && err.status === 401) { return; }
        setStatusNode(els.detailStatus, messageOf(err));
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
        hideDetail();
        return loadList('已删除。');
      })
      .catch(function (err) {
        els.deleteConfirmBtn.disabled = false;
        if (err && err.status === 401) { return; }
        resetDeleteConfirm();
        setStatusNode(els.detailStatus, messageOf(err));
      });
  }

  /* ---------------------------------------------------------------- 闸门 */

  function submitGate(event) {
    event.preventDefault();
    var token = els.tokenInput.value.trim();
    if (!token) {
      setStatusNode(els.gateError, '请输入访问令牌。');
      els.gateError.hidden = false;
      els.tokenInput.focus();
      return;
    }

    setGateBusy(true);
    els.gateError.hidden = true;
    state.token = token;

    api('/admin/feedback?limit=1')
      .then(function () {
        setGateBusy(false);
        storeToken(token);
        state.offset = 0;
        els.tokenInput.value = '';
        showConsole();
        return loadList();
      })
      .catch(function (err) {
        setGateBusy(false);
        state.token = '';
        var message = err && err.status === 401 ? '令牌无效' : messageOf(err);
        setStatusNode(els.gateError, message);
        els.gateError.hidden = false;
        els.tokenInput.focus();
      });
  }

  function logout() {
    clearToken();
    state.token = '';
    state.filter = '';
    els.filter.value = '';
    hideDetail();
    closeConsole();
    setStatusNode(els.consoleStatus, '');
    showGate('');
  }

  /* ---------------------------------------------------------------- 绑定 */

  els.gateForm.addEventListener('submit', submitGate);
  els.logout.addEventListener('click', logout);
  els.refresh.addEventListener('click', function () {
    loadList('已刷新。');
  });
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
    state.offset = state.offset + PAGE_SIZE;
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
  els.deleteBtn.addEventListener('click', requestDelete);
  els.deleteCancelBtn.addEventListener('click', cancelDelete);
  els.deleteConfirmBtn.addEventListener('click', confirmDelete);

  /* ---------------------------------------------------------------- 启动 */

  var saved = readToken();
  if (saved) {
    state.token = saved;
    showConsole();
    loadList();
  } else {
    showGate('');
  }
})();

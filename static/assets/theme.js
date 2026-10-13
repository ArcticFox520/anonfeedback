(function () {
  'use strict';

  var KEY = 'anonfb_theme';
  var root = document.documentElement;

  function stored() {
    try {
      var v = window.localStorage.getItem(KEY);
      return v === 'light' || v === 'dark' ? v : '';
    } catch (err) {
      return '';
    }
  }

  function save(value) {
    try {
      window.localStorage.setItem(KEY, value);
    } catch (err) {

    }
  }

  function current() {
    var explicit = root.getAttribute('data-theme');
    if (explicit === 'light' || explicit === 'dark') { return explicit; }
    return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches
      ? 'dark'
      : 'light';
  }

  function label() {
    return current() === 'dark' ? '切换到浅色模式' : '切换到深色模式';
  }

  function applyIcon() {
    var btn = document.getElementById('theme-toggle');
    if (btn) { btn.setAttribute('aria-label', label()); }
  }

  function setTheme(value) {
    root.setAttribute('data-theme', value);
    applyIcon();
  }

  function toggle() {
    var next = current() === 'dark' ? 'light' : 'dark';
    setTheme(next);
    save(next);
  }

  var saved = stored();
  if (saved) { root.setAttribute('data-theme', saved); }

  document.addEventListener('DOMContentLoaded', function () {
    var btn = document.getElementById('theme-toggle');
    if (!btn) { return; }
    applyIcon();
    btn.addEventListener('click', toggle);
  });

  if (window.matchMedia) {
    var mq = window.matchMedia('(prefers-color-scheme: dark)');
    var onChange = function () {
      if (!stored()) { root.removeAttribute('data-theme'); }
      applyIcon();
    };
    if (mq.addEventListener) {
      mq.addEventListener('change', onChange);
    } else if (mq.addListener) {
      mq.addListener(onChange);
    }
  }
})();

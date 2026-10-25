/*
 * Launcher lobby: Quick Play and the project's own server list.
 *
 * Talks to the master proxy's project directory:
 *   GET  /play/api/servers            our servers (status, players, free slots)
 *   POST /play/api/matchmake          {mode, region, exclude} -> one server + reservation
 *   POST /play/api/matchmake/release  {reservation}
 *
 * The chosen server is passed to the engine as "+connect ip:port" when it boots
 * (window.cod2LobbyArgs, read by the shell's startGame). If the engine is
 * already running, Module._Web_LobbyConnect issues the connect. When the
 * server still refuses ("Server is full"), Quick Play asks the
 * directory again with that server excluded. The dedicated server remains
 * the authority on whether a client gets in.
 *
 * Without a project directory (old public-masterlist mode) the lobby hides
 * itself and the launcher behaves as before.
 */
(function () {
  'use strict';
  if (typeof document === 'undefined' || typeof fetch !== 'function') return;

  var API = '/play/api';
  var MAX_RETRIES = 3;
  var REFRESH_MS = 5000;

  var state = {
    servers: [],
    pending: null,        // {connect, id, reservation, quick} waiting for engine boot
    current: null,        // last join attempt after boot args were consumed
    retries: 0,
    excluded: [],
    timer: 0,
    available: false
  };

  var els = {};

  function h(tag, attrs, text) {
    var el = document.createElement(tag);
    if (attrs) for (var k in attrs) if (Object.prototype.hasOwnProperty.call(attrs, k)) el.setAttribute(k, attrs[k]);
    if (text != null) el.textContent = text;
    return el;
  }

  function setStatus(text, isErr) {
    if (!els.status) return;
    els.status.textContent = text || '';
    els.status.className = 'lobby-status' + (isErr ? ' err' : '');
  }

  function engineStarted() {
    var canvas = document.getElementById('canvas');
    return !!(canvas && canvas.style.display === 'block');
  }

  function postJson(path, body) {
    return fetch(API + path, {
      method: 'POST',
      credentials: 'same-origin',
      cache: 'no-cache',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {})
    }).then(function (res) {
      return res.json().catch(function () { return {}; }).then(function (data) {
        return { ok: res.ok, status: res.status, data: data || {} };
      });
    });
  }

  function release(reservation) {
    if (!reservation) return;
    try {
      var blob = new Blob([JSON.stringify({ reservation: reservation })], { type: 'application/json' });
      if (navigator.sendBeacon && navigator.sendBeacon(API + '/matchmake/release', blob)) return;
    } catch (e) {}
    postJson('/matchmake/release', { reservation: reservation }).catch(function () {});
  }

  function describeFailure(r) {
    if (r.status === 429) return 'Za dużo prób. Spróbuj za minutę.';
    if (r.data && r.data.error === 'no_server_online') return 'Żaden serwer nie jest teraz dostępny.';
    if (r.data && r.data.error === 'no_free_server') return 'Wszystkie serwery są pełne. Spróbuj za chwilę.';
    return 'Nie udało się znaleźć serwera (HTTP ' + r.status + ').';
  }

  // ---- server list ------------------------------------------------------
  function render() {
    if (!els.list) return;
    els.list.textContent = '';
    if (!state.servers.length) {
      els.list.appendChild(h('div', { 'class': 'lobby-empty' }, 'Brak skonfigurowanych serwerów.'));
      return;
    }
    state.servers.forEach(function (s) {
      var online = s.status === 'online';
      var row = h('div', { 'class': 'lobby-row' + (online ? '' : ' off') });
      var main = h('div', { 'class': 'lobby-main' });
      main.appendChild(h('div', { 'class': 'lobby-name' }, s.name));
      main.appendChild(h('div', { 'class': 'lobby-meta' },
        online ? [s.map || '?', (s.mode || s.gametype || '').toUpperCase(), s.ping_ms < 999 ? s.ping_ms + ' ms' : '']
                   .filter(Boolean).join(' · ')
               : 'offline'));
      row.appendChild(main);
      row.appendChild(h('div', { 'class': 'lobby-count' },
        online ? (s.players + '/' + s.maxPlayers) : '–'));
      var btn = h('button', { type: 'button', 'class': 'lobby-join' }, s.free > 0 ? 'Dołącz' : (online ? 'Pełny' : '—'));
      btn.disabled = !online || s.free <= 0 || s.password;
      btn.addEventListener('click', function () { joinServer(s); });
      row.appendChild(btn);
      els.list.appendChild(row);
    });
  }

  function refresh() {
    return fetch(API + '/servers', { credentials: 'same-origin', cache: 'no-cache' })
      .then(function (res) {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.json();
      })
      .then(function (data) {
        if (!data || data.source !== 'project' || !Array.isArray(data.servers)) throw new Error('no directory');
        state.servers = data.servers;
        if (!state.available) {
          state.available = true;
          els.root.style.display = '';
        }
        var online = data.servers.filter(function (s) { return s.status === 'online'; });
        var players = online.reduce(function (n, s) { return n + (s.players | 0); }, 0);
        els.summary.textContent = online.length + ' z ' + data.servers.length + ' serwerów online · ' + players + ' graczy';
        els.quick.disabled = !online.some(function (s) { return s.free > 0; });
        render();
      })
      .catch(function () {
        if (!state.available) els.root.style.display = 'none';
        else els.summary.textContent = 'Lista serwerów chwilowo niedostępna';
      });
  }

  function startRefresh() {
    stopRefresh();
    refresh();
    state.timer = setInterval(function () {
      if (engineStarted()) { stopRefresh(); return; }
      refresh();
    }, REFRESH_MS);
  }

  function stopRefresh() {
    if (state.timer) clearInterval(state.timer);
    state.timer = 0;
  }

  // ---- joining ----------------------------------------------------------
  function launch(target) {
    if (engineStarted()) {
      state.current = target;
      var mod = window.Module;
      var ok = 0;
      try {
        if (mod && typeof mod.ccall === 'function')
          ok = mod.ccall('Web_LobbyConnect', 'number', ['string'], [target.connect]);
      } catch (e) {}
      if (!ok) setStatus('Silnik nie przyjął adresu ' + target.connect, true);
      else setStatus('Łączenie z ' + target.name + '…');
      return;
    }
    state.pending = target;
    setStatus('Wybrano ' + target.name + ' (' + target.connect + '). Uruchamianie gry…');
    var begin = document.getElementById('beginBtn');
    if (begin && !begin.disabled) begin.click();
    else setStatus('Wybrano ' + target.name + '. Kliknij Enter, gdy gra będzie gotowa.');
  }

  function quickPlay(opts) {
    opts = opts || {};
    els.quick.disabled = true;
    setStatus('Szukanie serwera z wolnym miejscem…');
    return postJson('/matchmake', { mode: opts.mode || '', region: opts.region || '', exclude: state.excluded.slice(0, 16) })
      .then(function (r) {
        els.quick.disabled = false;
        if (!r.ok) { setStatus(describeFailure(r), true); return; }
        var s = r.data;
        launch({ id: s.id, name: s.name, connect: s.connect, reservation: s.reservation, quick: true });
      })
      .catch(function (err) {
        els.quick.disabled = false;
        setStatus('Błąd połączenia z katalogiem serwerów: ' + (err && err.message ? err.message : err), true);
      });
  }

  function joinServer(s) {
    state.excluded = [];
    state.retries = 0;
    launch({ id: s.id, name: s.name, connect: s.connect, reservation: null, quick: false });
  }

  /* Called by the shell's startGame(): extra engine arguments, consumed once. */
  window.cod2LobbyArgs = function () {
    var t = state.pending;
    state.pending = null;
    if (!t) return [];
    state.current = t;
    stopRefresh();
    return ['+connect', t.connect];
  };

  /* Called by the shell for every engine console line. */
  /* The dedicated server answers a full server with "error\nEXE_SERVERISFULL"
   * (sv_client_mp.c); the client drops with that key or its localized text. */
  var REFUSED = /EXE_SERVERISFULL|server is full/i;
  window.cod2LobbyOnLine = function (line) {
    var t = state.current;
    if (!t || !REFUSED.test(String(line))) return;
    release(t.reservation);
    t.reservation = null;
    if (!t.quick) { setStatus('Serwer ' + t.name + ' odmówił połączenia: ' + line, true); return; }
    if (state.excluded.indexOf(t.id) < 0) state.excluded.push(t.id);
    if (state.retries >= MAX_RETRIES) { setStatus('Nie udało się wejść na żaden serwer.', true); return; }
    state.retries++;
    state.current = null;
    quickPlay();
  };

  window.addEventListener('pagehide', function () {
    if (state.current) release(state.current.reservation);
    if (state.pending) release(state.pending.reservation);
  });

  // ---- UI ---------------------------------------------------------------
  function build() {
    var panel = document.getElementById('panel');
    var begin = document.getElementById('beginBtn');
    if (!panel || !begin) return;

    var style = h('style');
    style.textContent = [
      '#lobby{margin:6px 0 10px;text-align:left}',
      '#lobby .lobby-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}',
      '#lobby .lobby-title{font-family:Oswald,Impact,sans-serif;font-size:12px;font-weight:600;letter-spacing:.14em;text-transform:uppercase;color:var(--ink)}',
      '#lobby .lobby-summary{font-size:12px;color:var(--muted)}',
      '#lobbyQuick{width:100%;margin:0 0 10px;letter-spacing:.14em}',
      '#lobby .lobby-list{max-height:220px;overflow:auto;border:1px solid rgba(216,210,192,.14);background:rgba(0,0,0,.22)}',
      '#lobby .lobby-row{display:flex;align-items:center;gap:10px;padding:8px 10px;border-bottom:1px solid rgba(216,210,192,.08)}',
      '#lobby .lobby-row:last-child{border-bottom:0}',
      '#lobby .lobby-row.off{opacity:.55}',
      '#lobby .lobby-main{flex:1;min-width:0}',
      '#lobby .lobby-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:var(--ink)}',
      '#lobby .lobby-meta{font-size:12px;color:var(--muted)}',
      '#lobby .lobby-count{font-variant-numeric:tabular-nums;color:var(--ink);min-width:44px;text-align:right}',
      '#lobby .lobby-join{margin:0;padding:6px 12px;font-size:12px}',
      '#lobby .lobby-empty{padding:10px;color:var(--muted);font-size:13px}',
      '#lobby .lobby-status{min-height:18px;margin-top:8px;font-size:12px;color:var(--muted)}',
      '#lobby .lobby-status.err{color:var(--danger)}'
    ].join('\n');
    document.head.appendChild(style);

    var root = h('section', { id: 'lobby', 'aria-label': 'Serwery' });
    root.style.display = 'none';
    var head = h('div', { 'class': 'lobby-head' });
    head.appendChild(h('span', { 'class': 'lobby-title' }, 'Nasze serwery'));
    var summary = h('span', { 'class': 'lobby-summary' }, 'Wczytywanie…');
    head.appendChild(summary);
    var quick = h('button', { id: 'lobbyQuick', type: 'button' }, 'Szybka gra');
    quick.addEventListener('click', function () {
      state.excluded = [];
      state.retries = 0;
      quickPlay();
    });
    var list = h('div', { 'class': 'lobby-list', role: 'list' });
    var status = h('div', { 'class': 'lobby-status', role: 'status', 'aria-live': 'polite' });
    root.appendChild(head);
    root.appendChild(quick);
    root.appendChild(list);
    root.appendChild(status);
    begin.parentNode.insertBefore(root, begin.nextSibling);

    els = { root: root, summary: summary, quick: quick, list: list, status: status };
    startRefresh();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build);
  else build();
})();

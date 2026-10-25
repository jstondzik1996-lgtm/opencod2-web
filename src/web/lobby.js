/*
 * Launcher lobby: nick, "Graj" (Quick Play) and the project's own server list.
 *
 * Master proxy endpoints (tools/master_proxy):
 *   GET  /play/api/servers            our servers (status, players, free slots)
 *   POST /play/api/session            {nick, playerKey} -> player session token
 *   POST /play/api/matchmake          {session, mode, region, exclude} -> server + ticket
 *   POST /play/api/join               {session, serverId} -> server + ticket
 *   POST /play/api/matchmake/release  {reservation}
 *
 * Identity: a random playerKey stays in this browser (localStorage); the
 * backend derives a stable player id from it, so no account or CD key is
 * needed on project servers. Sessions are optional: when the backend has no
 * COD2_SESSION_SECRET, /session answers 404 and joins work without tickets.
 *
 * The chosen server is passed to the engine as "+set name <nick> +connect
 * ip:port" at boot (window.cod2LobbyArgs, read by the shell's startGame). If
 * the engine is already running, Module._Web_LobbyConnect issues the
 * connect. The join ticket is published in COD2_RELAY_TICKETS for
 * web_net.js; COD2_RELAY_SESSION covers in-game reconnects. When a server
 * still refuses ("Server is full"), Quick Play retries with it excluded.
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
  var KEY_STORE = 'cod2web.playerKey';
  var NICK_STORE = 'cod2web.nick';

  var state = {
    servers: [],
    pending: null,        // {connect, id, name, reservation, quick} waiting for engine boot
    current: null,        // last join attempt after boot args were consumed
    retries: 0,
    excluded: [],
    timer: 0,
    available: false,
    session: null,        // {token, pid, nick, expiresAt} or false when sessions are off
    busy: false
  };

  var els = {};

  function h(tag, attrs, text) {
    var el = document.createElement(tag);
    if (attrs) for (var k in attrs) if (Object.prototype.hasOwnProperty.call(attrs, k)) el.setAttribute(k, attrs[k]);
    if (text != null) el.textContent = text;
    return el;
  }

  function store(name, value) {
    try {
      if (value === undefined) return window.localStorage.getItem(name);
      window.localStorage.setItem(name, value);
    } catch (e) {}
    return null;
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
    var err = r.data && r.data.error;
    if (r.status === 429) return 'Za dużo prób. Spróbuj za minutę.';
    if (err === 'banned') return 'Ten gracz ma zablokowany dostęp do naszych serwerów.';
    if (err === 'bad_nick') return 'Nick: 2–16 znaków (litery, cyfry, _ - . [ ]).';
    if (err === 'session_required') return 'Sesja wygasła. Spróbuj ponownie.';
    if (err === 'no_server_online') return 'Żaden serwer nie jest teraz dostępny.';
    if (err === 'no_free_server') return 'Wszystkie serwery są pełne. Spróbuj za chwilę.';
    if (err === 'full') return 'Ten serwer jest pełny.';
    if (err === 'offline') return 'Ten serwer jest offline.';
    return 'Nie udało się dołączyć (HTTP ' + r.status + ').';
  }

  // ---- identity ---------------------------------------------------------
  function playerKey() {
    var key = store(KEY_STORE);
    if (key && /^[A-Za-z0-9_-]{16,128}$/.test(key)) return key;
    var bytes = new Uint8Array(24);
    (window.crypto || window.msCrypto).getRandomValues(bytes);
    key = btoa(String.fromCharCode.apply(null, bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    store(KEY_STORE, key);
    return key;
  }

  function cleanNick(raw) {
    return String(raw || '').replace(/\^./g, '').replace(/ /g, '_').replace(/[^A-Za-z0-9_\-.\[\]]/g, '').slice(0, 16);
  }

  function currentNick() {
    var nick = cleanNick(els.nick ? els.nick.value : store(NICK_STORE));
    return nick.length >= 2 ? nick : '';
  }

  /* Resolves to the session object, or false when the backend runs without
   * sessions. Rejects with an error message for the status line. */
  function ensureSession() {
    var nick = currentNick();
    if (!nick) return Promise.reject(new Error('Wpisz nick (2–16 znaków).'));
    store(NICK_STORE, nick);
    var s = state.session;
    if (s === false) return Promise.resolve(false);
    if (s && s.nick === nick && s.expiresAt - Date.now() > 60000) return Promise.resolve(s);
    return postJson('/session', { nick: nick, playerKey: playerKey() }).then(function (r) {
      if (r.status === 404) { state.session = false; return false; }
      if (!r.ok) throw new Error(describeFailure(r));
      state.session = {
        token: r.data.token, pid: r.data.pid, nick: r.data.nick,
        expiresAt: Date.now() + (r.data.expiresIn | 0) * 1000
      };
      publishSession();
      return state.session;
    });
  }

  function publishSession() {
    var token = state.session ? state.session.token : '';
    window.COD2_RELAY_SESSION = token;
    try { if (window.Module) window.Module.COD2_RELAY_SESSION = token; } catch (e) {}
  }

  function publishTicket(connect, ticket) {
    if (!ticket) return;
    window.COD2_RELAY_TICKETS = window.COD2_RELAY_TICKETS || {};
    window.COD2_RELAY_TICKETS[connect] = ticket;
    try {
      if (window.Module) {
        window.Module.COD2_RELAY_TICKETS = window.Module.COD2_RELAY_TICKETS || {};
        window.Module.COD2_RELAY_TICKETS[connect] = ticket;
      }
    } catch (e) {}
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
      var row = h('div', { 'class': 'lobby-row' + (online ? '' : ' off'), role: 'listitem' });
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
      btn.disabled = !online || s.free <= 0 || s.password || state.busy;
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
        els.quick.disabled = state.busy || !online.some(function (s) { return s.free > 0; });
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

  function setBusy(b) {
    state.busy = b;
    if (els.quick) els.quick.disabled = b;
    render();
  }

  // ---- joining ----------------------------------------------------------
  function launch(target) {
    publishTicket(target.connect, target.ticket);
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

  function toTarget(s, quick) {
    return { id: s.id, name: s.name, connect: s.connect, reservation: s.reservation || null,
             ticket: s.ticket || null, quick: quick };
  }

  function quickPlay(opts) {
    opts = opts || {};
    setBusy(true);
    setStatus('Szukanie serwera z wolnym miejscem…');
    return ensureSession()
      .then(function (session) {
        return postJson('/matchmake', {
          session: session ? session.token : undefined,
          mode: opts.mode || '', region: opts.region || '',
          exclude: state.excluded.slice(0, 16)
        });
      })
      .then(function (r) {
        setBusy(false);
        if (!r.ok) {
          if (r.status === 401) state.session = null;
          setStatus(describeFailure(r), true);
          return;
        }
        launch(toTarget(r.data, true));
      })
      .catch(function (err) {
        setBusy(false);
        setStatus(err && err.message ? err.message : String(err), true);
      });
  }

  function joinServer(s) {
    state.excluded = [];
    state.retries = 0;
    setBusy(true);
    setStatus('Dołączanie do ' + s.name + '…');
    return ensureSession()
      .then(function (session) {
        return postJson('/join', { session: session ? session.token : undefined, serverId: s.id });
      })
      .then(function (r) {
        setBusy(false);
        if (r.status === 404 && !(r.data && r.data.error)) {
          /* Older backend without /join: connect directly. */
          launch(toTarget(s, false));
          return;
        }
        if (!r.ok) {
          if (r.status === 401) state.session = null;
          setStatus(describeFailure(r), true);
          refresh();
          return;
        }
        launch(toTarget(r.data, false));
      })
      .catch(function (err) {
        setBusy(false);
        setStatus(err && err.message ? err.message : String(err), true);
      });
  }

  /* Called by the shell's startGame(): extra engine arguments, consumed once. */
  window.cod2LobbyArgs = function () {
    var t = state.pending;
    state.pending = null;
    if (!t) return [];
    state.current = t;
    stopRefresh();
    var args = [];
    /*
     * The project's own directory contains dedicated LAN-mode servers
     * accessible through our admission-controlled relay. Tell the original
     * client about that LAN connection *before* issuing +connect. Without
     * this flag CL_CheckForResend invokes CL_RequestAuthorization, and a
     * fresh browser with no game key fails with EXE_ERR_INVALID_CD_KEY.
     *
     * This only applies to destinations admitted by our project directory,
     * not to arbitrary external/stock CoD2 servers.
     */
    args.push('+set', 'net_lanauthorize', '0');
    var nick = currentNick();
    if (nick) args.push('+set', 'name', nick);
    args.push('+connect', t.connect);
    return args;
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
      '#lobby .lobby-play{display:flex;gap:8px;margin:0 0 10px}',
      '#lobbyNick{flex:1;min-width:0;padding:10px 12px;font:inherit;color:var(--ink);background:rgba(0,0,0,.3);border:1px solid rgba(216,210,192,.22)}',
      '#lobbyNick:focus{outline:none;border-color:var(--brass)}',
      '#lobbyQuick{margin:0;letter-spacing:.14em;white-space:nowrap}',
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

    var play = h('div', { 'class': 'lobby-play' });
    var nick = h('input', { id: 'lobbyNick', type: 'text', maxlength: '40', autocomplete: 'nickname',
                            placeholder: 'Twój nick', 'aria-label': 'Nick' });
    nick.value = cleanNick(store(NICK_STORE) || '');
    nick.addEventListener('keydown', function (ev) {
      ev.stopPropagation();   /* the launcher binds Enter to its own button */
      if (ev.key === 'Enter' && !quick.disabled) quick.click();
    });
    nick.addEventListener('change', function () { nick.value = cleanNick(nick.value); });
    var quick = h('button', { id: 'lobbyQuick', type: 'button' }, 'Graj');
    quick.title = 'Szybka gra: najlepszy serwer z wolnym miejscem';
    quick.addEventListener('click', function () {
      state.excluded = [];
      state.retries = 0;
      quickPlay();
    });
    play.appendChild(nick);
    play.appendChild(quick);

    var list = h('div', { 'class': 'lobby-list', role: 'list' });
    var status = h('div', { 'class': 'lobby-status', role: 'status', 'aria-live': 'polite' });
    root.appendChild(head);
    root.appendChild(play);
    root.appendChild(list);
    root.appendChild(status);
    begin.parentNode.insertBefore(root, begin.nextSibling);

    els = { root: root, summary: summary, quick: quick, list: list, status: status, nick: nick };
    startRefresh();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build);
  else build();
})();

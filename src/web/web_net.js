/**
 * Browser-side WebSocket ↔ CoD2 UDP bridge (linked via --post-js).
 *
 * Provides globalThis.cod2NetBridge which the C glue (web_net.c) calls
 * through EM_ASM / ccall to send/receive game packets over a WebSocket
 * relay (tools/game_proxy/relay.py).
 *
 * Packet queue: WS binary frames from the relay are queued in JS.
 * The C side drains the queue each frame via cod2NetRecv().
 */
(function () {
  'use strict';

  var MAX_PACKET = 16384;

  /* ---- relay URL resolution ---- */
  function relayUrl(ip, port) {
    var base = '';
    try {
      if (typeof Module !== 'undefined' && Module.COD2_RELAY_URL)
        base = Module.COD2_RELAY_URL;
    } catch (e) {}
    if (!base) {
      try {
        if (typeof globalThis !== 'undefined' && globalThis.COD2_RELAY_URL)
          base = globalThis.COD2_RELAY_URL;
      } catch (e) {}
    }
    if (!base) {
      /* Default: same-origin WebSocket at /play/api/gamews */
      var loc = (typeof location !== 'undefined') ? location : null;
      if (loc) {
        var proto = (loc.protocol === 'https:') ? 'wss:' : 'ws:';
        base = proto + '//' + loc.host + '/play/api/gamews';
      } else {
        base = 'ws://127.0.0.1:20712/';
      }
    }
    var sep = (base.indexOf('?') >= 0) ? '&' : '?';
    var url = base + sep + 'host=' + encodeURIComponent(ip) +
              '&port=' + encodeURIComponent(port);
    /* Project servers: the lobby stores a join ticket per "ip:port"
     * (COD2_RELAY_TICKETS); the relay admits the session only with it. */
    var ticket = relayTicket(ip + ':' + port);
    if (ticket) {
      url += '&ticket=' + encodeURIComponent(ticket);
    } else {
      /* Fallback for reconnects / in-game server list: the player session. */
      var session = relayScopeValue('COD2_RELAY_SESSION');
      if (session) url += '&session=' + encodeURIComponent(session);
    }
    return url;
  }

  function relayScopeValue(name) {
    var scopes = [];
    try { if (typeof Module !== 'undefined') scopes.push(Module); } catch (e) {}
    try { if (typeof globalThis !== 'undefined') scopes.push(globalThis); } catch (e) {}
    for (var i = 0; i < scopes.length; i++) {
      try {
        if (typeof scopes[i][name] === 'string' && scopes[i][name]) return scopes[i][name];
      } catch (e) {}
    }
    return '';
  }

  function relayTicket(key) {
    var scopes = [];
    try { if (typeof Module !== 'undefined') scopes.push(Module); } catch (e) {}
    try { if (typeof globalThis !== 'undefined') scopes.push(globalThis); } catch (e) {}
    for (var i = 0; i < scopes.length; i++) {
      try {
        var map = scopes[i].COD2_RELAY_TICKETS;
        if (map && typeof map[key] === 'string' && map[key]) return map[key];
      } catch (e) {}
    }
    return '';
  }

  /* ---- session state ---- */
  var ws = null;
  var rxQueue = [];          /* ArrayBuffer[] from relay */
  var sessionIp = [0,0,0,0];
  var sessionPort = 0;       /* network byte order (big-endian) */
  var connected = false;

  function ipStr(ip) {
    return ip[0] + '.' + ip[1] + '.' + ip[2] + '.' + ip[3];
  }

  /* ---- public API (called from C via EM_ASM) ---- */

  /**
   * Open (or reuse) a WS connection to the relay for target server ip:port.
   * ip is 4-byte array, port is in host byte order.
   */
  function cod2NetConnect(ip0, ip1, ip2, ip3, hostPort) {
    var ip = [ip0 & 0xff, ip1 & 0xff, ip2 & 0xff, ip3 & 0xff];
    /* CoD2 stores port in network order (big-endian) in netadr_t.port.
     * The C side passes it raw, so we convert to host order for the URL. */
    var netPort = hostPort & 0xffff;
    var displayPort = ((netPort >> 8) & 0xff) | ((netPort & 0xff) << 8);

    /* If already connected to same target, reuse */
    if (ws && ws.readyState <= 1 &&
        sessionIp[0] === ip[0] && sessionIp[1] === ip[1] &&
        sessionIp[2] === ip[2] && sessionIp[3] === ip[3] &&
        sessionPort === netPort) {
      return;
    }

    /* Close any prior session */
    cod2NetClose();

    sessionIp = ip;
    sessionPort = netPort;
    connected = false;
    rxQueue = [];

    var url = relayUrl(ipStr(ip), displayPort);
    if (typeof console !== 'undefined')
      console.log('[cod2-net] connecting relay', url.replace(/([?&](?:ticket|session)=)[^&]*/g, '$1[redacted]'));

    try {
      ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
    } catch (e) {
      if (typeof console !== 'undefined')
        console.error('[cod2-net] WebSocket create failed', e);
      ws = null;
      return;
    }

    ws.onopen = function () {
      connected = true;
      if (typeof console !== 'undefined')
        console.log('[cod2-net] relay connected to', ipStr(ip) + ':' + displayPort);
    };

    ws.onmessage = function (ev) {
      if (ev.data instanceof ArrayBuffer) {
        if (rxQueue.length < 256)
          rxQueue.push(ev.data);
      }
    };

    ws.onerror = function (ev) {
      if (typeof console !== 'undefined')
        console.warn('[cod2-net] ws error', ev);
    };

    ws.onclose = function (ev) {
      connected = false;
      if (typeof console !== 'undefined')
        console.log('[cod2-net] ws closed', ev.code, ev.reason);
    };
  }

  /**
   * Send binary data to the relay (→ UDP to game server).
   * Returns 1 on success, 0 on failure.
   */
  function cod2NetSend(ptr, length) {
    if (!ws || ws.readyState !== 1) return 0;
    if (length <= 0 || length > MAX_PACKET) return 0;
    try {
      var buf = new Uint8Array(Module.HEAPU8.buffer, ptr, length);
      ws.send(buf.slice().buffer);
      return 1;
    } catch (e) {
      if (typeof console !== 'undefined')
        console.warn('[cod2-net] send error', e);
      return 0;
    }
  }

  /**
   * Receive next queued packet from relay.
   * Writes into the C buffer at outPtr (up to maxLen bytes).
   * Returns actual length, or 0 if no packet available.
   * Also writes the session IP (4 bytes) at ipOutPtr and port (2 bytes, network order) at portOutPtr.
   */
  function cod2NetRecv(outPtr, maxLen, ipOutPtr, portOutPtr) {
    if (rxQueue.length === 0) return 0;
    var ab = rxQueue.shift();
    var src = new Uint8Array(ab);
    var len = src.length;
    if (len > maxLen) len = maxLen;
    Module.HEAPU8.set(src.subarray(0, len), outPtr);
    /* Write source address (the remote server we're connected to) */
    Module.HEAPU8[ipOutPtr]     = sessionIp[0];
    Module.HEAPU8[ipOutPtr + 1] = sessionIp[1];
    Module.HEAPU8[ipOutPtr + 2] = sessionIp[2];
    Module.HEAPU8[ipOutPtr + 3] = sessionIp[3];
    Module.HEAPU8[portOutPtr]     = (sessionPort) & 0xff;
    Module.HEAPU8[portOutPtr + 1] = (sessionPort >> 8) & 0xff;
    return len;
  }

  /**
   * Close the current relay session.
   */
  function cod2NetClose() {
    if (ws) {
      try { ws.close(); } catch (e) {}
      ws = null;
    }
    connected = false;
    rxQueue = [];
  }

  /**
   * Returns 1 if the WS is open (CONNECTING or OPEN), 0 otherwise.
   */
  function cod2NetIsOpen() {
    return (ws && ws.readyState <= 1) ? 1 : 0;
  }

  /* ---- attach to globals ---- */
  function attach(target) {
    if (!target) return;
    try {
      target.cod2NetConnect = cod2NetConnect;
      target.cod2NetSend    = cod2NetSend;
      target.cod2NetRecv    = cod2NetRecv;
      target.cod2NetClose   = cod2NetClose;
      target.cod2NetIsOpen  = cod2NetIsOpen;
    } catch (e) {}
  }

  attach(typeof Module !== 'undefined' ? Module : null);
  attach(typeof moduleArg !== 'undefined' ? moduleArg : null);
  attach(typeof globalThis !== 'undefined' ? globalThis : null);
  attach(typeof self !== 'undefined' ? self : null);
  attach(typeof window !== 'undefined' ? window : null);
})();

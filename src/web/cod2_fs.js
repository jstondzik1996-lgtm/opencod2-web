(function (global) {
  'use strict';

  var IDB_NAME = 'cod2-web-fs-v1';
  var IDB_STORE = 'kv';
  var KEY_DIR_HANDLE = 'install-directory-handle';
  var KEY_CACHE_META = 'opfs-cache-meta';
  var OPFS_ROOT = 'cod2';

  var WANTED_EXT = ['.iwd', '.cfg', '.dat', '.txt', '.menu', '.so', '.dll',
                    '.bik', '.roq', '.str', '.gsc', '.csc', '.arena'];

  var WANTED_DIRS = ['main', 'zone'];

  function openDb() {
    return new Promise(function (resolve, reject) {
      var req = indexedDB.open(IDB_NAME, 1);
      req.onupgradeneeded = function () { req.result.createObjectStore(IDB_STORE); };
      req.onsuccess = function () { resolve(req.result); };
      req.onerror = function () { reject(req.error); };
    });
  }
  function idbGet(key) {
    return openDb().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(IDB_STORE, 'readonly');
        var rq = tx.objectStore(IDB_STORE).get(key);
        rq.onsuccess = function () { resolve(rq.result); };
        rq.onerror = function () { reject(rq.error); };
        tx.oncomplete = function () { db.close(); };
      });
    });
  }
  function idbSet(key, value) {
    return openDb().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(IDB_STORE, 'readwrite');
        tx.objectStore(IDB_STORE).put(value, key);
        tx.oncomplete = function () { db.close(); resolve(); };
        tx.onerror = function () { db.close(); reject(tx.error); };
      });
    });
  }
  function idbDel(key) {
    return openDb().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(IDB_STORE, 'readwrite');
        tx.objectStore(IDB_STORE)['delete'](key);
        tx.oncomplete = function () { db.close(); resolve(); };
        tx.onerror = function () { db.close(); reject(tx.error); };
      });
    });
  }

  function hasFsAccess() { return typeof global.showDirectoryPicker === 'function'; }
  function hasOpfs() {
    return !!(global.navigator && navigator.storage &&
              typeof navigator.storage.getDirectory === 'function');
  }

  function opfsRoot() { return navigator.storage.getDirectory(); }

  async function opfsDir(root, relPath, create) {
    var dir = root;
    var parts = relPath.split('/').filter(Boolean);
    for (var i = 0; i < parts.length; i++) {
      dir = await dir.getDirectoryHandle(parts[i], { create: !!create });
    }
    return dir;
  }
  async function opfsFileHandle(root, relPath, create) {
    var slash = relPath.lastIndexOf('/');
    var dir = slash >= 0 ? await opfsDir(root, relPath.slice(0, slash), create) : root;
    return dir.getFileHandle(relPath.slice(slash + 1), { create: !!create });
  }
  async function opfsRead(relPath) {
    var root = await opfsRoot();
    var base = await opfsDir(root, OPFS_ROOT, false);
    var fh = await opfsFileHandle(base, relPath, false);
    return fh.getFile();
  }
  async function opfsClearCache() {
    var root = await opfsRoot();
    try { await root.removeEntry(OPFS_ROOT, { recursive: true }); } catch (e) {  }
    await idbDel(KEY_CACHE_META);
  }

  function isWantedFile(name) {
    var lower = name.toLowerCase();
    for (var i = 0; i < WANTED_EXT.length; i++) {
      if (lower.endsWith(WANTED_EXT[i])) return true;
    }
    return false;
  }

  async function resolveInstallRoot(handle) {

    for await (var entry of handle.values()) {
      if (entry.kind === 'directory' && entry.name.toLowerCase() === 'main') {
        return handle;
      }
    }

    return null;
  }

  async function scanInstall(pickedHandle) {
    var entries = [];
    var installRoot = await resolveInstallRoot(pickedHandle);
    var pairs;
    if (installRoot) {
      pairs = [{ dir: installRoot, prefix: '' }];
    } else {

      pairs = [{ dir: pickedHandle, prefix: 'main' }];
    }

    async function walk(dirHandle, prefix, depth) {
      for await (var entry of dirHandle.values()) {
        var rel = prefix ? prefix + '/' + entry.name : entry.name;
        if (entry.kind === 'file') {
          if (isWantedFile(entry.name)) {
            var f = await entry.getFile();
            entries.push({ rel: rel, handle: entry, size: f.size, file: f });
          }
        } else if (entry.kind === 'directory') {
          var top = rel.split('/')[0].toLowerCase();

          if (depth === 0 && WANTED_DIRS.indexOf(top) < 0) continue;
          if (depth < 8) await walk(entry, rel, depth + 1);
        }
      }
    }
    for (var i = 0; i < pairs.length; i++) {
      await walk(pairs[i].dir, pairs[i].prefix, 0);
    }
    return entries;
  }

  function looksLikeInstall(entries) {
    for (var i = 0; i < entries.length; i++) {
      var r = entries[i].rel.toLowerCase();
      if (r.indexOf('main/') === 0 && r.endsWith('.iwd')) return true;
    }
    return false;
  }

  async function copyToOpfs(entries, onProgress) {
    var root = await opfsRoot();
    var base = await opfsDir(root, OPFS_ROOT, true);

    var bytesTotal = 0, bytesDone = 0;
    for (var i = 0; i < entries.length; i++) bytesTotal += entries[i].size;

    var meta = { ready: false, files: [] };
    for (var j = 0; j < entries.length; j++) {
      var e = entries[j];
      var skip = false;

      try {
        var existing = await opfsFileHandle(base, e.rel, false);
        var ef = await existing.getFile();
        if (ef.size === e.size) skip = true;
      } catch (err) {  }

      if (onProgress) onProgress({
        phase: skip ? 'skip' : 'copy', fileIndex: j + 1, fileCount: entries.length,
        fileName: e.rel, bytesDone: bytesDone, bytesTotal: bytesTotal
      });

      if (!skip) {
        var fh = await opfsFileHandle(base, e.rel, true);
        var writable = await fh.createWritable();

        await e.file.stream().pipeTo(writable);
      }
      bytesDone += e.size;
      meta.files.push({ path: e.rel, size: e.size });
      if (onProgress) onProgress({
        phase: 'progress', fileIndex: j + 1, fileCount: entries.length,
        fileName: e.rel, bytesDone: bytesDone, bytesTotal: bytesTotal
      });
    }
    meta.ready = true;
    await idbSet(KEY_CACHE_META, meta);
    if (typeof navigator.storage.persist === 'function') {
      try { await navigator.storage.persist(); } catch (e) {  }
    }
    return meta;
  }

  async function queryPerm(handle) {
    if (!handle || typeof handle.queryPermission !== 'function') return 'denied';
    return handle.queryPermission({ mode: 'read' });
  }
  async function requestPerm(handle) {
    if (!handle || typeof handle.requestPermission !== 'function') return 'denied';
    return handle.requestPermission({ mode: 'read' });
  }

  async function enumerateInstall(handle) {
    var entries = await scanInstall(handle);
    if (!looksLikeInstall(entries)) {
      throw new Error('That folder does not look like a CoD2 install ' +
        '(no main/*.iwd found). Pick the folder that contains "main".');
    }

    await idbSet(KEY_DIR_HANDLE, handle);
    var files = entries.map(function (e) {
      return { path: e.rel, file: e.file, size: e.size };
    });
    return { name: handle.name || 'cod2', files: files };
  }

  var OPFS_HOME = 'home';
  async function opfsHomeDir(create) {
    var root = await opfsRoot();
    return opfsDir(root, OPFS_HOME, create !== false);
  }

  var Cod2Fs = {
    OPFS_ROOT: OPFS_ROOT,
    OPFS_HOME: OPFS_HOME,
    hasFsAccess: hasFsAccess,
    hasOpfs: hasOpfs,

    enumerateInstall: enumerateInstall,
    opfsHomeDir: opfsHomeDir,

    async getCacheMeta() {
      var m = await idbGet(KEY_CACHE_META);
      return (m && m.ready) ? m : null;
    },

    getSavedHandle() { return idbGet(KEY_DIR_HANDLE); },

    queryHandlePermission: queryPerm,
    requestHandlePermission: requestPerm,

    async pickDirectory() {
      if (!hasFsAccess()) {
        throw new Error('This browser lacks the File System Access API ' +
          '(Chromium-only). Use Chrome, Edge, or another Chromium browser.');
      }
      return global.showDirectoryPicker({ id: 'cod2', mode: 'read' });
    },

    async ingestHandle(handle, onProgress) {
      if (onProgress) onProgress({ phase: 'scan' });
      var entries = await scanInstall(handle);
      if (!looksLikeInstall(entries)) {
        throw new Error('That folder does not look like a CoD2 install ' +
          '(no main/*.iwd found). Pick the folder that contains "main".');
      }
      await idbSet(KEY_DIR_HANDLE, handle);
      var meta = await copyToOpfs(entries, onProgress);
      if (onProgress) onProgress({ phase: 'done', fileCount: entries.length });
      return meta;
    },

    readCached: opfsRead,

    clearCache: opfsClearCache,
    forgetHandle() { return idbDel(KEY_DIR_HANDLE); }
  };

  async function ingestFiles(files, onProgress) {
    // files: [{ rel, size, file: File }, ...]
    return copyToOpfs(files, onProgress);
  }

  Cod2Fs.ingestFiles = ingestFiles;
  Cod2Fs.isWantedFile = isWantedFile;

  // ---- Remote HTTP assets (VPS / CDN) ------------------------------------
  // Manifest shape: { baseUrl?: string, files: [{ path, size, version? }] }
  // Paths are relative to baseUrl (default: same origin "/").
  // Example path: "main/iw_00.iwd"

  function joinUrl(base, rel) {
    var b = String(base || '/');
    if (b.charAt(b.length - 1) !== '/') b += '/';
    return b + String(rel || '').replace(/^\/+/, '');
  }

  async function fetchJson(url) {
    var res = await fetch(url, { credentials: 'same-origin', cache: 'no-cache' });
    if (!res.ok) throw new Error('Failed to fetch ' + url + ' (' + res.status + ')');
    return res.json();
  }

  async function downloadToOpfs(baseUrl, files, onProgress) {
    var root = await opfsRoot();
    var base = await opfsDir(root, OPFS_ROOT, true);
    /* Stable order so UI / workers don't bounce between names randomly. */
    files = files.slice().sort(function (a, b) {
      return String(a.path || '').localeCompare(String(b.path || ''));
    });
    var bytesTotal = 0, i;
    /* Five ~500–740MB IWDs on the VPS pack; 2 streams serialized the first run. */
    var CONCURRENCY = 6;
    for (i = 0; i < files.length; i++) bytesTotal += (files[i].size | 0);

    var meta = { ready: false, files: new Array(files.length), source: 'remote', baseUrl: baseUrl };
    /* Versions of what is already cached, so a replaced archive of the same
     * size is downloaded again (manifest "version", optional). */
    var cachedVersion = Object.create(null);
    try {
      var oldMeta = await idbGet(KEY_CACHE_META);
      if (oldMeta && oldMeta.files) {
        for (i = 0; i < oldMeta.files.length; i++) {
          var of = oldMeta.files[i];
          if (of && of.path && of.version) cachedVersion[of.path] = of.version;
        }
      }
    } catch (e) { /* no previous cache */ }
    var nextIndex = 0;
    var finishedCount = 0;
    var finishedBytes = 0;
    /* idx -> bytes received so far (in-flight only). */
    var inflight = Object.create(null);
    var currentName = '';
    var lastReportAt = 0;

    function totalDone() {
      var n = finishedBytes, k;
      for (k in inflight) n += inflight[k];
      return n;
    }

    function report(phase, fileName) {
      if (!onProgress) return;
      var now = Date.now();
      /* Throttle mid-stream updates; always emit start/skip/finish. */
      if (phase === 'progress' && (now - lastReportAt) < 200) return;
      lastReportAt = now;
      if (fileName) currentName = fileName;
      onProgress({
        phase: phase,
        fileIndex: Math.min(finishedCount + (Object.keys(inflight).length ? 1 : 0), files.length),
        fileCount: files.length,
        fileName: currentName,
        finishedCount: finishedCount,
        bytesDone: totalDone(),
        bytesTotal: bytesTotal
      });
    }

    async function writeStream(res, writable, idx, expected) {
      var loaded = 0;
      inflight[idx] = 0;
      if (res.body && typeof res.body.getReader === 'function') {
        var reader = res.body.getReader();
        for (;;) {
          var chunk = await reader.read();
          if (chunk.done) break;
          await writable.write(chunk.value);
          loaded += chunk.value.byteLength;
          inflight[idx] = loaded;
          report('progress', null);
        }
        await writable.close();
      } else {
        var buf = new Uint8Array(await res.arrayBuffer());
        await writable.write(buf);
        await writable.close();
        loaded = buf.byteLength;
      }
      delete inflight[idx];
      return loaded || expected;
    }

    async function processOne(idx) {
      var rec = files[idx];
      var rel = String(rec.path || '');
      var size = rec.size | 0;
      var version = rec.version ? String(rec.version) : '';
      var skip = false;
      try {
        var existing = await opfsFileHandle(base, rel, false);
        var ef = await existing.getFile();
        if (size > 0 && ef.size === size && (!version || cachedVersion[rel] === version)) skip = true;
      } catch (err) { /* missing */ }

      if (skip) {
        finishedCount++;
        finishedBytes += size;
        meta.files[idx] = { path: rel, size: size, version: version };
        report('skip', rel);
        return;
      }

      report('copy', rel);
      var url = joinUrl(baseUrl, rel);
      var res = await fetch(url, { credentials: 'same-origin' });
      if (!res.ok) throw new Error('Download failed: ' + url + ' (' + res.status + ')');
      var fh = await opfsFileHandle(base, rel, true);
      var writable = await fh.createWritable();
      var written = await writeStream(res, writable, idx, size);
      if (size > 0 && written !== size) {
        throw new Error('Size mismatch for ' + rel + ': got ' + written + ', expected ' + size);
      }
      finishedCount++;
      finishedBytes += written;
      meta.files[idx] = { path: rel, size: written, version: version };
      report('progress', rel);
    }

    async function worker() {
      for (;;) {
        var idx = nextIndex++;
        if (idx >= files.length) return;
        await processOne(idx);
      }
    }

    var workers = [];
    var n = Math.min(CONCURRENCY, files.length || 1);
    for (i = 0; i < n; i++) workers.push(worker());
    await Promise.all(workers);

    meta.ready = true;
    await idbSet(KEY_CACHE_META, meta);
    if (typeof navigator.storage.persist === 'function') {
      try { await navigator.storage.persist(); } catch (e) { /* ignore */ }
    }
    return meta;
  }

  async function detectRemoteConfig() {
    // Priority:
    // 1) window.COD2_REMOTE_ASSETS = { manifestUrl, baseUrl }
    // 2) ?assets=/play/assets.json  (or absolute URL)
    // 3) sibling ./assets.json
    var cfg = global.COD2_REMOTE_ASSETS;
    if (cfg && cfg.manifestUrl) {
      return {
        manifestUrl: cfg.manifestUrl,
        baseUrl: cfg.baseUrl || '/'
      };
    }
    try {
      var params = new URLSearchParams(global.location && location.search || '');
      var q = params.get('assets');
      if (q) {
        return { manifestUrl: q, baseUrl: params.get('base') || '/' };
      }
    } catch (e) { /* ignore */ }
    try {
      var probe = await fetch('assets.json', { method: 'HEAD', credentials: 'same-origin', cache: 'no-cache' });
      if (probe.ok) return { manifestUrl: 'assets.json', baseUrl: '/' };
    } catch (e2) { /* ignore */ }
    return null;
  }

  async function ingestRemote(remoteCfg, onProgress) {
    if (!hasOpfs()) {
      throw new Error('OPFS is required for remote assets. Use Chrome / Edge.');
    }
    if (onProgress) onProgress({ phase: 'scan' });
    var manifest = await fetchJson(remoteCfg.manifestUrl);
    var baseUrl = remoteCfg.baseUrl || manifest.baseUrl || '/';
    var files = manifest.files || [];
    if (!files.length) throw new Error('Remote manifest has no files: ' + remoteCfg.manifestUrl);
    var looks = false;
    for (var i = 0; i < files.length; i++) {
      var p = String(files[i].path || '').toLowerCase();
      if (p.indexOf('main/') === 0 && p.endsWith('.iwd')) { looks = true; break; }
    }
    if (!looks) {
      throw new Error('Remote manifest does not look like a CoD2 install (no main/*.iwd).');
    }
    var meta = await downloadToOpfs(baseUrl, files, onProgress);
    if (onProgress) onProgress({ phase: 'done', fileCount: files.length });
    return meta;
  }

  Cod2Fs.detectRemoteConfig = detectRemoteConfig;
  Cod2Fs.ingestRemote = ingestRemote;
  Cod2Fs.joinUrl = joinUrl;

  global.Cod2Fs = Cod2Fs;
})(typeof window !== 'undefined' ? window : this);

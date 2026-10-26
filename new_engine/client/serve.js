// Local static preview of the Jules/PlayCanvas client.
// Uses only Node.js built-ins; does not modify the legacy CoD2 preview.
'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname);
const port = Number(process.env.WWII_PREVIEW_PORT || 8090);
const mimeTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.wasm': 'application/wasm',
};

http.createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405, { 'Allow': 'GET, HEAD' }).end();
    return;
  }
  let pathname;
  try {
    pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
  } catch {
    res.writeHead(400).end('Invalid URL');
    return;
  }
  if (pathname === '/') pathname = '/index.html';
  const filename = path.resolve(root, '.' + pathname);
  if (filename !== root && !filename.startsWith(root + path.sep)) {
    res.writeHead(403).end('Forbidden');
    return;
  }
  fs.stat(filename, (err, stats) => {
    if (err || !stats.isFile()) {
      res.writeHead(404).end('Not found');
      return;
    }
    res.setHeader('Content-Type', mimeTypes[path.extname(filename)] || 'application/octet-stream');
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.writeHead(200);
    if (req.method === 'HEAD') {
      res.end();
      return;
    }
    fs.createReadStream(filename).on('error', () => res.destroy()).pipe(res);
  });
}).listen(port, '127.0.0.1', () => {
  console.log(`WWII new-engine client preview: http://127.0.0.1:${port}`);
  console.log('Backend WebSocket (GameRoom): ws://127.0.0.1:2567');
});

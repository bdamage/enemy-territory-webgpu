#!/usr/bin/env node
// Enemy Territory web server: serves the game (dist/ at /, data/ at /data/)
// and runs the WebSocket <-> UDP relay that gives browsers network access.
//
//   node tools/server.mjs [--port 8080] [--host 127.0.0.1]
//                         [--ports 27000-28000] [--no-private] [--no-static]
//
// Relay protocol (binary WebSocket messages, first byte = type):
//   0x00 DATA     c->s  [ip:4][port:2 BE][payload]   send a UDP datagram
//                 s->c  [ip:4][port:2 BE][payload]   datagram received from ip:port
//   0x01 RESOLVE  c->s  [fakeip:4][hostname utf8]    bind a placeholder address to a
//                                                    hostname; the relay resolves it
//   0x02 BIND     c->s  [port:2 BE]                  open this connection's UDP socket
//                                                    on a fixed port (0 = ephemeral)
//   0x03 BOUND    s->c  [port:2 BE]                  the socket's local port (0 = failed)
//
// Each WebSocket gets its own UDP socket, so to a game server every browser
// is an ordinary client (and a browser can host a server on a bound port).
// Hostnames are resolved here because the engine resolves synchronously; the
// client uses placeholder addresses in 240.0.0.0/8 and the relay translates.

import http from 'node:http';
import dgram from 'node:dgram';
import dns from 'node:dns/promises';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));

// ---------------------------------------------------------------------------
// options

const opts = { port: 8080, host: '127.0.0.1', ports: [27000, 28000], private: true, static: true };
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '--port') opts.port = +process.argv[++i];
  else if (a === '--host') opts.host = process.argv[++i];
  else if (a === '--ports') opts.ports = process.argv[++i].split('-').map(Number);
  else if (a === '--no-private') opts.private = false;
  else if (a === '--no-static') opts.static = false;
  else { console.error(`unknown option ${a}`); process.exit(1); }
}

const MAX_PACKET = 16384;
const RATE_PER_SEC = 400;        // datagrams per second per connection (server browser pings bursts)
const MAX_NAMES = 256;

// ---------------------------------------------------------------------------
// static files

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.pk3': 'application/octet-stream', '.json': 'application/json', '.png': 'image/png',
};

function serveStatic(req, res) {
  const url = new URL(req.url, 'http://x');
  let rel = decodeURIComponent(url.pathname);
  let base = path.join(ROOT, 'dist');
  if (rel.startsWith('/data/')) {
    base = path.join(ROOT, 'data');
    rel = rel.slice('/data'.length);
  }
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.normalize(path.join(base, rel));
  if (!file.startsWith(base + path.sep)) {
    res.writeHead(403).end();
    return;
  }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) {
      res.writeHead(404).end('not found');
      return;
    }
    const headers = {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Content-Length': st.size,
      'Last-Modified': st.mtime.toUTCString(),
      'Cache-Control': base.endsWith('data') ? 'public, max-age=86400' : 'no-cache',
    };
    if (req.headers['if-modified-since'] && new Date(req.headers['if-modified-since']) >= new Date(st.mtime.toUTCString())) {
      res.writeHead(304, headers).end();
      return;
    }
    res.writeHead(200, headers);
    if (req.method === 'HEAD') res.end();
    else fs.createReadStream(file).pipe(res);
  });
}

// ---------------------------------------------------------------------------
// minimal WebSocket server (RFC 6455, binary messages only)

class WebSocketConn {
  constructor(socket) {
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.fragments = [];
    this.closed = false;
    this.onmessage = null;
    this.onclose = null;
    socket.setNoDelay(true);
    socket.on('data', (d) => this.#onData(d));
    socket.on('close', () => this.#finish());
    socket.on('error', () => this.#finish());
  }

  #finish() {
    if (this.closed) return;
    this.closed = true;
    if (this.onclose) this.onclose();
  }

  #onData(data) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, data]) : data;
    for (;;) {
      if (this.buf.length < 2) return;
      const b0 = this.buf[0], b1 = this.buf[1];
      const fin = (b0 & 0x80) !== 0, opcode = b0 & 0x0f, masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f, off = 2;
      if (len === 126) {
        if (this.buf.length < 4) return;
        len = this.buf.readUInt16BE(2); off = 4;
      } else if (len === 127) {
        if (this.buf.length < 10) return;
        len = Number(this.buf.readBigUInt64BE(2)); off = 10;
      }
      if (len > 1 << 20 || !masked) { this.close(1009); return; }
      if (this.buf.length < off + 4 + len) return;
      const mask = this.buf.subarray(off, off + 4);
      const payload = Buffer.from(this.buf.subarray(off + 4, off + 4 + len));
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      this.buf = this.buf.subarray(off + 4 + len);

      if (opcode === 0x8) { this.close(1000); return; }
      if (opcode === 0x9) { this.#sendFrame(0xA, payload); continue; }
      if (opcode === 0xA) continue;
      if (opcode !== 0x0) this.fragments = [];
      this.fragments.push(payload);
      if (fin) {
        const msg = this.fragments.length === 1 ? this.fragments[0] : Buffer.concat(this.fragments);
        this.fragments = [];
        if (this.onmessage) this.onmessage(msg);
      }
    }
  }

  #sendFrame(opcode, payload) {
    if (this.closed) return;
    const len = payload.length;
    let header;
    if (len < 126) {
      header = Buffer.from([0x80 | opcode, len]);
    } else if (len < 65536) {
      header = Buffer.alloc(4); header[0] = 0x80 | opcode; header[1] = 126; header.writeUInt16BE(len, 2);
    } else {
      header = Buffer.alloc(10); header[0] = 0x80 | opcode; header[1] = 127; header.writeBigUInt64BE(BigInt(len), 2);
    }
    this.socket.write(Buffer.concat([header, payload]));
  }

  send(buf) { this.#sendFrame(0x2, buf); }

  close(code = 1000) {
    if (this.closed) return;
    const p = Buffer.alloc(2); p.writeUInt16BE(code);
    this.#sendFrame(0x8, p);
    this.socket.end();
    this.#finish();
  }
}

function acceptWebSocket(req, socket) {
  const key = req.headers['sec-websocket-key'];
  if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return null;
  }
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  return new WebSocketConn(socket);
}

// ---------------------------------------------------------------------------
// relay

function isPrivate(ip) {
  const [a, b] = ip.split('.').map(Number);
  return a === 10 || a === 127 || a === 0 || (a === 172 && b >= 16 && b < 32) ||
         (a === 192 && b === 168) || (a === 169 && b === 254) || a >= 224;
}

let nextId = 1;

class RelaySession {
  constructor(ws, remote) {
    this.id = nextId++;
    this.ws = ws;
    this.remote = remote;
    this.udp = null;
    this.names = new Map();      // fake ip -> { real: string|null, pending: Buffer[] }
    this.reverse = new Map();    // real ip -> fake ip
    this.peers = new Set();      // 'ip:port' that sent us datagrams (replies always allowed)
    this.tokens = RATE_PER_SEC;
    this.lastRefill = Date.now();
    this.sent = 0;
    this.received = 0;
    ws.onmessage = (m) => this.onMessage(m);
    ws.onclose = () => this.close();
    log(`#${this.id} connected from ${remote}`);
  }

  openSocket(port) {
    if (this.udp) return;
    const udp = dgram.createSocket('udp4');
    this.udp = udp;
    udp.on('message', (msg, rinfo) => this.onDatagram(msg, rinfo));
    udp.on('error', (err) => {
      log(`#${this.id} udp error: ${err.message}`);
      if (!udp.boundPort) this.reply(0x03, Buffer.from([0, 0]));
    });
    udp.bind(port, () => {
      udp.boundPort = udp.address().port;
      const p = Buffer.alloc(2); p.writeUInt16BE(udp.boundPort);
      this.reply(0x03, p);
      log(`#${this.id} udp bound to port ${udp.boundPort}`);
    });
  }

  reply(type, body) {
    this.ws.send(Buffer.concat([Buffer.from([type]), body]));
  }

  allowSend() {
    const now = Date.now();
    this.tokens = Math.min(RATE_PER_SEC, this.tokens + (now - this.lastRefill) * RATE_PER_SEC / 1000);
    this.lastRefill = now;
    if (this.tokens < 1) return false;
    this.tokens--;
    return true;
  }

  onMessage(m) {
    if (!m.length) return;
    switch (m[0]) {
      case 0x00: return this.onSend(m);
      case 0x01: return this.onResolve(m);
      case 0x02: {
        if (m.length < 3) return;
        const port = m.readUInt16BE(1);
        if (port && (port < opts.ports[0] || port > opts.ports[1])) {
          this.reply(0x03, Buffer.from([0, 0]));
          return;
        }
        this.openSocket(port);
        return;
      }
    }
  }

  onResolve(m) {
    if (m.length < 6 || this.names.size >= MAX_NAMES) return;
    const fake = [...m.subarray(1, 5)].join('.');
    const host = m.subarray(5).toString('utf8');
    if (this.names.has(fake) || !/^[A-Za-z0-9.-]{1,253}$/.test(host)) return;
    const entry = { real: null, pending: [] };
    this.names.set(fake, entry);
    dns.lookup(host, { family: 4 }).then(({ address }) => {
      entry.real = address;
      this.reverse.set(address, fake);
      log(`#${this.id} resolved ${host} -> ${address}`);
      for (const p of entry.pending) this.onSend(p);
      entry.pending = [];
    }).catch((err) => {
      log(`#${this.id} cannot resolve ${host}: ${err.code || err.message}`);
      entry.pending = [];
    });
  }

  onSend(m) {
    if (m.length < 8 || m.length > MAX_PACKET + 7) return;
    let ip = [...m.subarray(1, 5)].join('.');
    const port = m.readUInt16BE(5);
    if (ip.startsWith('240.')) {
      const entry = this.names.get(ip);
      if (!entry) return;
      if (!entry.real) {
        if (entry.pending.length < 32) entry.pending.push(m);
        return;
      }
      ip = entry.real;
    }
    // unsolicited traffic is limited to game-server ports; replies to anyone
    // who contacted this socket (e.g. clients of a server hosted in the tab)
    // are always allowed
    if (!this.peers.has(`${ip}:${port}`)) {
      if (port < opts.ports[0] || port > opts.ports[1]) return;
      if (!opts.private && isPrivate(ip)) return;
    }
    if (!this.allowSend()) return;
    this.openSocket(0);
    this.sent++;
    this.udp.send(m.subarray(7), port, ip);
  }

  onDatagram(msg, rinfo) {
    if (this.ws.closed) return;
    if (this.peers.size < 4096) this.peers.add(`${rinfo.address}:${rinfo.port}`);
    const ip = this.reverse.get(rinfo.address) || rinfo.address;
    const header = Buffer.alloc(7);
    header[0] = 0x00;
    ip.split('.').forEach((v, i) => { header[1 + i] = +v; });
    header.writeUInt16BE(rinfo.port, 5);
    this.received++;
    this.ws.send(Buffer.concat([header, msg]));
  }

  close() {
    if (this.udp) {
      try { this.udp.close(); } catch { /* already closed */ }
      this.udp = null;
    }
    log(`#${this.id} closed (sent ${this.sent}, received ${this.received})`);
  }
}

function log(msg) {
  console.log(`[${new Date().toISOString().slice(11, 19)}] ${msg}`);
}

// ---------------------------------------------------------------------------
// /dl?url=... : proxy for servers' HTTP pk3 downloads (sv_wwwBaseURL), which
// browsers can't fetch directly because download hosts don't send CORS
// headers. Only http(s) URLs to .pk3 files on public hosts are allowed.

const MAX_DOWNLOAD = 512 * 1024 * 1024;

async function checkDownloadURL(raw) {
  let u;
  try { u = new URL(raw); } catch { return null; }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
  if (!/\.pk3$/i.test(u.pathname)) return null;
  try {
    const { address } = await dns.lookup(u.hostname, { family: 4 });
    if (isPrivate(address) && !opts.private) return null;
  } catch { return null; }
  return u;
}

async function proxyDownload(req, res) {
  let target = new URL(req.url, 'http://x').searchParams.get('url');
  try {
    for (let hop = 0; hop < 4; hop++) {
      const u = await checkDownloadURL(target);
      if (!u) { res.writeHead(403).end('forbidden'); return; }
      const up = await fetch(u, { redirect: 'manual', headers: { 'User-Agent': 'ET 2.60b web' } });
      if (up.status >= 300 && up.status < 400 && up.headers.get('location')) {
        target = new URL(up.headers.get('location'), u).toString();
        continue;
      }
      const len = +up.headers.get('content-length') || 0;
      if (!up.ok || len > MAX_DOWNLOAD) { res.writeHead(502).end(`upstream ${up.status}`); return; }
      log(`dl ${u} (${len || '?'} bytes)`);
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', ...(len ? { 'Content-Length': len } : {}) });
      let total = 0;
      for await (const chunk of up.body) {
        total += chunk.length;
        if (total > MAX_DOWNLOAD) { res.destroy(); return; }
        if (!res.write(chunk)) await new Promise((r) => res.once('drain', r));
      }
      res.end();
      return;
    }
    res.writeHead(508).end('too many redirects');
  } catch (err) {
    log(`dl failed: ${err.message}`);
    if (!res.headersSent) res.writeHead(502).end('download failed');
    else res.destroy();
  }
}

// ---------------------------------------------------------------------------

const server = http.createServer((req, res) => {
  if (req.url.startsWith('/dl?')) {
    proxyDownload(req, res);
    return;
  }
  if (!opts.static) {
    res.writeHead(404).end();
    return;
  }
  serveStatic(req, res);
});

server.on('upgrade', (req, socket) => {
  if (new URL(req.url, 'http://x').pathname !== '/relay') {
    socket.end('HTTP/1.1 404 Not Found\r\n\r\n');
    return;
  }
  const ws = acceptWebSocket(req, socket);
  if (ws) new RelaySession(ws, `${socket.remoteAddress}:${socket.remotePort}`);
});

server.listen(opts.port, opts.host, () => {
  log(`http://${opts.host === '0.0.0.0' ? 'localhost' : opts.host}:${opts.port}/  ` +
      `(static: ${opts.static ? 'dist/ + data/' : 'off'}, relay: /relay, ` +
      `udp ports ${opts.ports[0]}-${opts.ports[1]}, private targets ${opts.private ? 'allowed' : 'blocked'})`);
});

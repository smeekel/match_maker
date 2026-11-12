// Browser client for the matchmaker: HTTPS for auth and lobby discovery, one
// WebTransport session for everything inside a lobby. Uses fetch and
// WebTransport only.

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export class MatchmakerError extends Error {
  constructor(status, body) {
    super(body?.message || body?.error || `HTTP ${status}`);
    this.name = 'MatchmakerError';
    this.status = status;
    this.code = body?.error ?? null;
    this.body = body;
  }
}

export function uuidToBytes(uuid) {
  const hex = uuid.replace(/-/g, '');
  if (!/^[0-9a-fA-F]{32}$/.test(hex)) {
    throw new TypeError(`not a uuid: ${uuid}`);
  }
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function bytesToUuid(bytes) {
  const hex = Array.from(bytes.subarray(0, 16), (b) => b.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function toHashes(hashes) {
  if (!hashes) {
    return undefined;
  }
  return hashes.map((hash) => {
    if (typeof hash !== 'string') {
      return hash;
    }
    const hex = hash.replace(/[^0-9a-fA-F]/g, '');
    const value = new Uint8Array(hex.length / 2);
    for (let i = 0; i < value.length; i++) {
      value[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    }
    return { algorithm: 'sha-256', value };
  });
}

export class MatchmakerClient {
  // baseUrl: HTTPS origin of the matchmaker, e.g. "https://mm.example.com".
  // serverCertificateHashes: optional SHA-256 hashes (hex strings or
  //   WebTransportHash objects) for a self-signed development certificate.
  // webTransportOrigin: optional override for the WebTransport origin when
  //   the ticket url does not match how the browser reaches the server.
  constructor({ baseUrl, serverCertificateHashes, webTransportOrigin } = {}) {
    this.baseUrl = (baseUrl || '').replace(/\/+$/, '');
    this.serverCertificateHashes = toHashes(serverCertificateHashes);
    this.webTransportOrigin = webTransportOrigin ? webTransportOrigin.replace(/\/+$/, '') : null;
    this.accessToken = null;
    this.userId = null;
    this.gameId = null;
  }

  async request(method, path, body) {
    const headers = {};
    if (this.accessToken) {
      headers.Authorization = `Bearer ${this.accessToken}`;
    }
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
    }
    const res = await fetch(this.baseUrl + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await res.text();
    const parsed = text ? JSON.parse(text) : {};
    if (!res.ok) {
      throw new MatchmakerError(res.status, parsed);
    }
    return parsed;
  }

  config() {
    return this.request('GET', '/api/v1/config');
  }

  async auth(gameId, token, { alias, externalId } = {}) {
    const body = { game_id: gameId, token };
    if (alias) {
      body.alias = alias;
    }
    if (externalId) {
      body.external_id = externalId;
    }
    const res = await this.request('POST', '/api/v1/auth', body);
    this.accessToken = res.access_token;
    this.userId = res.user_id;
    this.gameId = res.game_id;
    return res;
  }

  listLobbies({ cursor, limit } = {}) {
    const query = new URLSearchParams();
    if (cursor) {
      query.set('cursor', cursor);
    }
    if (limit) {
      query.set('limit', String(limit));
    }
    const suffix = query.size ? `?${query}` : '';
    return this.request('GET', `/api/v1/lobbies${suffix}`);
  }

  getLobby(lobbyId, { clientTs } = {}) {
    const suffix = clientTs === undefined ? '' : `?client_ts=${encodeURIComponent(clientTs)}`;
    return this.request('GET', `/api/v1/lobbies/${encodeURIComponent(lobbyId)}${suffix}`);
  }

  lobbyByCode(code) {
    return this.request('GET', `/api/v1/lobbies/by-code/${encodeURIComponent(code)}`);
  }

  createLobby() {
    return this.request('POST', '/api/v1/lobbies', {});
  }

  joinLobby(lobbyId, { password } = {}) {
    const body = password === undefined ? {} : { password };
    return this.request('POST', `/api/v1/lobbies/${encodeURIComponent(lobbyId)}/join`, body);
  }

  // Opens the lobby session for the lobby this user created or joined. Must
  // run before the join reservation expires.
  async connect() {
    const ticket = await this.request('POST', '/api/v1/wt-ticket', {});
    let url = ticket.url;
    if (this.webTransportOrigin) {
      const parsed = new URL(url);
      url = this.webTransportOrigin + parsed.pathname + parsed.search;
    }
    const options = {};
    if (this.serverCertificateHashes) {
      options.serverCertificateHashes = this.serverCertificateHashes;
    }
    const transport = new WebTransport(url, options);
    await transport.ready;
    const session = new LobbySession(transport, ticket.lobby_id, this.userId);
    await session.start();
    return session;
  }
}

// Events dispatched on a LobbySession:
//   "snapshot"  detail: the lobby_snapshot
//   "event"     detail: any {"op":"event"} message (chat, user_entered, ...)
//   "message"   detail: every control message, including replies
//   "datagram"  detail: { channelId, data } for a wt relay channel
//   "close"     detail: { reason }
export class LobbySession extends EventTarget {
  constructor(transport, lobbyId, userId) {
    super();
    this.transport = transport;
    this.lobbyId = lobbyId;
    this.userId = userId;
    this.snapshot = null;
    this.channels = new Map();
    this.closed = false;
    this.nextId = 1;
    this.pending = new Map();
    this.writer = null;
    this.datagramWriter = null;
    this.heartbeatTimer = null;
  }

  async start() {
    const incoming = this.transport.incomingBidirectionalStreams.getReader();
    const { value: stream, done } = await incoming.read();
    incoming.releaseLock();
    if (done || !stream) {
      throw new Error('the server did not open the control stream');
    }
    this.writer = stream.writable.getWriter();
    this.datagramWriter = this.transport.datagrams.writable.getWriter();
    const snapshot = new Promise((resolve, reject) => {
      this.addEventListener('snapshot', (e) => resolve(e.detail), { once: true });
      this.addEventListener('close', () => reject(new Error('session closed')), { once: true });
    });
    this.readControl(stream.readable.getReader());
    this.readDatagrams(this.transport.datagrams.readable.getReader());
    this.transport.closed
      .then(() => this.finish('closed'))
      .catch((err) => this.finish(err?.message || 'failed'));
    await snapshot;
  }

  async readControl(reader) {
    let buffer = new Uint8Array(0);
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) {
          break;
        }
        const merged = new Uint8Array(buffer.length + value.length);
        merged.set(buffer);
        merged.set(value, buffer.length);
        buffer = merged;
        while (buffer.length >= 4) {
          const length = new DataView(buffer.buffer, buffer.byteOffset, 4).getUint32(0);
          if (buffer.length < 4 + length) {
            break;
          }
          const text = decoder.decode(buffer.subarray(4, 4 + length));
          buffer = buffer.slice(4 + length);
          this.onMessage(JSON.parse(text));
        }
      }
    } catch (err) {
      this.finish(err?.message || 'control stream failed');
      return;
    }
    this.finish('control stream ended');
  }

  async readDatagrams(reader) {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) {
          return;
        }
        if (value.length < 16) {
          continue;
        }
        const channelId = bytesToUuid(value);
        this.dispatchEvent(
          new CustomEvent('datagram', { detail: { channelId, data: value.subarray(16) } }),
        );
      }
    } catch {
      // The transport closing ends this loop; "close" reports it.
    }
  }

  onMessage(message) {
    if (message.op === 'ping') {
      this.send({ op: 'pong', id: message.id });
      return;
    }
    this.dispatchEvent(new CustomEvent('message', { detail: message }));
    if (message.op === 'lobby_snapshot') {
      this.snapshot = message;
      this.dispatchEvent(new CustomEvent('snapshot', { detail: message }));
      return;
    }
    if (message.op === 'event') {
      if (message.event === 'relay_allocated' && message.protocol === 'wt') {
        this.channels.set(message.channel_id, message);
      } else if (message.event === 'relay_released' || message.event === 'relay_expired') {
        this.channels.delete(message.channel_id);
      }
      this.dispatchEvent(new CustomEvent('event', { detail: message }));
      return;
    }
    if (message.id !== undefined && this.pending.has(message.id)) {
      const { resolve, reject } = this.pending.get(message.id);
      this.pending.delete(message.id);
      if (message.op === 'error') {
        reject(Object.assign(new Error(message.message || message.error), { reply: message }));
      } else {
        resolve(message);
      }
    }
  }

  send(message) {
    if (this.closed) {
      return Promise.reject(new Error('session closed'));
    }
    const body = encoder.encode(JSON.stringify(message));
    const framed = new Uint8Array(4 + body.length);
    new DataView(framed.buffer).setUint32(0, body.length);
    framed.set(body, 4);
    return this.writer.write(framed);
  }

  // Sends one lobby op and resolves with its "ok" (or other) reply.
  request(op, fields = {}) {
    const id = `r${this.nextId++}`;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.send({ ...fields, op, id }).catch((err) => {
        this.pending.delete(id);
        reject(err);
      });
    });
  }

  chat(text) {
    return this.request('chat', { text });
  }

  setPhase(phase) {
    return this.request('set_phase', { phase });
  }

  leave() {
    return this.request('leave');
  }

  // Host only: one wt channel between the host and |peerUserId|.
  allocateWt(peerUserId, lifetimeSec = 300) {
    return this.request('allocate_relays', {
      channels: [{ a: this.userId, b: peerUserId, protocol: 'wt', lifetime_sec: lifetimeSec }],
    });
  }

  releaseRelays(ids) {
    return this.request('release_relays', ids ? { ids } : {});
  }

  // Sends game bytes on a wt relay channel. Unreliable and unordered; the
  // server drops payloads larger than max_udp_packet_bytes.
  sendDatagram(channelId, data) {
    const payload = typeof data === 'string' ? encoder.encode(data) : new Uint8Array(data);
    const datagram = new Uint8Array(16 + payload.length);
    datagram.set(uuidToBytes(channelId));
    datagram.set(payload, 16);
    return this.datagramWriter.write(datagram);
  }

  // Host only: keeps the lobby alive with the heartbeat op.
  startHeartbeat(intervalMs) {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      this.send({ op: 'heartbeat' }).catch(() => {});
    }, intervalMs);
  }

  stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  close() {
    if (!this.closed) {
      this.transport.close({ closeCode: 0, reason: 'bye' });
    }
    this.finish('closed by client');
  }

  finish(reason) {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.stopHeartbeat();
    for (const { reject } of this.pending.values()) {
      reject(new Error(reason));
    }
    this.pending.clear();
    this.dispatchEvent(new CustomEvent('close', { detail: { reason } }));
  }
}

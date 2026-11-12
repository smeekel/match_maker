# Matchmaker protocol

One process hosts zero or more games. Clients talk only to this process. Control uses a single TLS listener (HTTP/1.1), plus an optional HTTP/3 listener for WebTransport. Game payloads use relay ports allocated from that process, or WebTransport datagrams. There is no hole punching and no player UI.

A client uses HTTPS until it enters a lobby, holds one WebSocket for the whole lobby stay, and returns to HTTPS when that socket closes. Releasing relay ports does not close the lobby socket.

When `relay_wt_enabled` is set, a client may instead hold one WebTransport session (HTTP/3 over QUIC) for the lobby stay. That session carries the same lobby ops as the WebSocket and also carries game bytes on `wt` relay channels as datagrams, so a browser needs nothing but `fetch` and `WebTransport`. HTTPS, the WebSocket, and UDP/TCP relay ports are unchanged. A lobby may mix both kinds of members, but game bytes never cross between the two transports.

## Startup

The only value outside the database is the SQLite path: `--database <path>` or `MATCHMAKER_DATABASE`. The process applies migrations, loads config and games, then binds the address in `listen`. Certificate bytes stay in the PEM files named by `tls_cert` and `tls_key`. `max_threads` (default 1) is how many threads run the process; a value below 1 runs as 1. `SIGHUP` (and `SIGBREAK` on Windows) reloads config and games. Listen address, TLS files, and the worker count stay as they were at startup. Existing relay channels keep the lifetime they were granted. `SIGTERM` and `SIGINT` close lobby sockets and exit.

When `relay_wt_enabled` is set at startup, the process also binds `wt_listen` (UDP, default `0.0.0.0:443`) for HTTP/3 with the same certificate and key, and startup fails if that bind fails. Reload does not start, stop, or rebind it. QUIC connections count toward `max_connections`.

Restarting the process drops sessions, lobbies, and forwarders. A JWT from a previous process is rejected because `boot_id` changes.

## HTTPS

JSON in, JSON out. Authenticated routes use `Authorization: Bearer <jwt>`. A token in the query string is rejected. Errors are:

```json
{"error":"<code>","message":"..."}
```

Status codes: 400, 401, 403, 404, 409, 413, 429. Codes include `unauthorized`, `unknown_game`, `not_host`, `not_member`, `lobby_not_open`, `late_join_disabled`, `full`, `bad_password`, `invalid_code`, `code_taken`, `limit`, `lifetime_exceeds_limit` (with `max_lifetime_sec`), `too_large`, `unknown_user`, `already_in_lobby`, `not_found`, `forbidden`.

When `Accept-Encoding` includes `gzip` and the body is at least `http_gzip_min_bytes`, the response is gzip and sets `Content-Encoding: gzip` and `Vary: Accept-Encoding`.

### CORS

`cors_origins` is a comma- or space-separated list of exact origins such as `https://game.example`, or `*` for any origin. When it is empty, the server sends no CORS headers and does not check `Origin`. When it is set:

- A request with no `Origin` header (a native client) is served as before.
- A request whose `Origin` is not listed gets 403 `forbidden`. This covers HTTPS routes, the WebSocket upgrade, and the WebTransport `CONNECT`.
- Responses to a listed origin carry `Access-Control-Allow-Origin` with that origin and `Vary: Origin`.
- `OPTIONS` from a listed origin is a preflight: 204 with `Access-Control-Allow-Methods: GET, POST, OPTIONS`, `Access-Control-Allow-Headers: Authorization, Content-Type`, and `Access-Control-Max-Age: 600`.

### `GET /api/v1/health`

No auth. `{"status":"ok","region":"<id>"}`.

### `GET /api/v1/config`

No auth. Region, the region list, advertised compression, whether the WebTransport listener is running, and `limits.max_message_bytes`. No secrets.

```json
{
  "health": {"status": "ok"},
  "region": {"id": "local", "public_url": "https://localhost"},
  "regions": [{"id": "local", "public_url": "https://localhost"}],
  "compression": {"http": ["gzip"], "websocket": ["permessage-deflate"]},
  "webtransport": {"enabled": false},
  "limits": {"max_message_bytes": 65536}
}
```

An empty array means that method is disabled.

### `POST /api/v1/auth`

```json
{"game_id":"<uuid>","token":"<game token>","external_id":"optional","alias":"optional"}
```

The token is compared in constant time to the game's SHA-256 hash. Each call creates a new in-memory user. `external_id` is a label, not an account. Failures are rate-limited per source address.

```json
{"token_type":"Bearer","access_token":"<jwt>","expires_in":900,"user_id":"<uuid>","game_id":"<uuid>"}
```

JWT is HS256. Claims: `sub` (user id), `game_id`, `iat`, `exp`, `jti`, `boot_id`. Default TTL is 15 minutes. The lobby socket stays up after the JWT expires; the next HTTPS call needs a new auth.

### Lobbies

`GET /api/v1/lobbies?limit&cursor` lists lobbies a voluntary player may join: phase `open`, or `in_progress` when the game and the host both allow late join. Default limit 50, max 200. Each entry has `lobby_id`, `host_id`, `phase`, `slots`, `member_count`, `locked`, `allow_late_join`, `short_code` (`null` when unset), and `metadata`. Games advertise modes and other rules through that object; the service stores it and returns it, and does not interpret the keys.

`GET /api/v1/lobbies/{id}?client_ts=` returns `lobby` (including metadata), `server_ts`, `client_ts` when supplied, and `host_control_rtt_ms` when the host socket answers a probe. This does not join.

`GET /api/v1/lobbies/by-code/{code}` is the same lookup by short code. Comparison is case-insensitive and finds a live lobby whether or not it is listed.

Estimated relay RTT is the client's own HTTPS round trip plus `host_control_rtt_ms`.

`POST /api/v1/lobbies` creates a closed lobby and reserves the caller as host. Slots start at the game player cap. The caller then opens the lobby WebSocket. A user who already has a lobby gets `already_in_lobby`.

`POST /api/v1/lobbies/{id}/join` with `{"password":"..."}` checks the lock and holds a free slot for `join_hold_sec` (default 10). The WebSocket upgrade or the WebTransport `CONNECT` completes the hold. A voluntary connection without a hold is rejected.

### `POST /api/v1/wt-ticket`

Bearer auth, empty body. Issues a one-time ticket for the WebTransport `CONNECT`, because a browser cannot set `Authorization` on a WebTransport handshake. The caller must hold a reservation (host or join hold); otherwise the reply is `not_member`. When the listener is not running the reply is 404 `not_found`.

```json
{"ticket":"<32 hex chars>","expires_in":30,"lobby_id":"<uuid>","url":"https://match.example.com/wt/v1/lobbies/<uuid>?ticket=<ticket>"}
```

The ticket is held in memory, bound to the calling user, usable once, and lives `wt_ticket_sec` (default 30). `url` is built from the host in `public_url` and the bound `wt_listen` port (omitted when 443).

## One lobby

A user id is in at most one lobby. The reservation starts at create or at a successful HTTPS join. It ends when they leave, the lobby ends, the join hold expires, or the host admission expires before they connect. A second auth is a different user. Every lobby is scoped to the session's `game_id`.

## Join rules

Phase is `closed`, `open`, or `in_progress`. The host sets it. Allocating relays does not change the phase. Create starts `closed`.

The effective slot cap is the minimum of the host's number, the game `max_players_per_lobby`, and the system `max_players_per_lobby`. The host cannot set slots below the current members plus outstanding holds, or above the game cap.

A voluntary join of an `open` lobby needs a free slot and a passing lock check. A voluntary join of an `in_progress` lobby also needs the game `allow_join_in_progress` and the host `allow_late_join`. A `closed` lobby rejects voluntary joins. The host reservation skips the phase and lock checks and still consumes a slot.

Lock is one in-memory password. Null or empty clears it. The password is never listed, snapshotted, or logged. A wrong or missing password on a locked lobby is `bad_password`.

Short code is optional, 1 to 16 characters from `A-Za-z0-9`, unique among live lobbies in that game, compared case-insensitively. Null or empty disables it. A taken code is `code_taken`.

## Lobby WebSocket

`GET /ws/v1/lobbies/{id}` with the bearer header, upgraded to WebSocket. The upgrade completes a host reservation or an HTTPS join hold. A user who is already connected, over either transport, gets 409 `already_in_lobby`.

Text frames are one JSON object. When the client offers `permessage-deflate`, the server accepts it with no context takeover, compression level `websocket_deflate_level` (default 3), and `server_max_window_bits` (default 12).

On success the joiner receives:

```json
{"op":"lobby_snapshot","lobby_id":"...","host_id":"...","phase":"closed","slots":4,"member_count":1,"locked":false,"allow_late_join":false,"short_code":null,"metadata":{},"members":[{"user_id":"...","alias":"...","external_id":"...","ready":false,"connected":true,"transport":"websocket"}]}
```

`transport` is `websocket` or `webtransport` for a connected member and `null` for a member who still holds a reservation.

Other connected members receive `{"op":"event","event":"user_entered","lobby_id":"...","user_id":"...","alias":"...","transport":"websocket"}`.

Requests may include `id`. `ok` and `error` echo it.

| op | who | fields |
| --- | --- | --- |
| `set_alias` | member | `alias` |
| `set_ready` | member | `ready` |
| `chat` | member | `text` |
| `custom` | member | `name`, `data`, optional `target_user_id` |
| `heartbeat` | host | reply is `heartbeat_ack` with `server_ts` |
| `set_metadata` | host | `metadata` object; null deletes a key |
| `set_phase` | host | `phase` |
| `set_slots` | host | `slots` |
| `set_late_join` | host | `allowed` |
| `set_password` | host | `password` or null |
| `set_short_code` | host | `code` or null |
| `allocate_relays` | host | `channels`: `{a, b, protocol, lifetime_sec}`. One endpoint is the host and the other is a different member. |
| `release_relays` | host | optional `ids`; omitted releases every channel in the lobby |
| `close_lobby` | host | |
| `leave` | member | |

`allocate_relays` replies with `op: ok` and `allocations` (ids, ports, expiry, user ids; no tokens). Each user also receives `relay_allocated` containing only that user's token:

```json
{"op":"event","event":"relay_allocated","lobby_id":"...","channel_id":"...","protocol":"udp","peer_user_id":"...","host":"127.0.0.1","port":30000,"token":"<32 hex chars>","expires_at_ms":0}
```

Other events, while the socket is open: `host_changed`, `lobby_metadata`, `lobby_phase_changed`, `slots_changed`, `late_join_changed`, `lock_changed`, `short_code_changed`, `alias_changed`, `ready_changed`, `chat`, `custom`, `relay_released`, `relay_expired`. Events are not echoed to the actor, except `lobby_closed`, which goes to every connected member and then the socket closes.

Leaving is any of: `leave`, socket close, missed WebSocket pong, `close_lobby`, host-leave teardown, or the last member departing. The server removes the member, emits `user_left` or `lobby_closed`, closes that WebSocket, and frees the one-lobby slot.

If `promote_host_on_leave` is false, host leave ends the lobby. If it is true, the earliest remaining member becomes host. The last member always ends the lobby. Ending a lobby releases its relays. Missing the allowed number of host heartbeat intervals uses the same rule.

A full outbound queue disconnects that client.

## Lobby WebTransport session

Available when `relay_wt_enabled` is set. The flow is:

1. HTTPS: auth, list, then create or join, exactly as above.
2. `POST /api/v1/wt-ticket`.
3. Extended `CONNECT` with `:protocol` = `webtransport` to `/wt/v1/lobbies/{id}?ticket=<ticket>` on `wt_listen`, ALPN `h3`. In a browser this is `new WebTransport(url)`.

The `CONNECT` applies the same checks as the WebSocket upgrade: the ticket must be live and unused, the user must hold a reservation for that lobby, the user must not already be connected over either transport, and `Origin` must pass the CORS rule. A JWT in the query string (`access_token`, `token`, or a dotted ticket) is rejected with 401. Refusals are an HTTP status with the usual error body; success is 200. The server answers `sec-webtransport-http3-draft: draft02` when the client offers `sec-webtransport-http3-draft02`.

One session per QUIC connection. After the 200, the server opens one bidirectional stream: the control stream. Any stream the client opens is reset. Browsers receive the control stream from `incomingBidirectionalStreams`.

### Control stream

Every message in both directions is a big-endian uint32 byte length followed by that many bytes of one UTF-8 JSON object:

```
00 00 00 1d  {"op":"chat","text":"hello"}
```

The length may not exceed `max_message_bytes`; a longer frame ends the session as a departure. The first message from the server is the `lobby_snapshot`. After that, requests, replies, and events are identical to the lobby WebSocket: the same op table, the same `id` echo, the same events, and the same teardown rules. A message that is not a JSON object with `op` gets `error` `bad_request`.

When the HTTPS get-lobby call probes a host on WebTransport, the server sends `{"op":"ping","id":"<hex>"}` on the control stream, and the client must answer `{"op":"pong","id":"<same>"}`. The pong is consumed by the server and never reaches the op handler. The host heartbeat is still the `heartbeat` op. The QUIC idle timeout is twice the host heartbeat window (at least 10 seconds), and the server sends keep-alive pings, so a quiet lobby stays up until heartbeat misses end it.

Leaving matches the WebSocket: the `leave` op, `close_lobby`, a `CLOSE_WEBTRANSPORT_SESSION` capsule or FIN on the `CONNECT` stream, a reset of the control stream, a lost or idle connection, or a full outbound queue. When the server ends a session after `leave` or `lobby_closed`, it finishes the control stream first, then sends `CLOSE_WEBTRANSPORT_SESSION`, and closes the QUIC connection about two seconds later.

### Datagrams

HTTP datagrams carry game bytes on `wt` relay channels. Inside the session (after the quarter stream id that WebTransport adds and browsers hide) each datagram is the 16 raw bytes of the channel UUID followed by the payload:

```
<16-byte channel id><payload>
```

The server forwards the datagram unchanged, prefix included, to the other member of that channel. It drops, without reply, a datagram that is shorter than 16 bytes, names an unknown or released channel, comes from a user who is not on the channel, has a payload larger than `max_udp_packet_bytes`, or exceeds `max_bytes_per_sec_per_channel`. Drops are counted in stats. Datagrams are unreliable and unordered, like UDP.

## Relay

The host allocates bidirectional channels. Every channel connects the host to one other connected member, so players who are not the host reach each other through the host. A lobby can hold at most as many channels as it has player slots. A requested lifetime above the minimum of the game cap and the system cap fails with `lifetime_exceeds_limit` and `max_lifetime_sec`. Idle timeout is the minimum of the game and system values. One channel uses two ports. Bind failure rolls back. The forward path does not write SQLite.

UDP: the first datagram is exactly the 16-byte token and pins the source. Later datagrams from that source are forwarded verbatim. A later datagram that is only the token rebinds the pin and is not forwarded. Datagrams before the peer has pinned, or larger than `max_udp_packet_bytes`, are dropped.

TCP is used only when system `relay_tcp_enabled` and the game `allow_tcp` are both set. The first 16 bytes are the token; the rest of the byte stream is spliced. One connection per side.

`wt` is used only when system `relay_wt_enabled` and the game `allow_wt` are both set, and only when both users on the channel are connected over WebTransport; otherwise the allocation fails with `limit` (disabled) or `bad_request` (a user is on the WebSocket) and nothing is opened. The usual host, peer, slot, and lifetime checks apply. A `wt` channel binds no ports and issues no tokens. Its `allocations` entry has `user_id` without `port` for each side, and `relay_allocated` omits `host`, `port`, and `token`:

```json
{"op":"event","event":"relay_allocated","lobby_id":"...","channel_id":"...","protocol":"wt","peer_user_id":"...","expires_at_ms":0}
```

Bytes then flow as datagrams on each user's WebTransport session (see Datagrams above).

`release_relays` emits `relay_released` and leaves both clients on the lobby socket or session. Idle or lifetime expiry emits `relay_expired`. For `wt` channels these events go out on the control stream, and the session stays open.

## Stats

`stats_listen` is a localhost plaintext port. A connection receives one JSON document and the server closes:

```json
{"connections":0,"lobbies":0,"channels":0,"forwarded_bytes":0,"drops":0,"games":[{"id":"...","lobbies":0}]}
```

## What is stored

SQLite holds config, regions, games, and operational logs. Sessions, lobbies, and forwarders are memory only. Logs record startup, shutdown, config reload, auth failure (game id only), lobby create and close, relay allocate and release, and errors. Chat, ready, custom, alias, and forwarded packets are not logged.

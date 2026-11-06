# Matchmaker protocol

One process hosts zero or more games. Clients talk only to this process. Control uses a single TLS listener (HTTP/1.1). Game payloads use relay ports allocated from that process. There is no hole punching and no player UI.

A client uses HTTPS until it enters a lobby, holds one WebSocket for the whole lobby stay, and returns to HTTPS when that socket closes. Releasing relay ports does not close the lobby socket.

## Startup

The only value outside the database is the SQLite path: `--database <path>` or `MATCHMAKER_DATABASE`. The process applies migrations, loads config and games, then binds the address in `listen`. Certificate bytes stay in the PEM files named by `tls_cert` and `tls_key`. `max_threads` (default 1) is how many threads run the process; a value below 1 runs as 1. `SIGHUP` (and `SIGBREAK` on Windows) reloads config and games. Listen address, TLS files, and the worker count stay as they were at startup. Existing relay channels keep the lifetime they were granted. `SIGTERM` and `SIGINT` close lobby sockets and exit.

Restarting the process drops sessions, lobbies, and forwarders. A JWT from a previous process is rejected because `boot_id` changes.

## HTTPS

JSON in, JSON out. Authenticated routes use `Authorization: Bearer <jwt>`. A token in the query string is rejected. Errors are:

```json
{"error":"<code>","message":"..."}
```

Status codes: 400, 401, 403, 404, 409, 413, 429. Codes include `unauthorized`, `unknown_game`, `not_host`, `not_member`, `lobby_not_open`, `late_join_disabled`, `full`, `bad_password`, `invalid_code`, `code_taken`, `limit`, `lifetime_exceeds_limit` (with `max_lifetime_sec`), `too_large`, `unknown_user`, `already_in_lobby`, `not_found`.

When `Accept-Encoding` includes `gzip` and the body is at least `http_gzip_min_bytes`, the response is gzip and sets `Content-Encoding: gzip` and `Vary: Accept-Encoding`.

### `GET /api/v1/health`

No auth. `{"status":"ok","region":"<id>"}`.

### `GET /api/v1/config`

No auth. Region, the region list, advertised compression, and `limits.max_message_bytes`. No secrets.

```json
{
  "health": {"status": "ok"},
  "region": {"id": "local", "public_url": "https://localhost"},
  "regions": [{"id": "local", "public_url": "https://localhost"}],
  "compression": {"http": ["gzip"], "websocket": ["permessage-deflate"]},
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

`POST /api/v1/lobbies/{id}/join` with `{"password":"..."}` checks the lock and holds a free slot for `join_hold_sec` (default 10). The WebSocket upgrade completes the hold. A voluntary upgrade without a hold is rejected.

## One lobby

A user id is in at most one lobby. The reservation starts at create or at a successful HTTPS join. It ends when they leave, the lobby ends, the join hold expires, or the host admission expires before they connect. A second auth is a different user. Every lobby is scoped to the session's `game_id`.

## Join rules

Phase is `closed`, `open`, or `in_progress`. The host sets it. Allocating relays does not change the phase. Create starts `closed`.

The effective slot cap is the minimum of the host's number, the game `max_players_per_lobby`, and the system `max_players_per_lobby`. The host cannot set slots below the current members plus outstanding holds, or above the game cap.

A voluntary join of an `open` lobby needs a free slot and a passing lock check. A voluntary join of an `in_progress` lobby also needs the game `allow_join_in_progress` and the host `allow_late_join`. A `closed` lobby rejects voluntary joins. The host reservation skips the phase and lock checks and still consumes a slot.

Lock is one in-memory password. Null or empty clears it. The password is never listed, snapshotted, or logged. A wrong or missing password on a locked lobby is `bad_password`.

Short code is optional, 1 to 16 characters from `A-Za-z0-9`, unique among live lobbies in that game, compared case-insensitively. Null or empty disables it. A taken code is `code_taken`.

## Lobby WebSocket

`GET /ws/v1/lobbies/{id}` with the bearer header, upgraded to WebSocket. The upgrade completes a host reservation or an HTTPS join hold.

Text frames are one JSON object. When the client offers `permessage-deflate`, the server accepts it with no context takeover, compression level `websocket_deflate_level` (default 3), and `server_max_window_bits` (default 12).

On success the joiner receives:

```json
{"op":"lobby_snapshot","lobby_id":"...","host_id":"...","phase":"closed","slots":4,"member_count":1,"locked":false,"allow_late_join":false,"short_code":null,"metadata":{},"members":[{"user_id":"...","alias":"...","external_id":"...","ready":false,"connected":true}]}
```

Other connected members receive `{"op":"event","event":"user_entered",...}`.

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

## Relay

The host allocates bidirectional channels. Every channel connects the host to one other connected member, so players who are not the host reach each other through the host. A lobby can hold at most as many channels as it has player slots. A requested lifetime above the minimum of the game cap and the system cap fails with `lifetime_exceeds_limit` and `max_lifetime_sec`. Idle timeout is the minimum of the game and system values. One channel uses two ports. Bind failure rolls back. The forward path does not write SQLite.

UDP: the first datagram is exactly the 16-byte token and pins the source. Later datagrams from that source are forwarded verbatim. A later datagram that is only the token rebinds the pin and is not forwarded. Datagrams before the peer has pinned, or larger than `max_udp_packet_bytes`, are dropped.

TCP is used only when system `relay_tcp_enabled` and the game `allow_tcp` are both set. The first 16 bytes are the token; the rest of the byte stream is spliced. One connection per side.

`release_relays` emits `relay_released` and leaves both clients on the lobby socket. Idle or lifetime expiry emits `relay_expired`.

## Stats

`stats_listen` is a localhost plaintext port. A connection receives one JSON document and the server closes:

```json
{"connections":0,"lobbies":0,"channels":0,"forwarded_bytes":0,"drops":0,"games":[{"id":"...","lobbies":0}]}
```

## What is stored

SQLite holds config, regions, games, and operational logs. Sessions, lobbies, and forwarders are memory only. Logs record startup, shutdown, config reload, auth failure (game id only), lobby create and close, relay allocate and release, and errors. Chat, ready, custom, alias, and forwarded packets are not logged.

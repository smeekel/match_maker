# matchmaker

One process hosts lobbies for any number of games. Clients speak HTTPS and a lobby WebSocket to this process. Game payloads go through relay ports the process allocates, always between the host and one other member. There is no hole punching and no player UI.

Browsers, or any client that prefers it, can replace the lobby WebSocket and the relay ports with one WebTransport session: HTTPS for auth and lobby discovery, then HTTP/3 for lobby ops and datagrams. See [WebTransport](#webtransport).

Sessions, lobbies, and forwarders live in memory. SQLite holds configuration, games, and operational logs. Restarting the process drops every live session, and a JWT from a previous process is rejected.

The wire API is specified in [PROTOCOL.md](PROTOCOL.md).

## Requirements

- Windows
- CMake 3.28 or newer
- MSVC with `/std:c++latest` (Visual Studio's C++26 mode)
- OpenSSL, default install at `C:/Program Files/OpenSSL-Win64`, using the VC x64 MD libraries (`lib/VC/x64/MD`). Override the root with `-DOPENSSL_ROOT_DIR=...` if it lives elsewhere.

The first configure downloads Boost 1.90 (Beast and Asio), the SQLite amalgamation, nlohmann/json, zlib, doctest, ngtcp2 1.26 (QUIC, built against the OpenSSL above), and nghttp3 1.19 (QPACK).

## Build

```powershell
cmake -S . -B out/build/x64-Debug
cmake --build out/build/x64-Debug --config Debug
ctest --test-dir out/build/x64-Debug -C Debug --output-on-failure
```

The build copies `migrations/sqlite` and the OpenSSL DLLs (`libssl-4-x64.dll`, `libcrypto-4-x64.dll`) next to `matchmaker.exe` and `matchmaker_tests.exe`.

## Run

```powershell
matchmaker --database C:\data\matchmaker.db
```

`--database` overrides `MATCHMAKER_DATABASE`. `--help` prints usage.

The first launch creates the file, applies `migrations/sqlite`, and inserts the default `config` and `regions` rows. The process then exits until `tls_cert`, `tls_key`, and `jwt_secret` are non-empty. Set them and start again:

```sql
UPDATE config SET value = 'C:\certs\server.pem' WHERE key = 'tls_cert';
UPDATE config SET value = 'C:\certs\server.key' WHERE key = 'tls_key';
UPDATE config SET value = 'replace-with-a-long-random-secret' WHERE key = 'jwt_secret';
UPDATE config SET value = '0.0.0.0:443' WHERE key = 'listen';
UPDATE config SET value = 'match.example.com' WHERE key = 'public_host';
UPDATE config SET value = 'https://match.example.com' WHERE key = 'public_url';
```

`tls_cert` and `tls_key` are PEM paths. `public_host` is the address clients receive in relay allocations. `listen` and `stats_listen` are `host:port` (IPv6 as `[addr]:port`).

Migrations are read from `MATCHMAKER_MIGRATIONS`, then from `migrations/sqlite` beside the executable, then from the source tree.

`SIGINT` and `SIGTERM` close lobby sockets and exit. `SIGHUP` and `SIGBREAK` reload config, regions, and games from the database. The listen address, stats address, WebTransport listener, TLS files, and worker count stay as they were at startup. Relay channels already allocated keep the lifetime they were granted.

`stats_listen` (default `127.0.0.1:7711`) is plaintext. One connection receives one JSON document and the server closes. See [PROTOCOL.md](PROTOCOL.md).

## WebTransport

The WebTransport path is off by default. To turn it on:

```sql
UPDATE config SET value = '1' WHERE key = 'relay_wt_enabled';
UPDATE config SET value = '0.0.0.0:443' WHERE key = 'wt_listen';
UPDATE config SET value = 'https://game.example' WHERE key = 'cors_origins';
```

Then set `"allow_wt": true` in the `limits_json` of each game that may allocate `wt` channels, and restart. `wt_listen` is a UDP bind, so it can share port 443 with the TCP `listen` address. It is bound only at startup, and startup fails if the bind fails. It uses the `tls_cert` and `tls_key` files.

A browser client calls `POST /api/v1/wt-ticket` after create or join, then opens `new WebTransport(url)` with the returned `url`. Lobby ops are length-prefixed JSON on a stream the server opens, and game bytes are datagrams prefixed with the 16-byte channel id. [PROTOCOL.md](PROTOCOL.md) has the details.

Browsers require a certificate they trust for WebTransport. For development with a self-signed certificate, Chromium accepts `serverCertificateHashes` when the certificate uses ECDSA and is valid for at most 14 days. Pass its SHA-256 digest to the client:

```powershell
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -days 13 -nodes `
  -keyout dev.key -out dev.pem -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
openssl x509 -in dev.pem -outform der | openssl dgst -sha256
```

The page itself must be served from an origin listed in `cors_origins`, because it calls the HTTPS API with `fetch`. `serverCertificateHashes` covers only the WebTransport connection; `fetch` still needs a certificate the browser trusts.

The ticket `url` uses the host from `public_url`. Browsers resolve `localhost` to `::1` first and do not fall back to IPv4 for WebTransport, so with `wt_listen` on `127.0.0.1` the handshake fails. Use `127.0.0.1` in `public_url` (the client's `webTransportOrigin` option also overrides it), or bind `wt_listen` to the address the hostname resolves to. The same applies to a public hostname with an AAAA record.

### Browser client and demo

[`web/src/matchmaker.js`](web/src/matchmaker.js) is a dependency-free ES module. It uses `fetch` for auth, list, create, and join, and `WebTransport` for the lobby session:

```js
import { MatchmakerClient } from './matchmaker.js';

const mm = new MatchmakerClient({ baseUrl: 'https://match.example.com' });
await mm.auth(gameId, gameToken, { alias: 'ada' });
await mm.createLobby();
const lobby = await mm.connect();
lobby.addEventListener('event', (e) => console.log(e.detail));
lobby.addEventListener('datagram', (e) => console.log(e.detail.channelId, e.detail.data));
await lobby.chat('hello');
```

The host calls `lobby.allocateWt(peerUserId)`, and both sides then call `lobby.sendDatagram(channelId, bytes)`. The module answers server pings itself. A host should call `startHeartbeat(ms)` below `host_heartbeat_interval_sec`.

[`web/demo/index.html`](web/demo/index.html) is a two-tab demo covering auth, create, join, chat, `wt` allocation, and a datagram exchange. Serve the `web/` folder from a listed origin, for example:

```powershell
cd web
python -m http.server 8080
# cors_origins = 'http://localhost:8080', then open http://localhost:8080/demo/ in two tabs
```

## Games

Each game has a UUID, a display name, and a SHA-256 hex digest of its shared token. Clients send the raw token to `POST /api/v1/auth`; the server hashes it and compares in constant time. `external_id` on that call is a label, not an account.

Hash the token as lowercase hex SHA-256 of its UTF-8 bytes (64 characters) and store that digest, not the token:

```powershell
$sha = [System.Security.Cryptography.SHA256]::Create()
-join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes('game-token')) | ForEach-Object { $_.ToString('x2') })
```

```sql
INSERT INTO games(id, name, token_hash, promote_host_on_leave, allow_join_in_progress, limits_json, created_at_ms)
VALUES (
  'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
  'Example',
  'paste-the-64-char-sha256-hex-here',
  0,
  0,
  '{"max_players_per_lobby":4,"max_lobbies":100}',
  CAST(strftime('%s','now') AS INTEGER) * 1000
);
```

Games do not have modes. A game advertises modes, maps, or rules by writing lobby metadata; list, get, and the lobby snapshot all include that object.

`limits_json` keys, each optional: `max_players_per_lobby`, `max_password_length`, `max_concurrent_channels`, `max_channel_lifetime_sec`, `max_idle_sec`, `allow_tcp`, `allow_wt`, `max_lobbies`, `max_metadata_bytes`, `max_chat_bytes`, `max_custom_bytes`, `max_alias_bytes`. A lobby may hold at most as many relay channels as it has player slots, and every channel connects the host to one other member. Game limits are clamped to the system caps. TCP relays also require `relay_tcp_enabled`. `wt` relays require `relay_wt_enabled`; when it is off, `allow_wt` is forced off.

A reload picks up inserted and edited games. `promote_host_on_leave` hands the lobby to the earliest remaining member when the host leaves; otherwise the lobby ends. `allow_join_in_progress` lets the host offer late join after the match starts.

## Configuration

Rows live in `config`. Booleans accept `1`/`true`/`yes` and `0`/`false`/`no`.

| Key | Default | Role |
| --- | --- | --- |
| `listen` | `0.0.0.0:443` | HTTPS and WebSocket bind (fixed at startup) |
| `public_host` | `localhost` | Host advertised on relay allocations |
| `public_url` | `https://localhost` | URL published in `/api/v1/config` |
| `region_id` | `local` | This process's region |
| `jwt_secret` | `change-me` | HS256 key; replace before any real use |
| `jwt_ttl_sec` | `900` | Access-token lifetime |
| `admit_sec` | `60` | How long a new lobby waits for the host socket |
| `join_hold_sec` | `10` | How long an HTTPS join holds a slot |
| `host_heartbeat_interval_sec` | `2` | Expected host heartbeat spacing |
| `host_heartbeat_misses` | `3` | Misses before the host is treated as gone |
| `max_threads` | `1` | Worker threads. Fixed at startup. Values below 1 run as 1 |
| `stats_listen` | `127.0.0.1:7711` | Localhost stats bind (fixed at startup) |
| `tls_cert` | empty | PEM certificate chain (required) |
| `tls_key` | empty | PEM private key (required) |
| `tls_min_version` | `1.2` | `1.2` or `1.3` |
| `http_gzip` | `1` | Gzip JSON responses |
| `http_gzip_min_bytes` | `256` | Smallest body that is compressed |
| `websocket_permessage_deflate` | `1` | Accept permessage-deflate |
| `websocket_deflate_level` | `3` | Deflate level |
| `websocket_max_window_bits` | `12` | Server window bits |
| `retention_days` | `30` | Operational log retention |
| `max_message_bytes` | `65536` | Advertised message cap |
| `max_connections` | `10000` | Connection cap |
| `max_outbound_queue_bytes` | `1048576` | Per-socket outbound queue; overflow disconnects |
| `max_concurrent_channels` | `4096` | System relay-channel cap |
| `max_channel_lifetime_sec` | `7200` | System relay lifetime cap |
| `max_idle_sec` | `120` | System relay idle cap |
| `max_udp_packet_bytes` | `1200` | UDP datagrams above this are dropped |
| `max_bytes_per_sec_per_channel` | `0` | Per-channel byte rate; `0` disables the cap |
| `max_players_per_lobby` | `64` | System player cap |
| `max_password_length` | `64` | System lobby-password cap |
| `max_frame_bytes` | `65536` | WebSocket frame cap |
| `relay_bind` | `0.0.0.0` | Address relay ports bind |
| `relay_port_start` | `30000` | First relay port |
| `relay_port_end` | `39999` | Last relay port |
| `relay_tcp_enabled` | `0` | Allow TCP relays when the game also sets `allow_tcp` |
| `relay_wt_enabled` | `0` | Start the WebTransport listener (startup only) and allow `wt` relays when the game sets `allow_wt` |
| `wt_listen` | `0.0.0.0:443` | UDP bind for HTTP/3 WebTransport, used only when `relay_wt_enabled` (fixed at startup) |
| `cors_origins` | empty | Comma- or space-separated browser origins allowed on HTTPS, WebSocket, and WebTransport; `*` allows any; empty disables CORS |
| `wt_ticket_sec` | `30` | Lifetime of a one-time WebTransport ticket |
| `auth_fail_limit` | `20` | Failed auths per source before rate limiting |
| `auth_fail_window_sec` | `60` | Auth failure window |
| `probe_timeout_ms` | `500` | Host RTT probe timeout |

`regions` rows are `id`, `public_url`, and `sort_order`. The default row is `local` / `https://localhost`.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | Server: HTTPS, lobby WebSocket, WebTransport (`wt.cpp`, `h3.cpp`), UDP/TCP relay, SQLite, JWT |
| `include/matchmaker/client.hpp` | In-tree HTTPS and WebSocket client linked by the tests |
| `web/` | Browser client (`src/matchmaker.js`) and two-tab demo (`demo/index.html`) |
| `tests/` | doctest suites (`matchmaker_tests`), including a small ngtcp2 WebTransport peer (`wt_peer.cpp`) |
| `migrations/sqlite/` | Schema and default config |
| `PROTOCOL.md` | HTTP, WebSocket, WebTransport, and relay specification |

## Logs

The `logs` table records startup, shutdown, config reload, auth failure (game id only), lobby create and close, relay allocate and release, and errors. Chat, ready state, custom messages, aliases, and forwarded packets are not logged. Rows older than `retention_days` are pruned at startup and about once an hour.

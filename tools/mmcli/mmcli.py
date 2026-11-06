#!/usr/bin/env python3
"""REPL client for the matchmaker service.

Each process is one player. Run a second copy to join a lobby created by the first.

    python mmcli.py --url https://127.0.0.1:443 --insecure --game <uuid> --token <game token> --alias host

The game token is the raw secret stored as a SHA-256 hash on the server, not the hash.
Type help inside the prompt for lobby, chat, and relay commands.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import os
import re
import shlex
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.parse

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)
LOBBY_FIELDS = (
    "lobby_id",
    "host_id",
    "phase",
    "slots",
    "member_count",
    "locked",
    "allow_late_join",
    "short_code",
    "metadata",
)


class ConsoleError(Exception):
    """A command cannot run because of how it was used or the current session."""


class HttpApi:
    def __init__(self, host: str, port: int, context: ssl.SSLContext, timeout: float):
        self.host = host
        self.port = port
        self.context = context
        self.timeout = timeout

    def request(self, method: str, path: str, body: object | None = None, bearer: str | None = None):
        conn = http.client.HTTPSConnection(
            self.host, self.port, context=self.context, timeout=self.timeout
        )
        headers = {"Accept": "application/json"}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if bearer:
            headers["Authorization"] = f"Bearer {bearer}"
        try:
            conn.request(method, path, body=data, headers=headers)
            resp = conn.getresponse()
            raw = resp.read()
            status = resp.status
        finally:
            conn.close()
        text = raw.decode("utf-8", "replace")
        parsed = None
        if text:
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = None
        return status, parsed, text


def encode_frame(opcode: int, payload: bytes) -> bytes:
    mask = os.urandom(4)
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack("!H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack("!Q", length)
    header += mask
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return bytes(header) + masked


class FrameReader:
    """Incremental WebSocket frame parser. Client frames are masked; server frames are not."""

    def __init__(self, initial: bytes = b""):
        self.buf = bytearray(initial)

    def feed(self, data: bytes) -> None:
        self.buf += data

    def pop(self):
        if len(self.buf) < 2:
            return None
        first, second = self.buf[0], self.buf[1]
        if first & 0x70:
            raise ConnectionError("unexpected RSV bits; permessage-deflate was not offered")
        masked = bool(second & 0x80)
        length = second & 0x7F
        offset = 2
        if length == 126:
            if len(self.buf) < offset + 2:
                return None
            length = struct.unpack("!H", self.buf[offset : offset + 2])[0]
            offset += 2
        elif length == 127:
            if len(self.buf) < offset + 8:
                return None
            length = struct.unpack("!Q", self.buf[offset : offset + 8])[0]
            offset += 8
        if length > 8 * 1024 * 1024:
            raise ConnectionError("frame too large")
        mask = b""
        if masked:
            if len(self.buf) < offset + 4:
                return None
            mask = bytes(self.buf[offset : offset + 4])
            offset += 4
        if len(self.buf) < offset + length:
            return None
        payload = bytes(self.buf[offset : offset + length])
        del self.buf[: offset + length]
        if mask:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        return bool(first & 0x80), first & 0x0F, payload


class LobbySocket:
    def __init__(self, on_text, on_close):
        self._on_text = on_text
        self._on_close = on_close
        self._sock: socket.socket | None = None
        self._rest = b""
        self._send_lock = threading.Lock()
        self._closed = False
        self._reader: threading.Thread | None = None

    @property
    def alive(self) -> bool:
        return self._sock is not None and not self._closed

    def connect(self, host: str, port: int, path: str, bearer: str, context: ssl.SSLContext) -> None:
        raw = socket.create_connection((host, port), timeout=10)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        server_hostname = None if _is_ip(host) else host
        sock = context.wrap_socket(raw, server_hostname=server_hostname)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Authorization: Bearer {bearer}\r\n"
            "\r\n"
        )
        sock.sendall(request.encode("ascii"))
        data = b""
        sock.settimeout(10)
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                sock.close()
                raise ConnectionError("lobby socket closed during the handshake")
            data += chunk
            if len(data) > 65536:
                sock.close()
                raise ConnectionError("handshake response is too large")
        header, rest = data.split(b"\r\n\r\n", 1)
        lines = header.decode("iso-8859-1").split("\r\n")
        parts = lines[0].split()
        if len(parts) < 2 or parts[1] != "101":
            sock.settimeout(1)
            try:
                while len(data) < 8192:
                    extra = sock.recv(4096)
                    if not extra:
                        break
                    data += extra
            except (socket.timeout, OSError):
                pass
            sock.close()
            body = data.split(b"\r\n\r\n", 1)[-1].decode("utf-8", "replace")
            raise ConnectionError(f"upgrade failed: {lines[0]} {body}".strip())
        accept = ""
        for line in lines[1:]:
            name, _, value = line.partition(":")
            if name.lower() == "sec-websocket-accept":
                accept = value.strip()
        expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        if accept != expected:
            sock.close()
            raise ConnectionError("server sent a bad Sec-WebSocket-Accept")
        sock.settimeout(None)
        self._sock = sock
        self._rest = rest

    def start(self) -> None:
        if self._reader is not None or self._sock is None:
            return
        self._reader = threading.Thread(
            target=self._read_loop, args=(self._rest,), name="lobby-socket", daemon=True
        )
        self._reader.start()

    def send_json(self, message: dict) -> None:
        self._send(0x1, json.dumps(message, ensure_ascii=False).encode("utf-8"))

    def close(self) -> None:
        sock = self._sock
        if sock is None or self._closed:
            return
        try:
            self._send(0x8, b"")
        except OSError:
            pass
        self._shutdown(sock)

    def _send(self, opcode: int, payload: bytes) -> None:
        sock = self._sock
        if sock is None or self._closed:
            raise ConnectionError("lobby socket is closed")
        frame = encode_frame(opcode, payload)
        with self._send_lock:
            sock.sendall(frame)

    def _shutdown(self, sock: socket.socket) -> None:
        self._closed = True
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

    def _read_loop(self, initial: bytes) -> None:
        reason = "closed"
        fragments: list[bytes] = []
        frag_opcode = None
        parser = FrameReader(initial)
        sock = self._sock
        assert sock is not None
        try:
            while not self._closed:
                frame = parser.pop()
                if frame is None:
                    chunk = sock.recv(4096)
                    if not chunk:
                        reason = "server closed the connection"
                        break
                    parser.feed(chunk)
                    continue
                fin, opcode, payload = frame
                if opcode == 0x9:
                    self._send(0xA, payload)
                    continue
                if opcode == 0xA:
                    continue
                if opcode == 0x8:
                    reason = "server closed the lobby socket"
                    if not self._closed:
                        try:
                            self._send(0x8, payload[:2])
                        except OSError:
                            pass
                    break
                if opcode == 0x0:
                    if frag_opcode is None:
                        raise ConnectionError("continuation frame without a start")
                    fragments.append(payload)
                    if fin:
                        self._deliver(frag_opcode, b"".join(fragments))
                        fragments = []
                        frag_opcode = None
                    continue
                if opcode in (0x1, 0x2):
                    if not fin:
                        frag_opcode = opcode
                        fragments = [payload]
                        continue
                    self._deliver(opcode, payload)
                    continue
                raise ConnectionError(f"unsupported opcode {opcode}")
        except Exception as exc:
            if not self._closed:
                reason = str(exc)
        finally:
            if sock is not None:
                self._shutdown(sock)
            self._on_close(reason)

    def _deliver(self, opcode: int, payload: bytes) -> None:
        if opcode != 0x1:
            self._on_text({"op": "binary", "hex": payload.hex()})
            return
        try:
            message = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            message = {"op": "invalid", "error": str(exc), "hex": payload.hex()}
        self._on_text(message)


class RelayChannel:
    def __init__(self, info: dict, on_payload):
        self.info = info
        self.on_payload = on_payload
        self.closed = False
        self.pinned = threading.Event()
        self.sock: socket.socket | None = None
        self._send_lock = threading.Lock()
        threading.Thread(target=self._run, name=f"relay-{info['channel_id'][:8]}", daemon=True).start()

    def send(self, payload: bytes) -> None:
        if not self.pinned.wait(5):
            raise ConsoleError("relay did not pin within 5 seconds")
        sock = self.sock
        if sock is None or self.closed:
            raise ConsoleError("relay is closed")
        with self._send_lock:
            if self.info["protocol"] == "tcp":
                sock.sendall(payload)
            else:
                sock.sendto(payload, self._peer)

    def close(self) -> None:
        self.closed = True
        sock = self.sock
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    def _run(self) -> None:
        try:
            token = bytes.fromhex(self.info["token"])
            if len(token) != 16:
                raise ConsoleError(f"relay token is {len(token)} bytes, expected 16")
            if self.info["protocol"] == "tcp":
                self._run_tcp(token)
            else:
                self._run_udp(token)
        except Exception as exc:
            if not self.closed:
                self.on_payload(self.info, None, f"relay failed: {exc}")

    def _run_udp(self, token: bytes) -> None:
        infos = socket.getaddrinfo(self.info["host"], self.info["port"], type=socket.SOCK_DGRAM)
        family, socktype, proto, _, peer = infos[0]
        sock = socket.socket(family, socktype, proto)
        sock.settimeout(0.5)
        self.sock = sock
        self._peer = peer
        sock.sendto(token, peer)
        time.sleep(0.02)
        self.pinned.set()
        while not self.closed:
            try:
                data, _addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            self.on_payload(self.info, data, None)

    def _run_tcp(self, token: bytes) -> None:
        infos = socket.getaddrinfo(self.info["host"], self.info["port"], type=socket.SOCK_STREAM)
        family, socktype, proto, _, peer = infos[0]
        sock = socket.socket(family, socktype, proto)
        sock.settimeout(5)
        sock.connect(peer)
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = sock
        sock.sendall(token)
        self.pinned.set()
        while not self.closed:
            try:
                data = sock.recv(65535)
            except OSError:
                break
            if not data:
                break
            self.on_payload(self.info, data, None)


def _is_ip(host: str) -> bool:
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            socket.inet_pton(family, host)
            return True
        except OSError:
            continue
    return False


def format_payload(data: bytes) -> str:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return data.hex()
    if text and all(ch.isprintable() or ch in "\t" for ch in text):
        return text
    return data.hex()


def parse_bool(text: str) -> bool:
    value = text.lower()
    if value in ("1", "on", "true", "yes"):
        return True
    if value in ("0", "off", "false", "no"):
        return False
    raise ConsoleError("expected on/off")


def parse_host_port(text: str, default_port: int) -> tuple[str, int]:
    if text.startswith("["):
        host, _, rest = text[1:].partition("]")
        if rest.startswith(":"):
            return host, int(rest[1:])
        return host, default_port
    if text.count(":") == 1:
        host, port = text.rsplit(":", 1)
        return host, int(port)
    return text, default_port


def pretty(value: object) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False)


class Console:
    def __init__(self, http: HttpApi, ssl_context: ssl.SSLContext, args: argparse.Namespace):
        self.http = http
        self.ssl_context = ssl_context
        self.args = args
        self.token: str | None = None
        self.user_id: str | None = None
        self.game_id: str | None = args.game
        self.alias = args.alias or ""
        self.external_id = args.external_id or ""
        self.lobby: dict = {}
        self.members: dict[str, dict] = {}
        self.lobby_id: str | None = None
        self.reservation: str | None = None
        self.ws: LobbySocket | None = None
        self.relays: dict[str, RelayChannel] = {}
        self.relay_log: list[str] = []
        self.pending: dict[str, tuple] = {}
        self._seq = 0
        self._ws_gen = 0
        self._stop = threading.Event()
        self._print_lock = threading.Lock()
        self._state_lock = threading.RLock()

    def out(self, text: str) -> None:
        with self._print_lock:
            print(text, flush=True)

    def run(self) -> None:
        self.out("matchmaker console — one process is one player")
        self.out(f"server https://{self.http.host}:{self.http.port}")
        if self.args.game and self.args.token:
            try:
                self._auth(self.args.game, self.args.token, self.alias, self.external_id)
            except (ConsoleError, OSError, ssl.SSLError, http.client.HTTPException) as exc:
                self.out(f"! auth failed: {exc}")
        self.out("type help")
        while not self._stop.is_set():
            try:
                line = input(self._prompt())
            except EOFError:
                self.out("")
                break
            except KeyboardInterrupt:
                self.out("")
                break
            try:
                if self.dispatch(line):
                    break
            except KeyboardInterrupt:
                self.out("! interrupted")
            except ConsoleError as exc:
                self.out(f"! {exc}")
            except json.JSONDecodeError as exc:
                self.out(f"! invalid json: {exc}")
            except Exception as exc:
                self.out(f"! {exc}")
        self.shutdown()

    def shutdown(self) -> None:
        self._stop.set()
        self._ws_gen += 1
        ws = self.ws
        if ws is not None:
            ws.close()
        self._close_relays()

    def dispatch(self, line: str) -> bool:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            return False
        cmd, _, rest = stripped.partition(" ")
        cmd = cmd.lower()
        handler = {
            "help": self.cmd_help,
            "?": self.cmd_help,
            "health": self.cmd_health,
            "config": self.cmd_config,
            "auth": self.cmd_auth,
            "who": self.cmd_who,
            "stats": self.cmd_stats,
            "lobbies": self.cmd_lobbies,
            "ls": self.cmd_lobbies,
            "get": self.cmd_get,
            "rtt": self.cmd_rtt,
            "code": self.cmd_code,
            "create": self.cmd_create,
            "join": self.cmd_join,
            "connect": self.cmd_connect,
            "phase": self.cmd_phase,
            "slots": self.cmd_slots,
            "late": self.cmd_late,
            "password": self.cmd_password,
            "shortcode": self.cmd_shortcode,
            "meta": self.cmd_meta,
            "alias": self.cmd_alias,
            "ready": self.cmd_ready,
            "say": self.cmd_say,
            "chat": self.cmd_say,
            "custom": self.cmd_custom,
            "beat": self.cmd_beat,
            "heartbeat": self.cmd_beat,
            "alloc": self.cmd_alloc,
            "allocall": self.cmd_allocall,
            "release": self.cmd_release,
            "close": self.cmd_close_lobby,
            "leave": self.cmd_leave,
            "disconnect": self.cmd_disconnect,
            "raw": self.cmd_raw,
            "members": self.cmd_members,
            "relays": self.cmd_relays,
            "pin": self.cmd_pin,
            "send": self.cmd_send,
            "hex": self.cmd_hex,
            "relaylog": self.cmd_relaylog,
            "http": self.cmd_http,
            "quit": lambda _rest: True,
            "exit": lambda _rest: True,
        }.get(cmd)
        if handler is None:
            raise ConsoleError(f"unknown command {cmd!r}; type help")
        return bool(handler(rest))

    def cmd_help(self, _rest: str) -> None:
        self.out(
            """
Session
  health                         GET /api/v1/health
  config                         GET /api/v1/config
  auth [game token alias]        authenticate; flags are reused when omitted
  who                            session, lobby, and role
  stats [host:port]              read the plaintext stats document
  http <METHOD> <path> [json]    any HTTPS call, with the bearer if you have one
  quit                           disconnect and exit

Lobbies
  lobbies [limit]                list lobbies this game can join
  get [id] [client_ts]           look up a lobby without joining
  rtt [id]                       get, sending client_ts so the server can echo it
  code <short>                   look up a live lobby by short code
  create                         reserve a lobby and open the host socket
  join <id|code> [password]      hold a slot and open the member socket
  connect                        open the socket for a reservation that is still valid

Host, once the socket is open
  phase <closed|open|in_progress>
  slots <n>
  late <on|off>
  password [text]                omit the text to clear the lock
  shortcode [code]               omit the code to clear it
  meta <json>                    patch metadata; a null value deletes that key
  alloc <member> [udp|tcp] [seconds]
  allocall [udp|tcp] [seconds]   one channel from you to each other member
  release [channel ...]          omit ids to release every channel in the lobby
  beat                           one heartbeat (host heartbeats are also automatic)
  close                          end the lobby

Any member
  alias <name>
  ready <on|off>
  say <text>
  custom <name> <json> [member]
  leave                          leave and close the socket
  disconnect                     drop the socket without sending leave
  raw <json>                     send one websocket object as-is
  members
  relays                         allocations delivered to this process
  pin [channel]                  send the 16-byte token again (pins or rebinds)
  send [channel] <text>          UDP datagram or TCP bytes on a relay
  hex [channel] <hex>            send raw relay bytes
  relaylog                       recent relay payloads

A second process, authenticated for the same game, is the other player.
Host heartbeats go out on their own while this process is host. Join holds expire quickly; connect runs as part of create and join.
""".strip()
        )

    def cmd_health(self, _rest: str) -> None:
        self._show_http(*self.http.request("GET", "/api/v1/health"))

    def cmd_config(self, _rest: str) -> None:
        self._show_http(*self.http.request("GET", "/api/v1/config"))

    def cmd_auth(self, rest: str) -> None:
        parts = shlex.split(rest)
        game = parts[0] if len(parts) >= 1 else self.game_id
        token = parts[1] if len(parts) >= 2 else self.args.token
        alias = parts[2] if len(parts) >= 3 else self.alias
        external = parts[3] if len(parts) >= 4 else self.external_id
        if not game or not token:
            raise ConsoleError("auth needs a game id and token (arguments or --game/--token)")
        self._auth(game, token, alias or "", external or "")

    def cmd_who(self, _rest: str) -> None:
        if not self.user_id:
            self.out("not authenticated")
            return
        self.out(f"user {self.user_id}")
        self.out(f"alias {self.alias or '-'}")
        self.out(f"external_id {self.external_id or '-'}")
        self.out(f"game {self.game_id}")
        if not self.lobby_id:
            self.out("lobby none")
            return
        role = "host" if self.is_host else "member"
        socket_state = "open" if self.ws and self.ws.alive else "closed"
        phase = self.lobby.get("phase", "?")
        slots = self.lobby.get("slots", "?")
        self.out(f"lobby {self.lobby_id}  role {role}  phase {phase}  slots {slots}  socket {socket_state}")
        self.out(f"members {len(self.members)}  relays {len(self.relays)}")

    def cmd_stats(self, rest: str) -> None:
        host, port = parse_host_port((rest or self.args.stats).strip(), 7711)
        with socket.create_connection((host, port), timeout=5) as sock:
            chunks = []
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                chunks.append(data)
        body = b"".join(chunks).decode("utf-8")
        try:
            self.out(pretty(json.loads(body)))
        except json.JSONDecodeError:
            self.out(body)

    def cmd_lobbies(self, rest: str) -> None:
        self._need_auth()
        path = "/api/v1/lobbies"
        text = rest.strip()
        if text:
            try:
                limit = int(text)
            except ValueError as exc:
                raise ConsoleError("limit must be an integer") from exc
            path += f"?limit={limit}"
        self._show_http(*self.http.request("GET", path, bearer=self.token))

    def cmd_get(self, rest: str) -> None:
        self._need_auth()
        lobby_id, client_ts = self._lobby_query(rest)
        path = f"/api/v1/lobbies/{urllib.parse.quote(lobby_id)}"
        if client_ts is not None:
            path += f"?client_ts={client_ts}"
        self._show_http(*self.http.request("GET", path, bearer=self.token))

    def cmd_rtt(self, rest: str) -> None:
        extra = rest.strip()
        if not extra and not self.lobby_id:
            raise ConsoleError("usage: rtt [lobby-id]")
        stamp = str(int(time.time() * 1000))
        self.cmd_get(f"{extra} {stamp}".strip() if extra else stamp)

    def cmd_code(self, rest: str) -> None:
        self._need_auth()
        code = rest.strip()
        if not code:
            raise ConsoleError("usage: code <short>")
        path = "/api/v1/lobbies/by-code/" + urllib.parse.quote(code)
        self._show_http(*self.http.request("GET", path, bearer=self.token))

    def cmd_create(self, _rest: str) -> None:
        self._need_auth()
        status, body, text = self.http.request("POST", "/api/v1/lobbies", {}, self.token)
        self._show_http(status, body, text)
        if status != 200 or not isinstance(body, dict) or "lobby_id" not in body:
            return
        self._remember_lobby(body, "host")
        self.out(f"created {body['lobby_id']}")
        self.out("admission is short; opening the host socket")
        self._connect()
        self.out("lobby is closed until you run: phase open")

    def cmd_join(self, rest: str) -> None:
        self._need_auth()
        target, _, password = rest.strip().partition(" ")
        if not target:
            raise ConsoleError("usage: join <lobby-id|short-code> [password]")
        lobby_id = target
        if not UUID_RE.match(target):
            status, body, text = self.http.request(
                "GET", "/api/v1/lobbies/by-code/" + urllib.parse.quote(target), bearer=self.token
            )
            self._show_http(status, body, text)
            if status != 200 or not isinstance(body, dict) or "lobby_id" not in body:
                return
            lobby_id = body["lobby_id"]
        payload = {}
        if password:
            payload["password"] = password
        status, body, text = self.http.request(
            "POST", f"/api/v1/lobbies/{urllib.parse.quote(lobby_id)}/join", payload, self.token
        )
        self._show_http(status, body, text)
        if status != 200:
            return
        self.lobby_id = lobby_id
        self.reservation = "join"
        self.out("join hold is about 10 seconds; opening the lobby socket")
        self._connect()

    def cmd_connect(self, _rest: str) -> None:
        self._need_auth()
        if not self.lobby_id:
            raise ConsoleError("no lobby reservation; use create or join")
        self._connect()

    def cmd_phase(self, rest: str) -> None:
        phase = rest.strip()
        if phase not in ("closed", "open", "in_progress"):
            raise ConsoleError("usage: phase <closed|open|in_progress>")
        self._send_op("set_phase", {"phase": phase}, ("phase", phase))

    def cmd_slots(self, rest: str) -> None:
        try:
            slots = int(rest.strip())
        except ValueError as exc:
            raise ConsoleError("usage: slots <n>") from exc
        self._send_op("set_slots", {"slots": slots}, ("slots", slots))

    def cmd_late(self, rest: str) -> None:
        allowed = parse_bool(rest.strip())
        self._send_op("set_late_join", {"allowed": allowed}, ("late", allowed))

    def cmd_password(self, rest: str) -> None:
        password = rest if rest.strip() else None
        self._send_op("set_password", {"password": password}, ("password", password))

    def cmd_shortcode(self, rest: str) -> None:
        code = rest.strip() or None
        self._send_op("set_short_code", {"code": code}, ("shortcode", code))

    def cmd_meta(self, rest: str) -> None:
        if not rest.strip():
            raise ConsoleError('usage: meta {"map":"arena","mode":null}')
        patch = json.loads(rest)
        if not isinstance(patch, dict):
            raise ConsoleError("metadata must be a JSON object")
        self._send_op("set_metadata", {"metadata": patch}, ("meta", patch))

    def cmd_alias(self, rest: str) -> None:
        alias = rest.strip()
        self._send_op("set_alias", {"alias": alias}, ("alias", alias))

    def cmd_ready(self, rest: str) -> None:
        ready = parse_bool(rest.strip())
        self._send_op("set_ready", {"ready": ready}, ("ready", ready))

    def cmd_say(self, rest: str) -> None:
        if not rest.strip():
            raise ConsoleError("usage: say <text>")
        self._send_op("chat", {"text": rest.strip()}, None)

    def cmd_custom(self, rest: str) -> None:
        try:
            parts = shlex.split(rest)
        except ValueError as exc:
            raise ConsoleError(str(exc)) from exc
        if len(parts) < 2:
            raise ConsoleError("usage: custom <name> <json> [member]")
        name, raw = parts[0], parts[1]
        data = json.loads(raw)
        fields: dict = {"name": name, "data": data}
        effect = None
        if len(parts) >= 3:
            target = self._resolve_member(parts[2])
            fields["target_user_id"] = target
        self._send_op("custom", fields, effect)

    def cmd_beat(self, _rest: str) -> None:
        self._send_op("heartbeat", {}, None)

    def cmd_alloc(self, rest: str) -> None:
        parts = rest.split()
        if not parts:
            raise ConsoleError("usage: alloc <member> [udp|tcp] [seconds]")
        peer = self._resolve_member(parts[0])
        protocol, lifetime = self._relay_opts(parts[1:])
        self._allocate([(self.user_id, peer, protocol, lifetime)])

    def cmd_allocall(self, rest: str) -> None:
        protocol, lifetime = self._relay_opts(rest.split())
        with self._state_lock:
            peers = [uid for uid in self.members if uid != self.user_id]
        if not peers:
            raise ConsoleError("no other members")
        self._allocate([(self.user_id, peer, protocol, lifetime) for peer in peers])

    def cmd_release(self, rest: str) -> None:
        ids = rest.split()
        resolved = [self._resolve_channel(item) for item in ids]
        fields: dict = {}
        if resolved:
            fields["ids"] = resolved
        self._send_op("release_relays", fields, None)

    def cmd_close_lobby(self, _rest: str) -> None:
        self._send_op("close_lobby", {}, None)

    def cmd_leave(self, _rest: str) -> None:
        self._send_op("leave", {}, None)

    def cmd_disconnect(self, _rest: str) -> None:
        if self.ws is None or not self.ws.alive:
            raise ConsoleError("lobby socket is not open")
        self.out("dropping the lobby socket")
        self.ws.close()

    def cmd_raw(self, rest: str) -> None:
        message = json.loads(rest)
        if not isinstance(message, dict):
            raise ConsoleError("raw payload must be a JSON object")
        self._need_ws()
        assert self.ws is not None
        self.ws.send_json(message)
        self.out(">> " + json.dumps(message, ensure_ascii=False))

    def cmd_members(self, _rest: str) -> None:
        with self._state_lock:
            members = [dict(member) for member in self.members.values()]
            host_id = self.lobby.get("host_id")
        if not members:
            self.out("no members")
            return
        for member in members:
            mark = " [host]" if member["user_id"] == host_id else ""
            you = " [you]" if member["user_id"] == self.user_id else ""
            ready = " [ready]" if member.get("ready") else ""
            connected = "" if member.get("connected", True) else " [down]"
            alias = member.get("alias") or "-"
            self.out(f"{alias}  {member['user_id']}{mark}{you}{ready}{connected}")

    def cmd_relays(self, _rest: str) -> None:
        with self._state_lock:
            relays = list(self.relays.items())
        if not relays:
            self.out("no relays")
            return
        for channel_id, channel in relays:
            info = channel.info
            self.out(
                f"{channel_id}  {info['protocol']}  {info['host']}:{info['port']}  "
                f"peer {info.get('peer_user_id', '-')}  token {info['token']}"
            )

    def cmd_pin(self, rest: str) -> None:
        channel = self._pick_channel(rest.strip())
        info = channel.info
        if info["protocol"] == "tcp":
            raise ConsoleError("tcp relays pin on connect; pin again is a UDP rebind")
        token = bytes.fromhex(info["token"])
        channel.send(token)
        self.out(f"sent token on {info['channel_id']}")

    def cmd_send(self, rest: str) -> None:
        channel, payload = self._split_channel_payload(rest, raw=True)
        if payload == "":
            raise ConsoleError("usage: send [channel] <text>")
        data = payload.encode("utf-8")
        channel.send(data)
        self.out(f"sent {len(data)} bytes on {channel.info['channel_id']}")

    def cmd_hex(self, rest: str) -> None:
        channel, payload = self._split_channel_payload(rest, raw=False)
        digits = "".join(payload.split())
        try:
            data = bytes.fromhex(digits)
        except ValueError as exc:
            raise ConsoleError("hex payload must be even-length hexadecimal") from exc
        channel.send(data)
        self.out(f"sent {len(data)} bytes on {channel.info['channel_id']}")

    def cmd_relaylog(self, _rest: str) -> None:
        if not self.relay_log:
            self.out("relay log is empty")
            return
        for line in self.relay_log:
            self.out(line)

    def cmd_http(self, rest: str) -> None:
        try:
            parts = shlex.split(rest)
        except ValueError as exc:
            raise ConsoleError(str(exc)) from exc
        if len(parts) < 2:
            raise ConsoleError("usage: http <METHOD> <path> [json]")
        method, path = parts[0].upper(), parts[1]
        body = json.loads(parts[2]) if len(parts) >= 3 else None
        self._show_http(*self.http.request(method, path, body, self.token))

    @property
    def is_host(self) -> bool:
        return bool(self.user_id and self.lobby.get("host_id") == self.user_id)

    def _auth(self, game: str, token: str, alias: str, external: str) -> None:
        body = {"game_id": game, "token": token}
        if alias:
            body["alias"] = alias
        if external:
            body["external_id"] = external
        status, parsed, text = self.http.request("POST", "/api/v1/auth", body)
        self._show_http(status, parsed, text)
        if status != 200 or not isinstance(parsed, dict) or "access_token" not in parsed:
            raise ConsoleError("authentication failed")
        self.token = parsed["access_token"]
        self.user_id = parsed["user_id"]
        self.game_id = parsed.get("game_id", game)
        self.alias = alias
        self.external_id = external
        self.args.token = token
        self.out(f"authenticated as {self.user_id}")

    def _connect(self) -> None:
        if self.ws is not None and self.ws.alive:
            raise ConsoleError("lobby socket is already open")
        if not self.lobby_id or not self.token:
            raise ConsoleError("no lobby reservation")
        ws = LobbySocket(self._on_ws, lambda reason: self._on_ws_close(ws, reason))
        path = "/ws/v1/lobbies/" + urllib.parse.quote(self.lobby_id)
        try:
            ws.connect(self.http.host, self.http.port, path, self.token, self.ssl_context)
        except Exception as exc:
            ws.close()
            hint = "join hold" if self.reservation == "join" else "host admission"
            raise ConnectionError(
                f"{exc}. The {hint} may still be valid; retry with connect"
            ) from exc
        self.ws = ws
        self._ws_gen += 1
        gen = self._ws_gen
        ws.start()
        if self.args.heartbeat > 0:
            threading.Thread(target=self._heartbeat_loop, args=(gen,), name="heartbeat", daemon=True).start()
        self.out("lobby socket open")

    def _heartbeat_loop(self, gen: int) -> None:
        while not self._stop.is_set():
            if gen != self._ws_gen:
                return
            if self.is_host:
                ws = self.ws
                if ws is None or not ws.alive:
                    return
                try:
                    ws.send_json({"op": "heartbeat"})
                except OSError:
                    return
            if self._stop.wait(self.args.heartbeat):
                return

    def _send_op(self, op: str, fields: dict, effect: tuple | None) -> None:
        self._need_ws()
        assert self.ws is not None
        self._seq += 1
        message = {"op": op, "id": str(self._seq), **fields}
        if effect is not None:
            self.pending[message["id"]] = effect
        self.ws.send_json(message)
        self.out(">> " + json.dumps(message, ensure_ascii=False))

    def _allocate(self, specs: list[tuple[str | None, str, str, int]]) -> None:
        if not self.user_id:
            raise ConsoleError("not authenticated")
        channels = []
        for left, right, protocol, lifetime in specs:
            if left == right:
                raise ConsoleError("a channel connects the host to one other member")
            channels.append({"a": left, "b": right, "protocol": protocol, "lifetime_sec": lifetime})
        self._send_op("allocate_relays", {"channels": channels}, None)

    def _on_ws(self, message: dict) -> None:
        if not isinstance(message, dict):
            self.out("<< " + repr(message))
            return
        op = message.get("op")
        if op == "heartbeat_ack" and "id" not in message and not self.args.verbose:
            return
        self._apply(message)
        label = op or "message"
        if op == "event":
            label = f"event {message.get('event', '')}".strip()
        elif op == "error":
            label = f"error {message.get('error', '')}".strip()
        self.out(f"\n<< {label}\n{pretty(message)}")

    def _apply(self, message: dict) -> None:
        with self._state_lock:
            note = self._apply_locked(message)
        if note:
            self.out(note)

    def _apply_locked(self, message: dict) -> None:
        op = message.get("op")
        if op == "lobby_snapshot":
            self._apply_snapshot(message)
            return
        if op == "ok":
            self._apply_ok(message)
            return
        if op == "error":
            self.pending.pop(str(message.get("id", "")), None)
            return
        if op != "event":
            return
        event = message.get("event")
        if event == "user_entered":
            uid = message.get("user_id")
            if uid:
                current = self.members.get(uid, {"user_id": uid, "external_id": "", "ready": False})
                current["alias"] = message.get("alias", current.get("alias", ""))
                current["connected"] = True
                self.members[uid] = current
        elif event == "user_left":
            self.members.pop(message.get("user_id", ""), None)
        elif event == "host_changed":
            self.lobby["host_id"] = message.get("host_id")
        elif event == "lobby_phase_changed":
            self.lobby["phase"] = message.get("phase")
        elif event == "slots_changed":
            self.lobby["slots"] = message.get("slots")
        elif event == "late_join_changed":
            self.lobby["allow_late_join"] = message.get("allow_late_join")
        elif event == "lock_changed":
            self.lobby["locked"] = message.get("locked")
        elif event == "short_code_changed":
            self.lobby["short_code"] = message.get("short_code")
        elif event == "lobby_metadata":
            self.lobby["metadata"] = message.get("metadata") or {}
        elif event == "alias_changed":
            member = self.members.get(message.get("user_id", ""))
            if member is not None:
                member["alias"] = message.get("alias", "")
        elif event == "ready_changed":
            member = self.members.get(message.get("user_id", ""))
            if member is not None:
                member["ready"] = bool(message.get("ready"))
        elif event == "relay_allocated":
            return self._open_relay(message)
        elif event in ("relay_released", "relay_expired"):
            self._drop_relay(message.get("channel_id", ""))
        elif event == "lobby_closed":
            self.lobby["phase"] = "closed"

    def _apply_snapshot(self, message: dict) -> None:
        self.lobby = {key: message.get(key) for key in LOBBY_FIELDS}
        self.lobby_id = message.get("lobby_id", self.lobby_id)
        self.members = {}
        for member in message.get("members") or []:
            if isinstance(member, dict) and member.get("user_id"):
                self.members[member["user_id"]] = member
        me = self.members.get(self.user_id or "")
        if me and me.get("alias"):
            self.alias = me["alias"]

    def _apply_ok(self, message: dict) -> None:
        effect = self.pending.pop(str(message.get("id", "")), None)
        if not effect:
            return
        kind = effect[0]
        if kind == "phase":
            self.lobby["phase"] = effect[1]
        elif kind == "slots":
            self.lobby["slots"] = effect[1]
        elif kind == "late":
            self.lobby["allow_late_join"] = effect[1]
        elif kind == "password":
            self.lobby["locked"] = bool(effect[1])
        elif kind == "shortcode":
            self.lobby["short_code"] = effect[1].upper() if effect[1] else None
        elif kind == "meta":
            metadata = dict(self.lobby.get("metadata") or {})
            for key, value in effect[1].items():
                if value is None:
                    metadata.pop(key, None)
                else:
                    metadata[key] = value
            self.lobby["metadata"] = metadata
        elif kind == "alias":
            self.alias = effect[1]
            me = self.members.get(self.user_id or "")
            if me is not None:
                me["alias"] = effect[1]
        elif kind == "ready":
            me = self.members.get(self.user_id or "")
            if me is not None:
                me["ready"] = effect[1]

    def _open_relay(self, message: dict) -> str | None:
        channel_id = message.get("channel_id")
        token = message.get("token")
        if not channel_id or not token:
            return
        with self._state_lock:
            previous = self.relays.pop(channel_id, None)
            if previous is not None:
                previous.close()
            channel = RelayChannel(dict(message), self._on_relay_payload)
            self.relays[channel_id] = channel
        return (
            f"relay {message.get('protocol', 'udp')} {channel_id} "
            f"via {message.get('host')}:{message.get('port')} peer {message.get('peer_user_id')}"
        )

    def _drop_relay(self, channel_id: str) -> None:
        with self._state_lock:
            channel = self.relays.pop(channel_id, None)
        if channel is not None:
            channel.close()

    def _on_relay_payload(self, info: dict, data: bytes | None, error: str | None) -> None:
        if error:
            line = f"relay {info.get('channel_id', '')}: {error}"
        else:
            assert data is not None
            line = f"relay {info.get('protocol')} {info.get('channel_id')} {len(data)} bytes: {format_payload(data)}"
        self.relay_log.append(line)
        del self.relay_log[:-50]
        self.out("\n" + line)

    def _on_ws_close(self, ws: LobbySocket, reason: str) -> None:
        with self._state_lock:
            if self.ws is not ws:
                return
            self._ws_gen += 1
            self.ws = None
            self.lobby = {}
            self.members = {}
            self.lobby_id = None
            self.reservation = None
            self.pending.clear()
        self._close_relays()
        if not self._stop.is_set():
            self.out(f"\nlobby socket closed ({reason})")

    def _close_relays(self) -> None:
        with self._state_lock:
            channels = list(self.relays.values())
            self.relays.clear()
        for channel in channels:
            channel.close()

    def _remember_lobby(self, summary: dict, reservation: str) -> None:
        self.lobby = {key: summary.get(key) for key in LOBBY_FIELDS}
        self.lobby_id = summary.get("lobby_id")
        self.reservation = reservation
        if self.user_id:
            self.members = {
                self.user_id: {
                    "user_id": self.user_id,
                    "alias": self.alias,
                    "external_id": self.external_id,
                    "ready": False,
                    "connected": False,
                }
            }

    def _need_auth(self) -> None:
        if not self.token:
            raise ConsoleError("not authenticated; use auth, or pass --game and --token")

    def _need_ws(self) -> None:
        self._need_auth()
        if self.ws is None or not self.ws.alive:
            raise ConsoleError("lobby socket is not open; use create, join, or connect")

    def _lobby_query(self, rest: str) -> tuple[str, str | None]:
        parts = rest.split()
        client_ts = None
        if not parts:
            if not self.lobby_id:
                raise ConsoleError("usage: get <lobby-id> [client_ts]")
            return self.lobby_id, None
        if len(parts) == 1:
            if UUID_RE.match(parts[0]) or not self.lobby_id:
                return parts[0], None
            return self.lobby_id, parts[0]
        return parts[0], parts[1]

    def _resolve_member(self, text: str) -> str:
        if not text:
            raise ConsoleError("missing member")
        exact = []
        partial = []
        with self._state_lock:
            members = list(self.members.values())
        for member in members:
            uid = member["user_id"]
            alias = member.get("alias") or ""
            if uid == text or (alias and alias == text):
                exact.append(member)
            elif uid.startswith(text) or (alias and alias.lower().startswith(text.lower())):
                partial.append(member)
        found = exact or partial
        if len(found) == 1:
            return found[0]["user_id"]
        if not found:
            raise ConsoleError(f"no member matches {text!r}; try members")
        names = ", ".join(f"{item.get('alias') or '-'} {item['user_id']}" for item in found)
        raise ConsoleError(f"ambiguous member {text!r}: {names}")

    def _resolve_channel(self, text: str) -> str:
        matches = [cid for cid in self.relays if cid == text or cid.startswith(text)]
        if len(matches) == 1:
            return matches[0]
        if not matches:
            return text
        raise ConsoleError(f"ambiguous channel {text!r}: {', '.join(matches)}")

    def _pick_channel(self, text: str) -> RelayChannel:
        if text:
            channel_id = self._resolve_channel(text)
            channel = self.relays.get(channel_id)
            if channel is None:
                raise ConsoleError(f"no relay {text!r}")
            return channel
        if len(self.relays) == 1:
            return next(iter(self.relays.values()))
        if not self.relays:
            raise ConsoleError("no relays yet")
        raise ConsoleError("more than one relay; pass a channel id")

    def _split_channel_payload(self, rest: str, raw: bool) -> tuple[RelayChannel, str]:
        text = rest if raw else rest.strip()
        if not text.strip():
            raise ConsoleError("missing payload")
        head, sep, tail = text.strip().partition(" ")
        matches = [cid for cid in self.relays if cid == head or cid.startswith(head)]
        if len(matches) > 1:
            raise ConsoleError(f"ambiguous channel {head!r}")
        if len(matches) == 1 and (sep or head == matches[0]):
            channel = self.relays[matches[0]]
            payload = tail if raw else tail.strip()
            return channel, payload
        return self._pick_channel(""), text if raw else text.strip()

    def _relay_opts(self, parts: list[str]) -> tuple[str, int]:
        protocol = "udp"
        lifetime = 60
        for part in parts:
            if part in ("udp", "tcp"):
                protocol = part
            else:
                try:
                    lifetime = int(part)
                except ValueError as exc:
                    raise ConsoleError("relay options are [udp|tcp] [seconds]") from exc
        if lifetime < 1:
            raise ConsoleError("lifetime must be positive")
        return protocol, lifetime

    def _show_http(self, status: int, body: object, text: str) -> None:
        if isinstance(body, (dict, list)):
            self.out(f"HTTP {status}\n{pretty(body)}")
        else:
            self.out(f"HTTP {status}\n{text}")

    def _prompt(self) -> str:
        if not self.user_id:
            label = "offline"
        else:
            name = self.alias or self.user_id[:8]
            if self.ws is not None and self.ws.alive:
                role = "host" if self.is_host else "member"
                label = f"{role}:{name}"
            else:
                label = name
        return f"mm {label}> "


def build_context(insecure: bool, ca: str | None) -> ssl.SSLContext:
    context = ssl.create_default_context()
    if insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    elif ca:
        context.load_verify_locations(cafile=ca)
    return context


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass
    parser = argparse.ArgumentParser(
        prog="mmcli",
        description="REPL client for the matchmaker. One running process is one player.",
    )
    parser.add_argument("--url", default=os.environ.get("MATCHMAKER_URL", "https://127.0.0.1:443"))
    parser.add_argument("--insecure", action="store_true", help="skip server certificate checks")
    parser.add_argument("--ca", help="PEM file used to verify the server certificate")
    parser.add_argument("--game", default=os.environ.get("MATCHMAKER_GAME"), help="game UUID")
    parser.add_argument("--token", default=os.environ.get("MATCHMAKER_TOKEN"), help="raw game token")
    parser.add_argument("--alias", default=os.environ.get("MATCHMAKER_ALIAS", ""))
    parser.add_argument("--external-id", default=os.environ.get("MATCHMAKER_EXTERNAL_ID", ""))
    parser.add_argument("--stats", default=os.environ.get("MATCHMAKER_STATS", "127.0.0.1:7711"))
    parser.add_argument(
        "--heartbeat",
        type=float,
        default=float(os.environ.get("MATCHMAKER_HEARTBEAT", "2")),
        help="seconds between automatic host heartbeats; 0 disables them",
    )
    parser.add_argument("--verbose", action="store_true", help="print automatic heartbeat replies")
    args = parser.parse_args(argv)
    parsed = urllib.parse.urlparse(args.url)
    if parsed.scheme != "https" or not parsed.hostname:
        parser.error("--url must be an https origin, for example https://127.0.0.1:443")
    if args.insecure and args.ca:
        parser.error("pass either --insecure or --ca")
    context = build_context(args.insecure, args.ca)
    http = HttpApi(parsed.hostname, parsed.port or 443, context, timeout=15)
    try:
        Console(http, context, args).run()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

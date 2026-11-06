CREATE TABLE IF NOT EXISTS schema_migrations (
  version TEXT PRIMARY KEY,
  applied_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS config (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS regions (
  id TEXT PRIMARY KEY,
  public_url TEXT NOT NULL,
  sort_order INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS games (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  token_hash TEXT NOT NULL,
  promote_host_on_leave INTEGER NOT NULL DEFAULT 0,
  allow_join_in_progress INTEGER NOT NULL DEFAULT 0,
  limits_json TEXT NOT NULL DEFAULT '{}',
  created_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS logs (
  id TEXT PRIMARY KEY,
  ts_ms INTEGER NOT NULL,
  level TEXT NOT NULL,
  category TEXT NOT NULL,
  message TEXT NOT NULL,
  game_id TEXT,
  detail_json TEXT
);

CREATE INDEX IF NOT EXISTS logs_ts ON logs(ts_ms);

INSERT INTO config(key, value) VALUES
  ('listen', '0.0.0.0:443'),
  ('public_host', 'localhost'),
  ('public_url', 'https://localhost'),
  ('region_id', 'local'),
  ('jwt_secret', 'change-me'),
  ('jwt_ttl_sec', '900'),
  ('admit_sec', '60'),
  ('join_hold_sec', '10'),
  ('host_heartbeat_interval_sec', '2'),
  ('host_heartbeat_misses', '3'),
  ('max_threads', '1'),
  ('stats_listen', '127.0.0.1:7711'),
  ('tls_cert', ''),
  ('tls_key', ''),
  ('tls_min_version', '1.2'),
  ('http_gzip', '1'),
  ('http_gzip_min_bytes', '256'),
  ('websocket_permessage_deflate', '1'),
  ('websocket_deflate_level', '3'),
  ('websocket_max_window_bits', '12'),
  ('retention_days', '30'),
  ('max_message_bytes', '65536'),
  ('max_connections', '10000'),
  ('max_outbound_queue_bytes', '1048576'),
  ('max_concurrent_channels', '4096'),
  ('max_channel_lifetime_sec', '7200'),
  ('max_idle_sec', '120'),
  ('max_udp_packet_bytes', '1200'),
  ('max_bytes_per_sec_per_channel', '0'),
  ('max_players_per_lobby', '64'),
  ('max_password_length', '64'),
  ('max_frame_bytes', '65536'),
  ('relay_bind', '0.0.0.0'),
  ('relay_port_start', '30000'),
  ('relay_port_end', '39999'),
  ('relay_tcp_enabled', '0'),
  ('auth_fail_limit', '20'),
  ('auth_fail_window_sec', '60'),
  ('probe_timeout_ms', '500')
ON CONFLICT(key) DO NOTHING;

INSERT INTO regions(id, public_url, sort_order) VALUES
  ('local', 'https://localhost', 0)
ON CONFLICT(id) DO NOTHING;

INSERT INTO config(key, value) VALUES
  ('relay_wt_enabled', '0'),
  ('wt_listen', '0.0.0.0:443'),
  ('cors_origins', ''),
  ('wt_ticket_sec', '30')
ON CONFLICT(key) DO NOTHING;

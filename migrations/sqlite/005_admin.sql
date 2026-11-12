INSERT INTO config(key, value) VALUES
  ('admin_token', '')
ON CONFLICT(key) DO NOTHING;

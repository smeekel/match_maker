INSERT INTO config(key, value)
SELECT 'max_threads', CASE WHEN CAST(value AS INTEGER) > 0 THEN value ELSE '1' END
FROM config
WHERE key = 'threads'
ON CONFLICT(key) DO NOTHING;

INSERT INTO config(key, value) VALUES ('max_threads', '1')
ON CONFLICT(key) DO NOTHING;

DELETE FROM config WHERE key = 'threads';

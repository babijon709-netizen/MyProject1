-- Run once in Cloudflare Dashboard > D1 > benzware-control > Console.
-- Default disabled; on/off commands update this single row via the Worker.
CREATE TABLE IF NOT EXISTS benzware_control (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  enabled INTEGER NOT NULL CHECK (enabled IN (0, 1))
);
INSERT OR IGNORE INTO benzware_control (id, enabled) VALUES (1, 0);

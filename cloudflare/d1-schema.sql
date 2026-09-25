-- Run once in Cloudflare Dashboard > D1 > benzware-control > Console.
-- Default disabled; on/off commands update this single row via the Worker.
CREATE TABLE IF NOT EXISTS benzware_control (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  enabled INTEGER NOT NULL CHECK (enabled IN (0, 1))
);
INSERT OR IGNORE INTO benzware_control (id, enabled) VALUES (1, 0);

-- Device IDs are SHA-256 digests of Android IDs; never store raw serial/Android ID.
CREATE TABLE IF NOT EXISTS benzware_devices (
  hwid TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  banned INTEGER NOT NULL DEFAULT 0 CHECK (banned IN (0, 1)),
  first_seen TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Presence is a recent successful auth heartbeat, not proof of being inside a game.
-- Separate table lets older device databases migrate with CREATE IF NOT EXISTS.
CREATE TABLE IF NOT EXISTS benzware_presence (
  hwid TEXT PRIMARY KEY,
  last_seen INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS benzware_presence_seen ON benzware_presence(last_seen);

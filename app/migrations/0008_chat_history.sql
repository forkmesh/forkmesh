-- ForkMesh D1 migration 0008 — retained encrypted chat history.
-- The relay keeps the last few days of durable chat messages per room so a node
-- that joins later sees some recent history even when no peer is online to
-- replay it. body is the opaque encrypted envelope exactly as relayed; the
-- server never sees plaintext. room_key matches the room Durable Object name.
-- The worker also creates this via ensure_schema in src/entry.py.

CREATE TABLE IF NOT EXISTS chat_history (
  room_key TEXT NOT NULL,    -- e.g. "repo:owner/name:room:general"
  msg_id   TEXT NOT NULL,    -- message id from the envelope (dedupes replays)
  ts       INTEGER NOT NULL, -- epoch ms the message was relayed
  body     TEXT NOT NULL,    -- encrypted envelope JSON, verbatim
  PRIMARY KEY (room_key, msg_id)
);

CREATE INDEX IF NOT EXISTS idx_chat_history_room_ts ON chat_history(room_key, ts);

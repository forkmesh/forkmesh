-- Thread subscriptions (issue #361): who follows a given issue/PR so a reply
-- reaches them, not just people the comment @mentions. Auto-populated for anyone
-- who comments; also set/cleared by the signed /subscribe endpoint.
-- The data column holds the subscriber's (public) node name plus a `muted` flag
-- so an explicit unsubscribe survives a later auto-subscribe.

CREATE TABLE IF NOT EXISTS thread_subscriptions (
  thread_bi     TEXT NOT NULL,    -- blind_index("thread:<owner>/<repo>:<source>:<number>")
  subscriber_bi TEXT NOT NULL,    -- blind_index(subscriber node name)
  ts            INTEGER NOT NULL,
  data          TEXT NOT NULL,    -- AES-GCM encrypted {node, muted}
  PRIMARY KEY (thread_bi, subscriber_bi)
);

CREATE INDEX IF NOT EXISTS idx_thread_subscriptions_thread
  ON thread_subscriptions(thread_bi);

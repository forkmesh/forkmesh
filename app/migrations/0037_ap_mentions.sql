-- Inbound fediverse repo-actor mentions ("@owner.repo@host ...") already
-- processed into an issue-inbox submission (or confidently classified as
-- not-an-issue by the AI intent pass). Keyed by
-- blind_index("ap-mention:<note id>") so Mastodon redeliveries of the same
-- post can never file the same issue twice.
CREATE TABLE IF NOT EXISTS ap_mentions (
    remote_id_bi TEXT PRIMARY KEY,
    ts INTEGER NOT NULL
);

-- ForkMesh D1 migration 0005 — manual email verification queue.
-- Until a real email service (e.g. Amazon SES) is wired up, a newly finalized
-- account's email is verified by hand by an administrator. Each new account is
-- enqueued here; an admin's Qt client lists the queue and verifies entries,
-- which sets email_verified on the account and removes the row. data is an
-- AES-GCM-encrypted {name, email, joinedAt} blob (no plaintext). The worker also
-- creates this lazily (ensure_schema in src/entry.py).

CREATE TABLE IF NOT EXISTS pending_verifications (
  name_bi TEXT PRIMARY KEY,   -- blind_index(nodeName)
  data    TEXT NOT NULL        -- AES-GCM encrypted {name, email, joinedAt}
);

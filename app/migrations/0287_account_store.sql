CREATE TABLE IF NOT EXISTS account_store (
        account_bi TEXT NOT NULL,
        item_key TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT 'settings',
        data TEXT NOT NULL,
        bytes INTEGER NOT NULL DEFAULT 0 CHECK(bytes >= 0),
        revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
        updated_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (account_bi, item_key));

CREATE TABLE IF NOT EXISTS account_store_keys (
        account_bi TEXT PRIMARY KEY,
        wrapped TEXT NOT NULL,
        created_at INTEGER NOT NULL DEFAULT 0);

-- lugbulk-labels-web schema
--
-- Kept intentionally small: users, the sheets they've added, and a log of
-- past generate runs. Generated PDFs/CSVs are NOT stored — every download
-- re-runs against the live sheet; `runs` is a history log only, not a
-- file store (see the design discussion this schema follows from).

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    google_sub         TEXT NOT NULL UNIQUE,  -- Google account's stable subject id
    email              TEXT NOT NULL,
    -- Refresh token is encrypted at rest before insertion (app-layer AES-GCM,
    -- key from env/secrets — never store it plaintext).
    refresh_token_enc  BLOB NOT NULL,
    created_at         TEXT NOT NULL DEFAULT (datetime('now')),
    last_login_at      TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS sheets (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    sheet_id      TEXT NOT NULL,   -- the Google Sheets file ID (from the URL)
    display_name  TEXT NOT NULL,   -- user-facing label, e.g. "ArkLUG 2026"
    added_at      TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE (user_id, sheet_id)
);

CREATE TABLE IF NOT EXISTS runs (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    sheet_id      INTEGER NOT NULL REFERENCES sheets(id) ON DELETE CASCADE,
    report_type   TEXT NOT NULL CHECK (report_type IN ('labels', 'lot_counts')),
    generated_at  TEXT NOT NULL DEFAULT (datetime('now')),
    item_count    INTEGER NOT NULL,  -- label count, or lot count depending on report_type
    status        TEXT NOT NULL CHECK (status IN ('ok', 'error')),
    error_message TEXT  -- set when status = 'error'; NULL otherwise
);

CREATE INDEX IF NOT EXISTS idx_sheets_user ON sheets(user_id);
CREATE INDEX IF NOT EXISTS idx_runs_sheet ON runs(sheet_id, generated_at DESC);

-- Session table: short-lived login sessions, separate from the long-lived
-- OAuth refresh token above. Session cookie value -> user, with an
-- expiry so stale sessions get swept.
CREATE TABLE IF NOT EXISTS sessions (
    token       TEXT PRIMARY KEY,  -- random session token, set as an HttpOnly cookie
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  TEXT NOT NULL DEFAULT (datetime('now')),
    expires_at  TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);

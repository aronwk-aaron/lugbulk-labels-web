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
    -- When we last confirmed (via the Sheets API) that this user can open
    -- the sheet; NULL for rows saved before that check existed. Editing the
    -- sheet's shared label design requires it.
    verified_at   TEXT,
    UNIQUE (user_id, sheet_id)
);

CREATE TABLE IF NOT EXISTS runs (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    sheet_id      INTEGER NOT NULL REFERENCES sheets(id) ON DELETE CASCADE,
    report_type   TEXT NOT NULL CHECK (report_type IN ('labels', 'lot_counts', 'parts', 'checklist', 'bundle')),
    generated_at  TEXT NOT NULL DEFAULT (datetime('now')),
    item_count    INTEGER NOT NULL,  -- labels, people, or parts, depending on report_type
    status        TEXT NOT NULL CHECK (status IN ('ok', 'error')),
    error_message TEXT  -- set when status = 'error'; NULL otherwise
);

CREATE INDEX IF NOT EXISTS idx_sheets_user ON sheets(user_id);
CREATE INDEX IF NOT EXISTS idx_runs_sheet ON runs(sheet_id, generated_at DESC);

-- Session table: short-lived login sessions, separate from the long-lived
-- OAuth refresh token above. Session cookie value -> user, with an
-- expiry so stale sessions get swept.
CREATE TABLE IF NOT EXISTS sessions (
    -- SHA-256 (hex) of the random session token; the raw token only ever
    -- lives in the user's HttpOnly cookie.
    token       TEXT PRIMARY KEY,
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  TEXT NOT NULL DEFAULT (datetime('now')),
    expires_at  TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);

-- Label design per Google Sheet (label stock, part order, parts switched
-- off), shared by everyone who has that sheet saved, so they all print the
-- same labels. Keyed by the Google file id, not the per-user sheets row.
CREATE TABLE IF NOT EXISTS sheet_designs (
    google_sheet_id TEXT PRIMARY KEY,
    label_spec      TEXT NOT NULL,
    part_order      TEXT NOT NULL,
    hidden_parts    TEXT NOT NULL,  -- comma-separated LabelPart names
    -- Packing checklist / parts list / lot counts settings and the zip's
    -- file choices: a JSON object (at most 4 KB) stored as the browser sent
    -- it, NULL if never saved. Db::migrate() adds it to older databases.
    report_options  TEXT,
    -- "Keep each part on one sheet": 'off' (labels run on continuously) or
    -- 'optimize' (packed so no part is split, static/js/packing.js).
    -- Db::migrate() adds it to older databases.
    keep_parts      TEXT NOT NULL DEFAULT 'off' CHECK (keep_parts IN ('off', 'optimize')),
    updated_by     INTEGER REFERENCES users(id) ON DELETE SET NULL,
    updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
);

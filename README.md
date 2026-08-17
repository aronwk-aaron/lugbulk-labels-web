# lugbulk-labels-web

> **AI disclaimer:** This project was scaffolded with assistance from Claude
> (Anthropic). Review the code before relying on it, especially the OAuth
> flow, token storage, and PDF generation logic.

Hosted, multi-user counterpart to
[lugbulk-label](https://github.com/aronwk-aaron/lugbulk-label) (the local
Python CLI). Lets a handful of trusted LUG organizers log in with their own
Google account, point at their own bulk-order sheet, and generate label
PDFs / lot-count reports without installing Python or a service account key
locally.

**Status: scaffolding.** The build (Crow + SQLite + PoDoFo + CURL +
OpenSSL, in Docker) is proven end-to-end with a `/healthz` route; the
actual OAuth flow, sheet pivoting, and PDF/report generation are not yet
implemented. See "Design" below for the intended shape.

## Stack

- **[CrowCpp](https://github.com/CrowCpp/Crow)** — C++ web framework, fetched via CMake `FetchContent` (header-only, not vendored in-repo)
- **SQLite** — users, saved sheets, and a run history log. No generated files are persisted; every download re-runs against the live sheet.
- **[PoDoFo](http://podofo.sourceforge.net)** — PDF generation. Chosen over libharu because libharu isn't packaged for Debian bookworm (our Docker base); PoDoFo is (`libpodofo-dev`).
- **libcurl + OpenSSL** — outbound HTTPS for the Google OAuth token exchange and Sheets API calls.
- **Docker** — deploy target is a single container on the maintainer's server, with `/data` as a mounted volume (SQLite DB + image cache).

## Design

Each user authenticates via Google OAuth and authorizes read access to
their own sheets — there is no shared service account like the CLI uses.
Sheet column layout is a fixed template (same shape as `lugbulk-label`'s
"Order Here" tab) — not per-sheet configurable in v1.

**Flow:** login → dashboard listing saved sheets → per sheet, "Generate
Labels" or "Generate Lot Counts" → runs synchronously against the live
sheet → PDF/CSV returned as a download. Nothing is stored on disk after
the response goes out; `runs` (see `sql/schema.sql`) is a history log
only — timestamp, report type, item count, status — not a file store.

**Data model:** see [`sql/schema.sql`](sql/schema.sql) — `users`,
`sheets`, `runs`, `sessions`.

**Ported from the CLI, planned:**
- Label PDF rendering (`render_labels.py` → PoDoFo)
- Lot-count CSV + PDF (`manifest.py`'s `write_lot_counts_*`)
- Sheet pivoting + duplicate/bad-qty detection (`sheets_source.py`),
  surfaced as inline errors on Generate

**Out of scope for v1** (not carried over from the CLI): the full
`--manifest` summary report, `--per-person` PDFs, `--label-spec` format
choice (defaults to Avery 5160 only), `--sort-by` toggle (one fixed
default order).

## Local development

```
cp .env.example .env   # fill in Google OAuth client id/secret (see below)
docker compose up --build
```

Serves on `http://localhost:8080`. `/healthz` returns `200 ok` once the
container is up.

### Google OAuth setup

1. [console.cloud.google.com](https://console.cloud.google.com) → a
   project with the Sheets API enabled (same as `lugbulk-label`'s service
   account setup, but this time for an OAuth client instead of a service
   account).
2. **APIs & Services → OAuth consent screen** — configure as Internal or
   External + Testing, add the organizers' Google accounts as test users
   if kept in Testing mode (fine for a "few trusted organizers" audience).
3. **APIs & Services → Credentials → Create Credentials → OAuth client
   ID** — type "Web application". Add an authorized redirect URI matching
   `GOOGLE_OAUTH_REDIRECT_URI` (e.g. `http://localhost:8080/auth/callback`
   for local dev; your real domain's callback URL in production).
4. Copy the client ID and secret into `.env`.

## Building without Docker

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/lugbulk_labels_web
```

Requires: a C++20 compiler, CMake ≥ 3.20, and dev packages for OpenSSL,
SQLite3, libcurl, PoDoFo, and standalone Asio (`libssl-dev
libsqlite3-dev libcurl4-openssl-dev libpodofo-dev libasio-dev` on
Debian/Ubuntu).

## Project files

| Path | Purpose |
|---|---|
| `src/main.cpp` | Entry point and route definitions |
| `CMakeLists.txt` | Build config; fetches Crow, locates PoDoFo/SQLite3/CURL/OpenSSL/Asio |
| `sql/schema.sql` | SQLite schema: users, sheets, runs, sessions |
| `Dockerfile` | Multi-stage build (Debian bookworm base) |
| `docker-compose.yml` | Local dev convenience — build + run with a persistent volume |
| `.env.example` | Template for OAuth client credentials and the token-encryption key |

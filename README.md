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

**Status: early build-out.** The build (Crow + SQLite + PoDoFo + CURL +
OpenSSL, in Docker) is proven end-to-end. Implemented so far: Google OAuth
login (encrypted refresh token storage, session cookies), and a Drive-backed
sheet search/picker (`GET /sheets/search`) plus saved-sheet CRUD
(`GET/POST /sheets`, `DELETE /sheets/:id`). Sheet pivoting and PDF/report
generation are not yet implemented. See "Design" below for the intended
shape.

## Stack

- **[CrowCpp](https://github.com/CrowCpp/Crow)** — C++ web framework, fetched via CMake `FetchContent` (header-only, not vendored in-repo)
- **SQLite** — users, saved sheets, and a run history log. No generated files are persisted; every download re-runs against the live sheet.
- **[PoDoFo](http://podofo.sourceforge.net)** — PDF generation. Chosen over libharu because libharu isn't packaged for Debian bookworm (our Docker base); PoDoFo is (`libpodofo-dev`).
- **libcurl + OpenSSL** — outbound HTTPS for the Google OAuth token exchange and Sheets API calls.
- **Docker** — deploy target is a single container on the maintainer's server, with `/data` as a mounted volume (SQLite DB + image cache).

## Design

Each user authenticates via Google OAuth and authorizes read access to
their own sheets — there is no shared service account like the CLI uses.
OAuth scopes requested: `openid email` (identity),
`spreadsheets.readonly` (reading the sheet data itself), and
`drive.metadata.readonly` (listing/searching the user's Drive by file name
so they can pick a sheet — file *contents* are never read via the Drive
API, only via the Sheets API scope, and only for a sheet the user has
explicitly saved). Sheet column layout is a fixed template (same shape as
`lugbulk-label`'s "Order Here" tab) — not per-sheet configurable in v1.

**Flow:** login → search/pick a sheet from Drive (`GET /sheets/search?q=`)
→ save it (`POST /sheets`) → dashboard listing saved sheets → per sheet,
"Generate Labels" or "Generate Lot Counts" → runs synchronously against
the live sheet → PDF/CSV returned as a download. Nothing is stored on disk
after the response goes out; `runs` (see `sql/schema.sql`) is a history
log only — timestamp, report type, item count, status — not a file store.

**Access tokens are never persisted.** Only the (encrypted) OAuth
*refresh* token is stored; a short-lived access token is minted from it
in-memory on each request that needs to call Google, used immediately, and
discarded.

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

**"Google hasn't verified this app" screen:** while the OAuth consent
screen is in Testing mode (the default above), every login shows Google's
unverified-app interstitial for anyone signing in — including test users
who were explicitly added. This is expected, not a bug: it goes away only
after submitting the app for Google's verification review (requires a
public privacy policy, homepage, etc. — not worth it for a handful of
trusted organizers). To get past it as a test user: click **Advanced** →
**"Go to lugbulk-labels-web (unsafe)"** → **Continue**. If the Advanced
link doesn't appear, the signed-in Google account isn't in **OAuth consent
screen → Test users** yet — add it there.

**TODO for later:** either live with the click-through screen permanently
(fine for this audience), or if it becomes annoying, look at Google's
verification process for real — see
[Learn more](https://support.google.com/cloud/answer/7454865) on that
screen.

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

## Container image

Every push to `master` and every `v*` tag builds and publishes to GitHub
Container Registry via `.github/workflows/docker-publish.yml` — no Docker
Hub account or registry secrets needed, it authenticates with the repo's
built-in `GITHUB_TOKEN`. Pull requests build (to catch a broken Dockerfile)
but never push.

```
docker pull ghcr.io/aronwk-aaron/lugbulk-labels-web:latest
```

Tags: `latest` and the short commit SHA on every `master` push; `X.Y.Z`,
`X.Y`, and `X` on a `vX.Y.Z` tag push.

## Project files

| Path | Purpose |
|---|---|
| `src/main.cpp` | Entry point and route definitions |
| `src/config.{h,cpp}` | Env-var configuration loading |
| `src/crypto.{h,cpp}` | AES-256-GCM refresh-token encryption, base64, CSPRNG tokens |
| `src/db.{h,cpp}` | SQLite access layer (users/sheets/sessions) |
| `src/oauth.{h,cpp}` | Google OAuth token exchange + userinfo + Drive sheet search |
| `CMakeLists.txt` | Build config; fetches Crow, locates PoDoFo/SQLite3/CURL/OpenSSL/Asio |
| `sql/schema.sql` | SQLite schema: users, sheets, runs, sessions |
| `templates/dashboard.html` | Mustache template for the logged-in dashboard page (Crow's bundled `crow::mustache`) |
| `Dockerfile` | Multi-stage build (Debian bookworm base) |
| `.github/workflows/docker-publish.yml` | CI: builds and publishes the image to GHCR |
| `docker-compose.yml` | Local dev convenience — build + run with a persistent volume |
| `.env.example` | Template for OAuth client credentials and the token-encryption key |

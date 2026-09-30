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

**Status: working, pre-release.** Google OAuth login (encrypted refresh
token storage, hashed session tokens), a Google Picker sheet chooser,
generation of label PDFs, a parts list and lot counts from the live sheet,
and a per-sheet "last run" history are implemented.

## Two ways in

- **Upload a spreadsheet** — no account needed. Upload the order sheet as
  an Excel `.xlsx` (Google Sheets: File → Download → Microsoft Excel) or a
  `.csv` of its "Order Here" tab, pick a label design, and download labels
  and reports. The file is read in memory for that one request and never
  stored. Up to 10 MB; an `.xlsx` may not unpack past 64 MB.
- **Sign in with Google** (optional, when the OAuth client is configured)
  — save your Google Sheets and read them live each time, with label
  designs saved per sheet and shared with everyone who has that sheet.

Without `GOOGLE_OAUTH_CLIENT_ID`/`_SECRET` the app runs upload-only (no
`TOKEN_ENCRYPTION_KEY` needed then). Set `PUBLIC_URL` to the app's public
address (e.g. `https://lugbulk.example.org`); it defaults to the origin of
`GOOGLE_OAUTH_REDIRECT_URI` when Google is on. Adding a Google Sheet also
needs `GOOGLE_API_KEY` and `GOOGLE_APP_ID` for the Google Picker (see
[Google OAuth setup](#google-oauth-setup)); without them, signed-in users
can still open sheets already in their list.

## What it generates

- **Labels** — one per (person, part), grouped by part: heaviest parts
  first (or lightest / sheet order), smallest quantity first within a
  part, each numbered "3 of 10" within its part. Each label shows the
  part photo, Element ID (bold), quantity, both LEGO and BrickLink color
  names, description and the person's name. Trans, white, and very pale
  parts are shown on a light gray tile so they don't vanish when printed.
- **Label design** — per sheet, with a live preview (sample parts, the
  real PDF renderer): switch any label part on or off — photo, element ID,
  quantity, LEGO and BrickLink color, description, name, "3 of 10" count,
  gray tile behind clear/white parts, color swatch, QR code linking to the
  part on BrickLink — and pick the stock and part order. Designs are saved
  per Google Sheet, so everyone with that sheet prints the same labels;
  Labels, Checklist and Parts list downloads follow it. An **alignment
  test page** prints the stock's outlines on plain paper to check the
  printer first.
- **Download all** — one `.zip` with the labels, packing checklist, parts
  list and lot counts (PDF and CSV) and the sheet check, for a saved sheet
  or an upload.
- **Packing checklist** (PDF) — one page per person listing their parts
  with a tick box per bag.
- **Label stock** — pick from 51 Avery (US-Letter and A4) and Dymo
  LabelWriter stocks, searchable by any part number on the box (e.g.
  8162, L7163, 30857); default Avery 5162. The inventory is
  `data/label_specs.json`, generated from the gLabels template database by
  the CLI's `tools/update_label_specs.py`.
- **Parts list** (PDF/CSV) — one row per part in label order: total
  pieces, number of people ordering it, weight.
- **Lot counts** (PDF/CSV) — per person: number of labels and pieces.
- **Check sheet** — label count and any data issues (non-numeric
  quantities, duplicates, bad element IDs, colors with no LEGO/BrickLink
  match, missing descriptions) without generating anything.

Part weight comes from an optional `Weight` column (grams per piece) on
the sheet, then the BrickLink catalog (if configured, below), then an
estimate from the description's stud dimensions (`PLATE 4X8`,
`BRICK 1X1X1 2/3`); parts with none of those sort last. Whichever of the
`LEGO Color`/`BL Color` columns is blank is filled from the color table in
`src/colors.cpp`, or failing that from BrickLink.

### BrickLink (optional)

Real part weights (for part order) and BrickLink color names (for colors a
sheet is missing) come from BrickLink's catalog download — its API is for
sellers only, but any free account can download the catalog at
[bricklink.com/catalogDownload.asp](https://www.bricklink.com/catalogDownload.asp)
as **Tab-Delimited File**:

1. **Catalog Items → Parts**, with **Include Weight** ticked;
2. **Part and Color Codes**.

Put both in `<data dir>/bricklink/` (e.g.
`/mnt/user/appdata/lugbulk-labels-web/bricklink/`). Any file names work —
they're recognised by their header row — and replacing them is picked up
automatically on the next report. Without them, weights are estimated.

## Stack

- **[CrowCpp](https://github.com/CrowCpp/Crow)** — C++ web framework, fetched via CMake `FetchContent` (header-only, not vendored in-repo)
- **SQLite** — users, saved sheets, and a run history log. No generated files are persisted; every download re-runs against the live sheet.
- **[PoDoFo](http://podofo.sourceforge.net)** — PDF generation. Chosen over libharu because libharu isn't packaged for Debian bookworm (our Docker base); PoDoFo is (`libpodofo-dev`).
- **libcurl + OpenSSL** — outbound HTTPS for the Google OAuth token exchange and Sheets API calls.
- **Docker** — deploy target is a single container on the maintainer's server, with `/data` as a mounted volume (SQLite DB + image cache).

## Design

Each user authenticates via Google OAuth and authorizes read access to
their own sheets — there is no shared service account like the CLI uses.
OAuth scopes requested: `openid email` (identity) and `drive.file` —
Google's non-sensitive per-file scope, which only reaches the files the
user opens with this app through the
[Google Picker](https://developers.google.com/drive/picker). The app never
sees the rest of their Drive; the Sheets API reads a picked sheet under
`drive.file`. Because no sensitive or restricted scope is requested, the
OAuth app can be published without Google's restricted-scope verification.
Users who signed in before the switch (when the app asked for
`spreadsheets.readonly` + `drive.metadata.readonly`) are sent through
sign-in again the first time they open the Picker, and a saved sheet Google
refuses to open must be chosen again with **Pick a Google Sheet…**. The "Order Here" tab's columns are found by header text (`Element ID` /
`Part Number`, `Description`, `LEGO Color`, `BL Color`, `Weight`), and both
known ways of laying out people are recognized: a `qty` marker row under
the names (the ArkLUG sheet) or (name, running cost) header pairs (2026's
master sheet) — see `src/sheet_pivot.h`.

**Flow:** login → **Pick a Google Sheet…** opens the Google Picker (the page
gets a short-lived `drive.file` token, the API key and the app id from
`GET /auth/picker-token`) → save the picked sheet (`POST /sheets`) → dashboard listing saved sheets → per sheet,
Check sheet (`GET /sheets/:id/check`), Labels (`POST /sheets/:id/labels?spec=&order=`),
Parts list (`POST /sheets/:id/parts?format=&order=`) or Lot counts
(`POST /sheets/:id/lots?format=`) → runs synchronously against the live
sheet → PDF/CSV returned as a download. Nothing is stored on disk
after the response goes out; `runs` (see `sql/schema.sql`) is a history
log only — timestamp, report type, item count, status — not a file store.

**Access tokens are never persisted.** Only the (encrypted) OAuth
*refresh* token is stored; a short-lived access token is minted from it
in-memory on each request that needs to call Google, used immediately, and
discarded.

**Data model:** see [`sql/schema.sql`](sql/schema.sql) — `users`,
`sheets`, `runs`, `sessions`.

The sheet pivot, ordering, color table and label layout mirror the CLI's
`pivot.py`, `ordering.py`, `colors.py` and `render_labels.py`; keep the
two in step when changing either.

**Not carried over from the CLI:** the full `--manifest` summary report,
`--per-person` PDFs, `--sort-by` toggle (last-name order only), and
per-event color/weight overrides (fix the sheet instead).

## Security and abuse limits

Generating a report is expensive (a Google Sheets read, dozens of photo
downloads and BrickLink lookups, PDF rendering), so the server limits what
any one person — or script — can make it do:

| What | Limit |
|---|---|
| Who can sign in | `ALLOWED_EMAILS` (addresses and/or `@domain`s); verified Google email required. Removing someone locks them out immediately. **Set this** — unset lets any Google account in and logs a warning at startup. |
| Uploads | Same report limits as sign-in, keyed by client IP; 10 MB per file |
| Reports and Check sheet | 6 at once, then one per 2 minutes, per user; one running per user; `MAX_CONCURRENT_JOBS` (default 2) server-wide. Over the limit gets an immediate "try again" (HTTP 429), never a queue. |
| Drive search | 10 at once, then one per 3 seconds, per user |
| Any request | 120 at once, then 10/second, per client IP (sign-in routes: 10, then one per 6 seconds) |
| Request body | 64 KB (Crow patched at build time — `cmake/patch_crow.cmake`); bigger uploads are dropped |
| Sheet size | 3,000 rows read; 20,000 labels / 2,000 parts per run; 32 MB Google response |
| Saved sheets | 50 per user (only sheets your Google account can open); 10 sessions per user |
| Preview / test page / design saves | 20, then 1/second, per user |
| Part photos | only digit element IDs, only from LEGO's CDN over HTTPS, 2 MB max |

Behind a reverse proxy, set `TRUST_PROXY=1` so limits apply per visitor
rather than to the proxy's single IP (only with a proxy that sets
`X-Forwarded-For` — otherwise clients could fake it). A proxy is also the
place for TLS and connection limits; the app itself serves plain HTTP.

**Keeping each organizer's data private:**

- Every sheet route checks the sheet belongs to the signed-in user
  (someone else's sheet is indistinguishable from one that doesn't exist:
  404). Sheets are only saved if your Google account can open them, and
  all Google reads use your own Google authorization — the app can't read a
  sheet on your behalf that you can't read yourself.
- Label designs are shared per Google Sheet by design; only people whose
  Google account can open the sheet can change one.
- No response is cached anywhere (`Cache-Control: no-store, private`), so a
  shared computer's Back button or a caching proxy can't show one
  organizer's data to another. Logging out also clears the browser cache
  for the site, and sign-ins last 14 days.
- Nothing personal is logged: no emails, names, tokens or request URLs
  (Crow's access log is off); errors log only internal user/sheet ids.

**Hardening:** a strict Content-Security-Policy (the dashboard's scripts
run only with a per-response nonce; no other script can), `nosniff`,
`X-Frame-Options: DENY`, HSTS over https, and a `text/plain` default;
state-changing requests from another origin are refused; session cookies
are HttpOnly, SameSite=Lax (Secure over https) and stored hashed; refresh
tokens are AES-256-GCM encrypted at rest. Third-party code is pinned by
commit (GitHub Actions, Crow, the QR library) and kept current by
Dependabot.

## Local development

```
cp .env.example .env   # fill in Google OAuth client id/secret (see below)
docker compose up --build
```

Serves on `http://localhost:8080`. `/healthz` returns `200 ok` once the
container is up.

### Google OAuth setup

1. [console.cloud.google.com](https://console.cloud.google.com) → a
   project with the **Google Sheets API** and the **Google Picker API**
   enabled (APIs & Services → Library).
2. **APIs & Services → OAuth consent screen** (Google Auth Platform) —
   scopes: `openid`, `.../auth/userinfo.email` and `.../auth/drive.file`
   only. `drive.file` is non-sensitive, so the app can be published
   (Audience → **Publish app**) without restricted-scope verification.
3. **APIs & Services → Credentials → Create Credentials → OAuth client
   ID** — type "Web application". Add an authorized redirect URI matching
   `GOOGLE_OAUTH_REDIRECT_URI` (e.g. `http://localhost:8080/auth/callback`
   for local dev; your real domain's callback URL in production).
4. Copy the client ID and secret into `.env`.
5. **Credentials → Create Credentials → API key** for the Picker. Restrict
   it: Application restrictions → **Websites**, the app's origin (e.g.
   `https://lugbulk.example.org/*`); API restrictions → **Google Picker
   API** only. Put it in `GOOGLE_API_KEY`. It is sent to signed-in users'
   browsers, which is how Picker keys work — the restrictions are what
   protect it.
6. `GOOGLE_APP_ID` is the project **number** (not the project id): Cloud
   Console → the project's **Dashboard** / **IAM & Admin → Settings**.

**"Google hasn't verified this app" screen:** it shows while the consent
screen is in **Testing** (only listed test users can sign in; click
**Advanced** → **"Go to lugbulk-labels-web (unsafe)"**). Since the app only
asks for `openid`, `email` and `drive.file` — none of them sensitive or
restricted — publishing it (Audience → **Publish app**) makes sign-in open
to any Google account without the security assessment that restricted
Drive scopes need. Use `ALLOWED_EMAILS` to keep it to your organizers.

## Building without Docker

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/lugbulk_labels_web
```

Requires: a C++20 compiler, CMake ≥ 3.20, and dev packages for OpenSSL,
SQLite3, libcurl, libjpeg, PoDoFo 0.9.x, and standalone Asio (`libssl-dev
libsqlite3-dev libcurl4-openssl-dev libjpeg-dev libpodofo-dev libasio-dev`
on Debian/Ubuntu).

Unit tests (sheet pivoting, colors, ordering, reports, image outlining,
PDF rendering for every label size) build alongside the server and run in
the Docker build:

```
ctest --test-dir build --output-on-failure
```

## Container image and releases

`.github/workflows/docker-publish.yml` builds, tests and publishes the
image to GitHub Container Registry with the repo's built-in
`GITHUB_TOKEN` (no registry secrets):

| Event | Image tags | GitHub release |
|---|---|---|
| Pull request | — (build + tests + smoke test only) | — |
| Push to `master` | `canary`, short SHA | Rolling **canary** pre-release, always the head of `master`, with the changes since the last release |
| Push a `vX.Y.Z` tag | `X.Y.Z`, `X.Y`, `X`, `latest` | Release `vX.Y.Z` with generated notes |
| Push a `vX.Y.Z-rc.N` tag | `X.Y.Z-rc.N` | Pre-release |

`latest` only ever moves on a release. Every release attaches
`docker-compose.yml` and `env.example` (save it as `.env`). The running build's version is
shown in the dashboard header and at `/version` (`1.2.0`,
`canary-<sha>`, or `dev` for local builds).

To cut a release:

```
git tag v1.2.0 && git push origin v1.2.0
```

Before anything is pushed, CI runs the unit tests (inside the image
build), boots the image, and checks `/healthz`, `/version`, the login
redirect, and that it runs as the unprivileged `lugbulk` user.

The container starts as root only long enough to hand `/data` to
`lugbulk` (so volumes from older, root-run images keep working), then drops
privileges. It has a Docker `HEALTHCHECK` on `/healthz`.

To run a published image:

```
docker run -d --name lugbulk -p 8080:8080 -v lugbulk-data:/data --env-file .env \
    ghcr.io/aronwk-aaron/lugbulk-labels-web:latest    # or :canary, or :1.2.0
```

or with the compose file: `LUGBULK_TAG=canary docker compose pull && docker compose up -d`.

## Project files

| Path | Purpose |
|---|---|
| `src/main.cpp` | Entry point, route definitions, security-header middleware |
| `src/config.{h,cpp}` | Env-var configuration loading |
| `src/crypto.{h,cpp}` | AES-256-GCM refresh-token encryption, SHA-256, base64, CSPRNG tokens |
| `src/db.{h,cpp}` | SQLite access layer (users/sheets/sessions/runs) and schema migrations |
| `src/oauth.{h,cpp}` | Google OAuth token exchange + userinfo + Drive sheet search + Sheets read |
| `src/sheet_layout.{h,cpp}` | Header names, qty marker, label stock inventory loading/lookup |
| `data/label_specs.json` | Avery/Dymo label stock inventory (generated by the CLI's `tools/update_label_specs.py`) |
| `src/sheet_pivot.{h,cpp}` | Sheet rows → one record per (person, part), plus data issues |
| `src/colors.{h,cpp}` | LEGO ↔ BrickLink color name table |
| `src/ordering.{h,cpp}` | Part weight estimates, label order, "N of M" numbering, per-part summaries |
| `src/labels_pdf.{h,cpp}` | Label PDF rendering and the shared part-photo cache |
| `src/image_backdrop.{h,cpp}` | Gray-tile treatment for trans/white part photos |
| `src/bricklink.{h,cpp}` | Reads BrickLink's catalog download files: weights and colors |
| `src/reports.{h,cpp}` | Lot counts and parts list, CSV + PDF |
| `src/zip_writer.{h,cpp}` | Builds the "Download all" `.zip` in memory |
| `src/spreadsheet.{h,cpp}` | Reads uploaded `.xlsx` (bounded unzip + SpreadsheetML) and `.csv` |
| `src/pdf_text.{h,cpp}` | UTF-8 → WinAnsi for PDF text |
| `tests/tests.cpp` | Unit tests (`ctest`) |
| `CMakeLists.txt` | Build config; fetches Crow, locates PoDoFo/SQLite3/CURL/OpenSSL/Asio |
| `sql/schema.sql` | SQLite schema: users, sheets, runs, sessions |
| `templates/dashboard.html` | Mustache template for the logged-in dashboard page (Crow's bundled `crow::mustache`) |
| `Dockerfile` | Multi-stage build (Debian bookworm base); tests run during the build |
| `src/rate_limits.{h,cpp}` | Rate limiter, job gate, sign-in allowlist |
| `cmake/patch_crow.cmake` | Caps Crow's request body size |
| `docker/entrypoint.sh` | Fixes `/data` ownership, then drops to the `lugbulk` user |
| `.github/workflows/docker-publish.yml` | CI: builds, tests, publishes the image to GHCR, and creates canary/versioned GitHub releases |
| `docker-compose.yml` | Local dev convenience — build + run with a persistent volume |
| `.env.example` | Template for OAuth client credentials and the token-encryption key |

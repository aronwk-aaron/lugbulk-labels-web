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
token storage, hashed session tokens), a Drive-backed sheet picker,
generation of label PDFs, a parts list and lot counts from the live sheet,
and a per-sheet "last run" history are implemented.

## What it generates

- **Labels** — one per (person, part), grouped by part: heaviest parts
  first (or lightest / sheet order), smallest quantity first within a
  part, each numbered "3 of 10" within its part. Each label shows the
  part photo, Element ID (bold), quantity, both LEGO and BrickLink color
  names, description and the person's name. Trans, white, and very pale
  parts are shown on a light gray tile so they don't vanish when printed.
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

Set `BRICKLINK_CONSUMER_KEY`, `BRICKLINK_CONSUMER_SECRET`,
`BRICKLINK_TOKEN` and `BRICKLINK_TOKEN_SECRET` (see `.env.example`) to
look each part up on BrickLink: catalog weight for part order, and the
BrickLink color for any the sheet is missing. Get them from BrickLink's
[API registration](https://www.bricklink.com/v2/api/register_consumer.page)
(may require a — possibly closed — store); the access token must be
created for the **server's** public IP. Lookups are cached in the
`bricklink_parts` table (misses retried after a week), so each part costs
two API calls once. Without credentials, or if BrickLink refuses them
(logged to stderr), weights are estimated.

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
explicitly saved). The "Order Here" tab's columns are found by header text (`Element ID` /
`Part Number`, `Description`, `LEGO Color`, `BL Color`, `Weight`), and both
known ways of laying out people are recognized: a `qty` marker row under
the names (the ArkLUG sheet) or (name, running cost) header pairs (2026's
master sheet) — see `src/sheet_pivot.h`.

**Flow:** login → search/pick a sheet from Drive (`GET /sheets/search?q=`)
→ save it (`POST /sheets`) → dashboard listing saved sheets → per sheet,
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
SQLite3, libcurl, libjpeg, PoDoFo 0.9.x, and standalone Asio (`libssl-dev
libsqlite3-dev libcurl4-openssl-dev libjpeg-dev libpodofo-dev libasio-dev`
on Debian/Ubuntu).

Unit tests (sheet pivoting, colors, ordering, reports, image outlining,
PDF rendering for every label size) build alongside the server and run in
the Docker build:

```
ctest --test-dir build --output-on-failure
```

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
`X.Y`, and `X` on a `vX.Y.Z` tag push. Before anything is pushed, CI runs
the unit tests (inside the image build), boots the image, and checks
`/healthz`, the login redirect, and that it runs as the unprivileged
`lugbulk` user.

The container starts as root only long enough to hand `/data` to
`lugbulk` (so volumes from older, root-run images keep working), then drops
privileges. It has a Docker `HEALTHCHECK` on `/healthz`.

To run the published image:

```
docker run -d --name lugbulk -p 8080:8080 -v lugbulk-data:/data --env-file .env \
    ghcr.io/aronwk-aaron/lugbulk-labels-web:latest
```

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
| `src/bricklink.{h,cpp}` | BrickLink API client (OAuth 1.0): weights and colors |
| `src/reports.{h,cpp}` | Lot counts and parts list, CSV + PDF |
| `src/pdf_text.{h,cpp}` | UTF-8 → WinAnsi for PDF text |
| `tests/tests.cpp` | Unit tests (`ctest`) |
| `CMakeLists.txt` | Build config; fetches Crow, locates PoDoFo/SQLite3/CURL/OpenSSL/Asio |
| `sql/schema.sql` | SQLite schema: users, sheets, runs, sessions |
| `templates/dashboard.html` | Mustache template for the logged-in dashboard page (Crow's bundled `crow::mustache`) |
| `Dockerfile` | Multi-stage build (Debian bookworm base); tests run during the build |
| `docker/entrypoint.sh` | Fixes `/data` ownership, then drops to the `lugbulk` user |
| `.github/workflows/docker-publish.yml` | CI: builds and publishes the image to GHCR |
| `docker-compose.yml` | Local dev convenience — build + run with a persistent volume |
| `.env.example` | Template for OAuth client credentials and the token-encryption key |

# lugbulk-labels-web

> **AI disclaimer:** This project was scaffolded with assistance from Claude
> (Anthropic). Review the code before relying on it, especially the OAuth
> flow, token storage, and the in-browser PDF generation logic.

Hosted, multi-user counterpart to
[lugbulk-label](https://github.com/aronwk-aaron/lugbulk-label) (the local
Python CLI). Lets a handful of trusted LUG organizers log in with their own
Google account, point at their own bulk-order sheet, and generate label
PDFs / lot-count reports without installing Python or a service account key
locally.

**Status: working, pre-release.** Google OAuth login (encrypted refresh
token storage, hashed session tokens), a Google Picker sheet chooser,
generation (in the browser) of label PDFs, a parts list and lot counts from
the live sheet, and a per-sheet "last download" history are implemented.

## Two ways in

- **Upload a spreadsheet** — no account needed. Upload the order sheet as
  an Excel `.xlsx` (Google Sheets: File → Download → Microsoft Excel) or a
  `.csv` of its "Order Here" tab, pick a label design, and download labels
  and reports. The file is read in your browser and never sent anywhere.
  Up to 10 MB; an `.xlsx` may not unpack past 64 MB.
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
- **Label design** — per sheet, with a live preview (the first page of
  the sheet's own labels, made in the browser): switch any label part on or off — photo, element ID,
  quantity, LEGO and BrickLink color, description, name, "3 of 10" count,
  gray tile behind clear/white parts, color swatch, QR code linking to the
  part on BrickLink — and pick the stock and part order. Designs are saved
  per Google Sheet, so everyone with that sheet prints the same labels;
  Labels, Checklist and Parts list downloads follow it. An **alignment
  test page** prints the stock's outlines on plain paper to check the
  printer first.
- **Keep each part on one sheet** (stocks with more than one label per
  sheet) — *Off* (default) lets the labels run on continuously, so a part
  can run over onto the next sheet; *Optimize* packs whole parts onto
  sheets so none is split, using the fewest sheets possible (this is bin
  packing, solved exactly in the Web Worker within about a second; if that
  isn't enough the best packing found is used and marked "not proven
  optimal"). Blank labels are left empty; a part bigger than one sheet
  fills whole sheets and only its remainder is packed. Among the packings
  with the fewest sheets it keeps closest to the part order (sheets in
  order of their first part, parts in order within a sheet). The design
  shows sheets, blank labels and split parts for both; the packed order is
  also what "Same as the labels" means in the checklist and parts list.
  Saved with the design (`keep_parts`: `off` | `optimize`).
- **Download** — tick the files you want: labels, packing checklist, parts
  list and lot counts (PDF and CSV) and the sheet check. One file downloads
  as it is; several come in one `.zip`. Everything — labels, reports and
  the zip — is made in the browser (the server no longer makes any PDF or
  zip): an uploaded file never leaves it, and for a saved Google Sheet the
  server only passes on the sheet's cells (`/sheets/:id/values`), BrickLink
  data (`/bricklink/lookup`) and part photos (`/img/<id>.jpg`). Label PDFs are built in a Web Worker
  (`static/js/labels_worker.js`) with progress ("Fetching photos 120/800",
  "Page 3 of 40"); a saved sheet's download is logged
  (`POST /sheets/:id/runs`, the kind and a count only) for its "Last
  download" line.
- **Report tabs** — Labels · Packing checklist · Parts list · Lot counts,
  each with its own settings and a live preview made in the browser. Every
  report can have its own title and subtitle line, US Letter or A4,
  portrait or landscape. Report settings and the zip's file choices are
  saved with the sheet, like the label design (for uploads: in the
  browser).
- **Packing checklist** (PDF) — one page per person (or one after another)
  listing their parts with a tick box per bag; sort people by first or last
  name, choose the part order, show colors, weight and part photos, and add
  a "Packed by / date" line.
- **Label stock** — pick from 51 Avery (US-Letter and A4) and Dymo
  LabelWriter stocks, searchable by any part number on the box (e.g.
  8162, L7163, 30857); default Avery 5162. The inventory is
  `data/label_specs.json`, generated from the gLabels template database by
  the CLI's `tools/update_label_specs.py`.
- **Parts list** (PDF/CSV) — one row per part in label order (or
  heaviest/lightest first, sheet order, element ID): total pieces, number
  of people ordering it, weight; optional photo and total weight columns,
  grouped by color if you like.
- **Lot counts** (PDF/CSV) — per person: number of labels and pieces;
  optional total weight, a totals row and a minimum number of lots.
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
- **[pdf-lib](https://pdf-lib.js.org)** and **[Nayuki's QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)** — PDFs and QR codes, in the browser (vendored, pinned, in `static/js/vendor/`). The server itself makes no PDFs.
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
`GET /auth/picker-token`) → save the picked sheet (`POST /sheets`) →
dashboard listing saved sheets → per sheet, the browser reads the live
sheet's cells (`GET /sheets/:id/values`), pivots it, asks for BrickLink
data (`POST /bricklink/lookup`) and part photos (`GET /img/<id>.jpg`), and
makes the labels, reports and zip itself. The label design is saved with
the sheet (`GET`/`PUT /sheets/:id/design`). Nothing generated is stored;
`runs` (see `sql/schema.sql`) is a history log only — timestamp, report
type, item count, status (`POST /sheets/:id/runs`) — not a file store.

**Access tokens are never persisted.** Only the (encrypted) OAuth
*refresh* token is stored; a short-lived access token is minted from it
in-memory on each request that needs to call Google, used immediately, and
discarded.

**Data model:** see [`sql/schema.sql`](sql/schema.sql) — `users`,
`sheets`, `runs`, `sessions`.

The sheet pivot, ordering, color table and label layout mirror the CLI's
`pivot.py`, `ordering.py`, `colors.py` and `render_labels.py`; keep the
two in step when changing either. In this repo the JavaScript in
`static/js/` is the reference: the C++ pivot/ordering/colors/spreadsheet
code that remains is checked against it by the parity tests (see Building
without Docker).

**Not carried over from the CLI:** the full `--manifest` summary report,
`--per-person` PDFs, `--sort-by` toggle (last-name order only), and
per-event color/weight overrides (fix the sheet instead).

## Security and abuse limits

Reading a sheet costs a Google Sheets call, and a big sheet asks for
hundreds of part photos (LEGO's CDN) and BrickLink lookups, so the server
limits what any one person — or script — can make it do:

| What | Limit |
|---|---|
| Who can sign in | `ALLOWED_EMAILS` (addresses and/or `@domain`s); verified Google email required. Removing someone locks them out immediately. **Set this** — unset lets any Google account in and logs a warning at startup. |
| Reading a saved sheet | 6 at once, then one per 2 minutes, per user; one running per user; `MAX_CONCURRENT_JOBS` (default 2) server-wide. Over the limit gets an immediate "try again" (HTTP 429), never a queue. |
| Uploads | Never reach the server: the file is read in the browser |
| Drive search | 10 at once, then one per 3 seconds, per user |
| Any request | 120 at once, then 10/second, per client IP (sign-in routes: 10, then one per 6 seconds) |
| Request body | 64 KB (Crow patched at build time — `cmake/patch_crow.cmake`); bigger requests are dropped |
| Sheet size | 3,000 rows read; 20,000 labels / 2,000 parts per run; 32 MB Google response |
| Saved sheets | 50 per user (only sheets your Google account can open); 10 sessions per user |
| BrickLink lookups / design saves | 20, then 1/second, per visitor / user |
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
- No response with anyone's data is cached anywhere (`Cache-Control:
  no-store, private`), so a shared computer's Back button or a caching proxy
  can't show one organizer's data to another. (Only public, identical-for-
  everyone responses are cacheable: LEGO part photos under `/img/` and the
  label stock list `/label-specs.json`.) Logging out also clears the browser cache
  for the site, and sign-ins last 14 days.
- Nothing personal is logged: no emails, names, tokens or request URLs
  (Crow's access log is off); errors log only internal user/sheet ids.

**Hardening:** a strict Content-Security-Policy (the dashboard's inline
scripts run only with a per-response nonce, plus the app's own
`/static/js/` modules; no other script can), `nosniff`,
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
SQLite3, libcurl, zlib and standalone Asio (`libssl-dev libsqlite3-dev
libcurl4-openssl-dev zlib1g-dev libasio-dev` on Debian/Ubuntu).

Unit tests (sheet pivoting, colors, ordering, the photo cache, design
storage, rate limits...) build alongside the server and run in the Docker
build:

```
ctest --test-dir build --output-on-failure
```

The browser code has its own tests (Node 22+, no install):

```
node --test tests/js/                       # unit tests + the checked-in fixtures
./build/lugbulk_golden golden && GOLDEN_DIR=golden node --test tests/js/   # + parity with the C++
```

`tests/js/golden/` holds fixtures dumped once from the C++ before its PDF,
CSV and label-layout code was retired (`labels.json`, `reports.json`); the
JavaScript is their reference now. `tests/js/golden/regenerate-labels.mjs`
rewrites the label layouts from it after an intended change to the layout
or the font widths (`static/js/afm.js`).

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
| `src/label_options.{h,cpp}` | Which label parts a saved design switches on or off (validates `PUT /sheets/:id/design`) |
| `src/part_images.{h,cpp}` | The shared part-photo cache behind `/img/<id>.jpg` |
| `static/js/packing.js` | "Keep each part on one sheet": exact bin packing of parts onto sheets (bounds, bin completion over size counts, closest-to-order tie-break, time box); run in the labels worker; `tests/js/packing.test.mjs` |
| `static/js/labels.js`, `backdrop.js`, `labels_worker.js`, `afm.js` | Label PDFs in the browser (pdf-lib, Nayuki's qrcodegen), the gray tile behind light parts, the Web Worker that runs them, and the Helvetica widths (Adobe AFM, by WinAnsi code) labels and reports measure text with; checked by `tests/js/labels.test.mjs`, `afm.test.mjs` |
| `src/bricklink.{h,cpp}` | Reads BrickLink's catalog download files: weights and colors |
| `src/json_check.{h,cpp}` | Strict JSON check for the report options saved with a sheet's design |
| `src/spreadsheet.{h,cpp}` | Reads `.xlsx` (bounded unzip + SpreadsheetML) and `.csv` — no longer used by a route; kept as the reference `static/js/spreadsheet.js` is tested against |
| `src/records.{h,cpp}` | Row cap, per-run size limits, BrickLink data on records, the "Check sheet" JSON |
| `static/js/` | Browser ports of the pure data logic (plain ES modules, served at `/static/js/<name>.js`): `pivot.js`, `ordering.js`, `colors.js`, `layout.js`, `records.js`, `spreadsheet.js`, `load.js`, and the reports (`reports.js`, `report_options.js`, `printf.js`) and zip writer (`zip.js`) |
| `static/js/vendor/` | Third-party browser modules, pinned and unmodified: `pdf-lib.js` (see its README) |
| `tests/tests.cpp` | Unit tests (`ctest`) |
| `tests/golden.cpp` | `lugbulk_golden <dir>`: writes what the C++ makes of each fixture, for the JS parity test |
| `tests/js/` | `node --test` tests for `static/js/`; `GOLDEN_DIR=<dir>` checks it matches the C++ (CI does this); `tests/js/golden/` holds the checked-in fixtures |
| `tests/fixtures/` | Invented order sheets (`.xlsx`, `.csv`) used by the tests |
| `CMakeLists.txt` | Build config; fetches Crow, locates SQLite3/CURL/OpenSSL/zlib/Asio |
| `sql/schema.sql` | SQLite schema: users, sheets, runs, sessions |
| `templates/dashboard.html` | Mustache template for the logged-in dashboard page (Crow's bundled `crow::mustache`) |
| `Dockerfile` | Multi-stage build (Debian bookworm base); tests run during the build |
| `src/rate_limits.{h,cpp}` | Rate limiter, job gate, sign-in allowlist |
| `cmake/patch_crow.cmake` | Caps Crow's request body size |
| `docker/entrypoint.sh` | Fixes `/data` ownership, then drops to the `lugbulk` user |
| `.github/workflows/docker-publish.yml` | CI: builds, tests, publishes the image to GHCR, and creates canary/versioned GitHub releases |
| `docker-compose.yml` | Local dev convenience — build + run with a persistent volume |
| `.env.example` | Template for OAuth client credentials and the token-encryption key |

## License

Copyright (C) 2026 Aaron Kimbrell.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, version 3 or (at your option) any later version. It is
distributed WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for the full terms.

Because it's the AGPL, anyone running a modified copy as a web service must
offer its users that version's source code — the dashboard and the terms page
link to it.

Bundled or fetched components keep their own licenses: Crow (BSD-3-Clause),
pdf-lib (MIT), Nayuki's QR Code generator (MIT), and the gLabels label
template database behind `data/label_specs.json` (MIT). The Helvetica widths
in `static/js/afm.js` are from Adobe's Core 14 AFM files.

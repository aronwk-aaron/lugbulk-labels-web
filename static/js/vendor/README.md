# Third-party browser modules

Served at `/static/js/vendor/<name>.js` (see `load_static_js` in
`src/main.cpp`; only flat `[a-z0-9_-]+.js` names are served). Each one is an
upstream file (unmodified, or built from upstream source as noted), pinned
here by version and SHA-256. To update one,
replace the file with the new upstream build, then update the version and
hash below.

## pdf-lib.js

- **What:** [pdf-lib](https://github.com/Hopding/pdf-lib) — makes the packing
  checklist, parts list and lot counts PDFs in the browser
  (`static/js/reports.js`).
- **Version:** 1.17.1
- **Upstream file:** `dist/pdf-lib.esm.min.js` from the npm package
  `pdf-lib@1.17.1` (`https://registry.npmjs.org/pdf-lib/-/pdf-lib-1.17.1.tgz`,
  integrity `sha512-V/mpyJAoTsN4cnP31vc0wfNA1+p20evqqnap0KLoRUN0Yk/p3wN52DOEsL4oBFcLdb76hlpKPtzJIgo67j/XLw==`),
  renamed to `pdf-lib.js`.
- **SHA-256:** `72c052d97b4d5d9fa6cdbdcb7ad709f03d4ddb1122390cb3afeba4d88651d969`
- **Size:** 523,417 bytes (about 206 KB gzipped).
- **License:** MIT, copyright (c) 2019 Andrew Dillon — see
  `pdf-lib.LICENSE.md`. The bundle also includes tslib (Apache-2.0; its notice is
  kept in the file), pako (MIT and zlib licenses) and
  `@pdf-lib/standard-fonts` and `@pdf-lib/upng` (MIT).

The file ends with a `sourceMappingURL` comment for a map that isn't
shipped; browsers only ask for it with developer tools open (a harmless 404).

Check a copy with:

```sh
sha256sum static/js/vendor/pdf-lib.js
```

## qrcodegen.js

- **What:** [QR Code generator library](https://github.com/nayuki/QR-Code-generator)
  by Project Nayuki, TypeScript/JavaScript port — the QR code to BrickLink on
  labels (`static/js/labels.js`). The same library, at the same commit, as
  the server's C++ build (`CMakeLists.txt`), so the codes match module for
  module (`tests/js/labels.test.mjs` checks against the C++'s matrices).
- **Version:** v1.8.0, commit `720f62bddb7226106071d4728c292cb1df519ceb`.
- **Upstream file:** `typescript-javascript/qrcodegen.ts` at that commit
  (SHA-256 `c4749095a91bf9696e3a303998b9905e467094f53041e64393e65e6d887737fd`),
  compiled with `npx -p typescript@5.6.3 tsc --target es2020 qrcodegen.ts`
  (upstream ships TypeScript only). The one change: an `export default
  qrcodegen;` line appended, to make it an ES module.
- **SHA-256:** `0cc3d38d2c3b2f083bec80e61d27270ea11328ecc9130b0c4cb829aa70f6d8d4`
- **License:** MIT, copyright (c) Project Nayuki — the notice is kept at the
  top of the file.

## datatables.js

- **What:** [DataTables](https://datatables.net/) — sorts and filters the
  Compare years tables in the browser (`static/js/years_page.js`). Version 3
  needs no jQuery.
- **Version:** 3.1.3
- **Upstream file:** `js/dataTables.min.mjs` from the npm package
  `datatables.net@3.1.3` (`https://registry.npmjs.org/datatables.net/-/datatables.net-3.1.3.tgz`,
  integrity `sha512-B34/A+KAkKQnl6si+SgmB8grhDepfHDXEFyQxJanqY70dB4ab0pHTF1aHZvLKKruuUoL815/y3YeTSYUr/pAOw==`),
  renamed to `datatables.js`. Its stylesheet isn't used: the dashboard
  styles the few classes it needs (sort arrows) in its own `<style>`.
- **SHA-256:** `46db53b49b215c6c12a1997718ab40a0c2a31af2256a18cdb24d27ad43250989`
- **Size:** 137,402 bytes (about 44 KB gzipped).
- **License:** MIT, copyright SpryMedia Limited and other contributors —
  see `datatables.LICENSE.txt`.

```sh
sha256sum static/js/vendor/datatables.js
```

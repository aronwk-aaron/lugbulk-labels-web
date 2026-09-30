# Third-party browser modules

Served at `/static/js/vendor/<name>.js` (see `load_static_js` in
`src/main.cpp`; only flat `[a-z0-9_-]+.js` names are served). Each one is an
unmodified upstream file, pinned here by version and SHA-256. To update one,
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

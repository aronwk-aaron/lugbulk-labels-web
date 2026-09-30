// Rewrites the "layout" of every case in labels.json from static/js/labels.js.
//
// labels.json was first dumped from the C++ server (before it was retired);
// the JavaScript is the reference now. Run this only when the layout or the
// font metrics change on purpose, then read `git diff` — only labels whose
// text changes width should differ:
//
//   node tests/js/golden/regenerate-labels.mjs
//
// The QR matrices and backdrop pixels in the file are left as they are, and so
// is any case whose layout moved by no more than 0.01 pt (the tests' own
// tolerance), so float noise doesn't churn the file.

import { readFileSync, writeFileSync } from 'node:fs';

import * as labels from '../../../static/js/labels.js';

const file = new URL('./labels.json', import.meta.url);
const g = JSON.parse(readFileSync(file, 'utf8'));

const TOL = 0.01;
const same = (a, b) => {
  if (a === null || b === null || typeof a !== 'object') return a === b || (typeof a === 'number' && typeof b === 'number' && Math.abs(a - b) <= TOL);
  const keys = Object.keys(a);
  return keys.length === Object.keys(b).length && keys.every((k) => k in b && same(a[k], b[k]));
};
const sameLayout = (a, b) => ['image', 'qr', 'swatch'].every((k) => same(a[k], b[k])) &&
  a.texts.length === b.texts.length && a.texts.every((t, i) => same(t, b.texts[i]));

let changed = 0;
for (const c of g.cases) {
  const lay = labels.layoutLabel(g.records[c.record], c.width, c.height, labels.optionsFromHidden(c.hidden), labels.helveticaMeasure);
  const next = {
    image: lay.image ?? null,
    qr: lay.qr ? { x: lay.qr.x, y: lay.qr.y, size: lay.qr.size } : null,
    swatch: lay.swatch ?? null,
    texts: lay.texts.map(({ font, size, x, y, text }) => ({ font, size, x, y, text })),
  };
  if (!sameLayout(c.layout, next)) {
    c.layout = next;
    ++changed;
  }
}

writeFileSync(file, JSON.stringify(g) + '\n');
console.log(`${changed} of ${g.cases.length} layouts changed`);

// Label PDFs in the browser (static/js/labels.js, backdrop.js):
//  - parity with the C++ server: the label layout (every text run, image,
//    QR and swatch box, to within 0.01 pt), the QR matrices and the image
//    backdrop pixels, from tests/golden.cpp's labels.json (GOLDEN_DIR);
//  - the PDFs themselves: they load, have the right page count, contain
//    all the text, and never cut long text off.
// All records are invented.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { PDFDocument, PDFRawStream, decodePDFRawStream } from '../../static/js/vendor/pdf-lib.js';
import { backdropPixels } from '../../static/js/backdrop.js';
import * as labels from '../../static/js/labels.js';

const dir = process.env.GOLDEN_DIR;
const skip = dir ? false : 'GOLDEN_DIR not set (see tests/golden.cpp)';
const specsDoc = JSON.parse(readFileSync(new URL('../../data/label_specs.json', import.meta.url), 'utf8'));
const specById = (id) => specsDoc.specs.find((s) => s.id === id);


const near = (a, b, what) => assert.ok(Math.abs(a - b) <= 0.01, `${what}: ${a} vs ${b}`);
function nearBox(a, b, what, keys) {
  if (b === null) return assert.equal(a, null, what);
  assert.ok(a, `${what}: missing`);
  for (const k of keys) near(a[k], b[k], `${what}.${k}`);
}

test('label layout matches C++ (labels.json)', { skip }, async () => {
  const g = JSON.parse(readFileSync(join(dir, 'labels.json'), 'utf8'));
  const measure = labels.helveticaMeasure;
  assert.ok(g.cases.length >= 100);
  for (const c of g.cases) {
    const what = `${c.spec} hide=${c.hidden} record ${c.record}`;
    const spec = specById(c.spec);
    near(spec.label_width_mm * labels.MM_TO_PT, c.width, `${what} width`);
    const got = labels.layoutLabel(g.records[c.record], c.width, c.height, labels.optionsFromHidden(c.hidden), measure);
    const want = c.layout;
    nearBox(got.image, want.image, `${what} image`, ['x', 'y', 'size']);
    nearBox(got.qr, want.qr, `${what} qr`, ['x', 'y', 'size']);
    nearBox(got.swatch, want.swatch, `${what} swatch`, ['x', 'y', 'side']);
    if (want.swatch) {
      assert.equal(got.swatch.trans, want.swatch.trans, `${what} swatch.trans`);
      want.swatch.rgb.forEach((v, i) => near(got.swatch.rgb[i], v, `${what} swatch.rgb`));
    }
    assert.deepEqual(got.texts.map((t) => [t.font, t.text]), want.texts.map((t) => [t.font, t.text]), what);
    got.texts.forEach((t, i) => {
      for (const k of ['size', 'x', 'y']) near(t[k], want.texts[i][k], `${what} text ${i} ${k}`);
    });
  }
});

test('QR matrices match C++', { skip }, () => {
  const g = JSON.parse(readFileSync(join(dir, 'labels.json'), 'utf8'));
  for (const q of g.qr) {
    const got = labels.qrMatrix(q.text).map((row) => row.map((b) => (b ? '1' : '0')).join(''));
    assert.deepEqual(got, q.rows, q.text);
  }
});

test('image backdrop pixels match C++', { skip }, () => {
  const g = JSON.parse(readFileSync(join(dir, 'labels.json'), 'utf8'));
  for (const [i, c] of g.backdrop.entries()) {
    const out = backdropPixels(Buffer.from(c.pixels, 'hex'), c.width, c.height, c.trans, c.light);
    if (c.out === null) {
      assert.equal(out, null, `case ${i}`);
      continue;
    }
    assert.ok(out, `case ${i}: expected a tile`);
    assert.equal(out.width, c.out.width);
    assert.equal(out.height, c.out.height);
    const want = Buffer.from(c.out.pixels, 'hex');
    // Exact, but allow ±1 where exp()/hypot() round differently.
    let off = 0;
    for (let j = 0; j < want.length; ++j) {
      const d = Math.abs(out.pixels[j] - want[j]);
      assert.ok(d <= 1, `case ${i} byte ${j}: ${out.pixels[j]} vs ${want[j]}`);
      off += d;
    }
    assert.ok(off <= want.length / 100, `case ${i}: ${off} bytes off by one`);
  }
});

// ---- the PDFs ---------------------------------------------------------------

const RECORDS = [
  { person: 'Alex Example', element_id: '6225242', description: 'BRICK 1X1X1 2/3 W/2 KNOBS', lego_color: 'Medium Stone Grey', bl_color: 'Light Bluish Gray', qty: '150', part_seq: 3, part_total: 10 },
  { person: 'Robin Invented', element_id: '300121', description: 'BRICK 2X2', lego_color: 'Bright Red', bl_color: 'Red', qty: '2000', part_seq: 1, part_total: 1 },
  { person: 'Sam Placeholder With An Unusually Long Invented Name', element_id: '4211395', description: 'PLATE 1X2 ' + 'WITH A VERY LONG MADE UP DESCRIPTION '.repeat(4), lego_color: 'Transparent', bl_color: 'Trans-Clear', qty: '25', part_seq: 2, part_total: 4 },
];

// All text drawn in a PDF, one run per line (pdf-lib writes each run as a
// WinAnsi hex string shown with Tj, in Flate-compressed content streams).
async function pdfText(bytes) {
  const doc = await PDFDocument.load(bytes);
  let text = '';
  for (const [, obj] of doc.context.enumerateIndirectObjects()) {
    if (!(obj instanceof PDFRawStream)) continue;
    let raw;
    try {
      raw = Buffer.from(decodePDFRawStream(obj).decode()).toString('latin1');
    } catch {
      continue;
    }
    for (const m of raw.matchAll(/<([0-9A-Fa-f]*)> Tj/g)) text += Buffer.from(m[1], 'hex').toString('latin1') + '\n';
  }
  return text;
}

test('labels PDF: pages, all text, no truncation', async () => {
  const spec = specById('avery5162'); // 14 per sheet
  const many = Array.from({ length: 30 }, (_, i) => ({ ...RECORDS[i % 3], part_seq: i + 1, part_total: 30 }));
  const bytes = await labels.buildLabelsPdf(many, spec, { show: labels.optionsFromHidden('photo') });
  const doc = await PDFDocument.load(bytes);
  assert.equal(doc.getPageCount(), 3);
  const all = (await pdfText(bytes)).replace(/\n/g, ' ').replace(/\s+/g, ' ');
  for (const r of RECORDS) {
    for (const s of [r.element_id, `Qty: ${r.qty}`, `LEGO: ${r.lego_color}`, `BL: ${r.bl_color}`]) {
      assert.ok(all.includes(s), `missing ${s}`);
    }
    // Wrapped text: every word is there, in order, nothing cut.
    for (const long of [r.description.trim(), r.person]) {
      assert.ok(all.includes(long.replace(/\s+/g, ' ')), `cut off: ${long}`);
    }
  }
  assert.ok(all.includes('30 of 30'));
});

test('labels PDF: previews stop at maxPages; rolls get one label a page', async () => {
  const many = Array.from({ length: 50 }, (_, i) => ({ ...RECORDS[i % 3] }));
  const sheet = await labels.buildLabelsPdf(many, specById('avery5160'), { maxPages: 1 });
  assert.equal((await PDFDocument.load(sheet)).getPageCount(), 1);
  const roll = await labels.buildLabelsPdf(many, specById('dymo30252'), { maxPages: 3 });
  assert.equal((await PDFDocument.load(roll)).getPageCount(), 3);
  const empty = await labels.buildLabelsPdf([], specById('avery5162'));
  assert.equal((await PDFDocument.load(empty)).getPageCount(), 1);
});

test('no truncation: a huge name and description still fit, whole', async () => {
  const measure = labels.helveticaMeasure;
  const spec = specById('dymo30252');
  const w = spec.label_width_mm * labels.MM_TO_PT;
  const h = spec.label_height_mm * labels.MM_TO_PT;
  const word = 'Invented'.repeat(12);
  const r = { ...RECORDS[0], person: `${word} ${word}`, description: `${word} `.repeat(6).trim(), qty: '1234567890' };
  const lay = labels.layoutLabel(r, w, h, labels.optionsFromHidden(''), measure);
  const joined = lay.texts.map((t) => t.text).join('');
  assert.ok(joined.replace(/ /g, '').includes(r.description.replace(/ /g, '')));
  assert.ok(joined.replace(/ /g, '').includes(r.person.replace(/ /g, '')));
  for (const t of lay.texts) {
    const tw = measure(t.font, t.text, t.size);
    assert.ok(t.x >= -0.01 && t.x + tw <= w + 0.01, `off the label: ${t.text}`);
    assert.ok(t.y >= 0 && t.y <= h, `off the label vertically: ${t.text}`);
  }
});

test('photos: embedded when given; missing or broken ones are skipped', async () => {
  const spec = specById('avery5162');
  const jpeg = readFileSync(new URL('../fixtures/tiny.jpg', import.meta.url));
  const images = new Map([['6225242', new Uint8Array(jpeg)], ['300121', new Uint8Array([1, 2, 3])]]);
  const bytes = await labels.buildLabelsPdf(RECORDS, spec, { images, show: labels.optionsFromHidden('backdrop') });
  const doc = await PDFDocument.load(bytes);
  assert.equal(doc.getPageCount(), 1);
  assert.ok(Buffer.from(bytes).toString('latin1').includes('/DCTDecode'));
});

test('alignment test page: one page, every slot numbered', async () => {
  const spec = specById('avery5160');
  const bytes = await labels.buildTestPage(spec);
  assert.equal((await PDFDocument.load(bytes)).getPageCount(), 1);
  const text = await pdfText(bytes);
  assert.ok(text.includes(`${spec.brand} ${spec.part} #1 `));
  assert.ok(text.includes(`#${spec.columns * spec.rows} `));
});

test('options: hidden parts', () => {
  const o = labels.optionsFromHidden('qr, photo');
  assert.equal(o.qr, false);
  assert.equal(o.photo, false);
  assert.equal(o.name, true);
  assert.throws(() => labels.optionsFromHidden('nope'));
  assert.equal(labels.bricklinkUrl('6225242'), 'https://www.bricklink.com/v2/search.page?q=6225242');
});

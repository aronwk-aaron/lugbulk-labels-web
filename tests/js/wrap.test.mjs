// The browser report PDFs never cut text off: long cells wrap, rows grow
// to fit and pages break between rows of any height. Every name and
// quantity here is invented.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PDFDocument, PDFArray, decodePDFRawStream } from '../../static/js/vendor/pdf-lib.js';
import { pivotSheet } from '../../static/js/pivot.js';
import * as reports from '../../static/js/reports.js';

const MARGIN = 15 * (72 / 25.4);

// Every drawing operation on each page: text {text, x, y} and filled
// rectangles {x, y, w, h} (the header, zebra and group bands).
async function pdfOps(bytes) {
  const doc = await PDFDocument.load(bytes);
  const decoder = new TextDecoder('windows-1252');
  return doc.getPages().map((page) => {
    const contents = page.node.Contents();
    const streams = contents instanceof PDFArray ? contents.asArray() : [contents];
    let ops = '';
    for (const ref of streams) {
      ops += new TextDecoder('latin1').decode(decodePDFRawStream(doc.context.lookup(ref)).decode());
    }
    const texts = [];
    const fills = [];
    for (const chunk of ops.split(/\nQ\n/)) {
      const t = chunk.match(/1 0 0 1 ([-\d.]+) ([-\d.]+) Tm\n<([0-9A-Fa-f]*)> Tj/);
      if (t) texts.push({ x: +t[1], y: +t[2], text: decoder.decode(Buffer.from(t[3], 'hex')) });
      const f = chunk.match(/1 0 0 1 ([-\d.]+) ([-\d.]+) cm[\s\S]*?0 0 m\n0 ([-\d.]+) l\n([-\d.]+) [-\d.]+ l[\s\S]*\nf$/);
      if (f && !/ RG\n/.test(chunk)) fills.push({ x: +f[1], y: +f[2], h: +f[3], w: +f[4] });
    }
    return { texts, fills, size: page.getSize() };
  });
}

const squash = (s) => s.replace(/\s+/g, '');

// All of `input` is in the PDF's text, in order, across line breaks.
function assertAllThere(pages, input) {
  const all = squash(pages.flatMap((p) => p.texts.map((t) => t.text)).join(''));
  assert.ok(all.includes(squash(input)), `missing from the PDF: ${input}`);
}

// Nothing is drawn in the margins, bands never overlap, and text in one
// column never collides.
function assertNoOverlap(pages) {
  for (const { texts, fills, size } of pages) {
    const bands = fills.map((f) => [f.y, f.y + f.h]).sort((a, b) => b[0] - a[0]);
    for (let i = 0; i < bands.length; i++) {
      assert.ok(bands[i][0] >= MARGIN - 1e-6, `band below the margin at ${bands[i][0]}`);
      assert.ok(bands[i][1] <= size.height - MARGIN + 1e-6, 'band above the top margin');
      if (i) assert.ok(bands[i][1] <= bands[i - 1][0] + 1e-6, 'bands overlap');
    }
    const byX = new Map();
    for (const t of texts) {
      assert.ok(t.y >= MARGIN - 1e-6, `text below the margin: ${t.text}`);
      if (!byX.has(t.x)) byX.set(t.x, []);
      byX.get(t.x).push(t.y);
    }
    for (const ys of byX.values()) {
      ys.sort((a, b) => b - a);
      for (let i = 1; i < ys.length; i++) assert.ok(ys[i - 1] - ys[i] >= 9 - 1e-6, 'lines collide');
    }
  }
}

const header = ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Weight (g)', 'Total',
  'Wendy Quill', '', 'Otto Brandt', ''];
const marker = ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$'];
const LONG_DESC =
  'BRICK 1X2 W/ BOW 1/2 AND CROSS AXLE HOLE, TRANSPARENT FLUORESCENT REDDISH ORANGE, ' +
  'WITH PRINTED STRIPES ON BOTH SIDES 012';
const LONG_WORD = `TILE ${'X'.repeat(90)}`;

function longRecords() {
  const rows = [header, marker];
  rows.push(['1', '6284070', '', LONG_DESC, 'Trans-Neon Orange', '', '', '2', '', '3', '']);
  rows.push(['2', '6225242', '', 'BRICK 1X2X5', 'Light Bluish Gray', '12.5', '', '5', '', '', '']);
  rows.push(['3', '300126', '', LONG_WORD, 'Black', '', '', '', '', '7', '']);
  for (let i = 0; i < 70; i++) {
    const desc = i % 3 ? `PLATE 1X${(i % 6) + 1}` : `${LONG_DESC} ${i}`;
    rows.push([String(i + 4), String(300200 + i), '', desc, 'White', '', '', '1', '', '2', '']);
  }
  return pivotSheet(rows).records;
}

test('wrapText: words, then characters, nothing dropped', () => {
  const w = (t) => [...t].length;
  assert.equal(LONG_DESC.length, 120);
  const lines = reports.wrapText(LONG_DESC, 30, w);
  assert.ok(lines.length >= 4 && lines.every((l) => l.length <= 30));
  assert.equal(squash(lines.join('')), squash(LONG_DESC));
  const word = 'W'.repeat(70);
  const split = reports.wrapText(`Ann ${word} end`, 20, w);
  assert.ok(split.every((l) => l.length <= 20));
  assert.equal(squash(split.join('')), `Ann${word}end`);
  assert.deepEqual(reports.wrapText('', 10, w), ['']);
  assert.deepEqual(reports.wrapText('PLATE 1X2', 10, w), ['PLATE 1X2']);
  assert.equal(reports.rowHeightFor(1), 16);
  assert.equal(reports.rowHeightFor(3), 38);
});

test('long descriptions, colors and names appear in full in every report', async () => {
  const records = longRecords();
  const neon = records.find((r) => r.element_id === '6284070');
  const grey = records.find((r) => r.element_id === '6225242');
  const neonColors = `${neon.lego_color} / ${neon.bl_color}`;
  const greyColors = `${grey.lego_color} / ${grey.bl_color}`;
  assert.equal(greyColors, 'Medium Stone Grey / Light Bluish Gray');
  assert.equal(neonColors, 'Transparent Fluorescent Reddish Orange / Trans-Neon Orange');
  const longTitle = 'Brick Club Fall Pickup '.repeat(3).trim(); // 68 of the 80 allowed
  const longSub = 'Pickup at the hall '.repeat(6).trim();
  const variants = [
    ['checklist', undefined],
    ['checklist', { checklist: { layout: 'continuous', packed_by: true, weight: true, photo: true, title: longTitle, subtitle: longSub } }],
    ['parts', undefined],
    ['parts', { parts: { group_by_color: true, total_weight: true, photo: true, title: longTitle, subtitle: 'A subtitle long enough to wrap onto a second line of the page heading, for certain, yes indeed' } }],
    ['parts', { parts: { paper: 'a4', orientation: 'landscape' } }],
  ];
  for (const [kind, o] of variants) {
    const pages = await pdfOps(await reports.reportPdf(kind, records, o, { labelOrder: 'heaviest' }));
    for (const text of [LONG_DESC, neonColors, greyColors, LONG_WORD]) assertAllThere(pages, text);
    if (o?.[kind]?.title) assertAllThere(pages, longTitle);
    if (o?.[kind]?.subtitle === longSub) assertAllThere(pages, longSub);
    assert.ok(!pages.some((p) => p.texts.some((t) => t.text.endsWith('...'))));
    assertNoOverlap(pages);
  }
  // A group heading wider than the table wraps too.
  const grouped = await pdfOps(await reports.partsPdf(records, { parts: { group_by_color: true } }));
  assertAllThere(grouped, `${neonColors} (1 part)`);

  const name = 'Maximilian Bartholomew Quillfeather-Montgomery-Ashworth the Third of Brickshire Upon Stud';
  const people = await pdfOps(
    await reports.lotCountsPdf(
      pivotSheet([
        ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Weight (g)', 'Total', name, ''],
        ['', '', '', '', '', '', '', 'qty', '$$'],
        ['1', '3001', '', 'BRICK 2X4', 'Red', '', '', '2', ''],
      ]).records,
      { lots: { totals: true, total_weight: true } },
    ),
  );
  assertAllThere(people, name);
  assertNoOverlap(people);
});

test('rows of different heights paginate without overlap', async () => {
  const records = longRecords();
  const cont = await pdfOps(await reports.checklistPdf(records, { checklist: { layout: 'continuous' } }));
  assert.ok(cont.length >= 2);
  assertNoOverlap(cont);
  for (const o of [undefined, { checklist: { photo: true } }]) {
    const pages = await pdfOps(await reports.checklistPdf(records, o));
    assertNoOverlap(pages);
    const heights = pages.flatMap((p) => p.fills.filter((f) => f.w > 400).map((f) => f.h));
    assert.ok(heights.some((h) => h > (o ? 30 : 16)), 'some rows grew to fit');
  }
  // Longer rows need more pages than one-line rows would.
  const parts = await pdfOps(await reports.partsPdf(records));
  assertNoOverlap(parts);
  assert.ok(parts.length >= 3);
});

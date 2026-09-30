// Tests for the browser reports (static/js/reports.js), their options
// (report_options.js), the printf helpers (printf.js) and the zip writer
// (zip.js). CSV parity with the C++ is in parity.test.mjs. Every name and
// quantity here is invented.

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { PDFDocument, PDFArray, decodePDFRawStream } from '../../static/js/vendor/pdf-lib.js';
import { pivotSheet } from '../../static/js/pivot.js';
import { formatF, formatG } from '../../static/js/printf.js';
import * as opts from '../../static/js/report_options.js';
import * as reports from '../../static/js/reports.js';
import { crc32, dosDateTime, zip } from '../../static/js/zip.js';

const header = ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Weight (g)', 'Total',
  'Wendy Quill', '', 'Otto Brandt', '', '=Evil Formula', '', 'Zoë "Zed" Nakamura', ''];
const marker = ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$', 'qty', '$$', 'qty', '$$'];
const sheet = pivotSheet([
  header,
  marker,
  ['1', '3001', '', 'BRICK 2X4', 'Red', '', '', '10', '', '4', '', '', '', '2', ''],
  ['2', '3020', '', 'PLATE 2X4, EXTRA', 'Blue', '', '', '', '', '6', '', '1', '', '', ''],
  ['3', '6225242', '', 'BRICK 1X2X5', 'Light Bluish Gray', '12.5', '', '3', '', '', '', '', '', '1.5', ''],
  ['4', '4211388', '', 'MYSTERY PART', 'Black', '', '', '7', '', '', '', '', '', '', ''],
]);
const recs = sheet.records;

// The text a PDF shows, per page: every string drawn with Tj (pdf-lib
// writes WinAnsi hex strings for the standard fonts).
async function pdfText(bytes) {
  const doc = await PDFDocument.load(bytes);
  const decoder = new TextDecoder('windows-1252');
  return doc.getPages().map((page) => {
    const contents = page.node.Contents();
    const streams = contents instanceof PDFArray ? contents.asArray() : [contents];
    let ops = '';
    for (const ref of streams) {
      const stream = doc.context.lookup(ref);
      ops += new TextDecoder('latin1').decode(decodePDFRawStream(stream).decode());
    }
    const out = [];
    for (const m of ops.matchAll(/<([0-9A-Fa-f]*)>\s*Tj/g)) {
      out.push(decoder.decode(Buffer.from(m[1], 'hex')));
    }
    return out;
  });
}

test('printf helpers round like glibc', () => {
  assert.equal(formatF(12.5, 0), '12');
  assert.equal(formatF(13.5, 0), '14');
  assert.equal(formatF(0.125, 2), '0.12');
  assert.equal(formatF(-3.25, 1), '-3.2');
  assert.equal(formatG(1.125, 3), '1.12');
  assert.equal(formatG(0.00001234, 3), '1.23e-05');
  assert.equal(formatG(1234567, 6), '1.23457e+06');
  assert.equal(formatG(999999.5, 6), '1e+06');
  assert.equal(formatG(0.05, 2), '0.05');
  assert.equal(formatG(4.6225, 2), '4.6');
  assert.equal(formatG(100, 3), '100');
  assert.equal(formatG(0, 3), '0');
  assert.equal(formatG(NaN), 'nan');
  // Against the system's printf where there is one.
  let printf = true;
  try {
    execFileSync('printf', ['%s', 'x']);
  } catch {
    printf = false;
  }
  if (printf) {
    for (const v of [0.1, 2.675, 9.995, 99.95, 1234.5, 0.00012345, 123456, 1e21, 1e-7, 5e-324]) {
      for (const [k, p] of [['g', 6], ['g', 3], ['g', 2], ['f', 0], ['f', 2]]) {
        const want = execFileSync('printf', [`%.${p}${k}`, v.toPrecision(17)]).toString();
        assert.equal(k === 'g' ? formatG(v, p) : formatF(v, p), want, `%.${p}${k} of ${v}`);
      }
    }
  }
});

test('report helpers', () => {
  assert.deepEqual(reports.personSortKey('Otto Brandt'), ['brandt', 'otto brandt']);
  assert.deepEqual(reports.personSortKey('Otto Brandt', 'first'), ['otto brandt', 'otto brandt']);
  assert.equal(reports.csvField('=SUM(A1)'), "'=SUM(A1)");
  assert.equal(reports.csvField('-5'), "'-5");
  assert.equal(reports.csvField('a,b'), '"a,b"');
  assert.equal(reports.csvField('say "hi"'), '"say ""hi"""');
  assert.equal(reports.csvField('@x,"y"'), '"\'@x,""y"""');
  assert.equal(reports.weightText({ weight: null }), 'size unknown');
  assert.equal(reports.weightText({ weight: 12.5, weight_source: 'sheet' }), '12 g/pc');
  assert.equal(reports.weightText({ weight: 4.6225, weight_source: 'estimate' }), '~4.6 g/pc');
  assert.equal(reports.weightText({ weight: 0.43, weight_source: 'bricklink' }), '0.43 g/pc');
  assert.equal(reports.formatCount(2000), '2000');
  assert.equal(reports.formatCount(1.5), '1.5');
  assert.equal(reports.massText(850.4), '850 g');
  assert.equal(reports.massText(12400), '12.40 kg');
  assert.equal(reports.winAnsi('Zoë — “ok” ą\tb'), 'Zoë — “ok” ? b');
  assert.equal(reports.fileStem('ArkLUG 2026: Bulk!'), 'ArkLUG 2026_ Bulk');
  assert.equal(reports.fileStem('Zoë.xlsx', true), 'Zo');
  assert.equal(reports.fileStem('.xlsx', true), 'order sheet');
  assert.equal(reports.fileStem('***'), 'sheet');
});

test('lot counts and parts CSV', () => {
  const lots = reports.lotCountsCsv(recs);
  assert.equal(
    lots,
    'person,lot_count,total_pieces\r\n' +
      'Otto Brandt,2,10\r\n' +
      "'=Evil Formula,1,1\r\n" +
      '"Zoë ""Zed"" Nakamura",2,3.5\r\n' +
      'Wendy Quill,3,20\r\n',
  );
  const first = reports.lotCountsCsv(recs, 'first').split('\r\n').slice(1, 5).map((l) => l.split(',')[0]);
  assert.deepEqual(first, ["'=Evil Formula", 'Otto Brandt', 'Wendy Quill', '"Zoë ""Zed"" Nakamura"']);
  assert.equal(reports.lotCountsCsv(recs, 'last', 2).split('\r\n').length, 5); // header + 3 + ''

  const parts = reports.reportCsv('parts', recs, null, 'heaviest').split('\r\n');
  assert.equal(parts[0], 'order,element_id,description,lego_color,bl_color,total_pieces,people,grams_per_piece,weight_source');
  assert.equal(parts[1], '1,6225242,BRICK 1X2X5,Medium Stone Grey,Light Bluish Gray,4.5,2,12.5,sheet');
  assert.match(parts[2], /^2,3001,BRICK 2X4,Bright Red,Red,16,3,3.44,estimate$/);
  assert.match(parts[3], /^3,3020,"PLATE 2X4, EXTRA",/);
  assert.equal(parts[4], '4,4211388,MYSTERY PART,Black,Black,7,1,,');
  const byId = reports.reportCsv('parts', recs, { parts: { order: 'element' } }).split('\r\n');
  assert.deepEqual(byId.slice(1, 5).map((l) => l.split(',')[1]), ['3001', '3020', '4211388', '6225242']);
});

test('sheet check text', () => {
  assert.equal(reports.sheetCheckText({ records: [1, 2], issues: [] }), '2 labels\nNo issues found.\n');
  assert.equal(
    reports.sheetCheckText({ records: [], issues: [{ row: 4, kind: 'bad_qty', detail: 'Nope' }] }),
    '0 labels\nRow 4 (bad_qty): Nope\n',
  );
});

test('lot counts PDF: default layout and text', async () => {
  const bytes = await reports.lotCountsPdf(recs);
  const doc = await PDFDocument.load(bytes);
  assert.equal(doc.getPageCount(), 1);
  assert.deepEqual(doc.getPage(0).getSize(), { width: 612, height: 792 });
  const [page] = await pdfText(bytes);
  assert.deepEqual(page.slice(0, 5), ['Lot counts by person', '4 people, 8 lots total — sorted by last name', 'Person', 'Lots', 'Total pieces']);
  assert.deepEqual(page.slice(5, 8), ['Otto Brandt', '2', '10']);
  assert.ok(page.includes('Zoë "Zed" Nakamura'));
  assert.ok(!page.some((t) => t.startsWith('Total (')));
});

test('lot counts PDF: options', async () => {
  const o = {
    lots: { sort: 'first', totals: true, total_weight: true, pieces: false, min_lots: 2, title: 'Brick Club 2031', subtitle: 'Pickup day', paper: 'a4', orientation: 'landscape' },
  };
  const bytes = await reports.lotCountsPdf(recs, o);
  const doc = await PDFDocument.load(bytes);
  const { width, height } = doc.getPage(0).getSize();
  assert.deepEqual([width, height], [841.89, 595.28]);
  const [page] = await pdfText(bytes);
  assert.deepEqual(page.slice(0, 3), ['Brick Club 2031', 'Pickup day', '3 people, 7 lots total — sorted by first name (1 with fewer than 2 lots left out)']);
  assert.ok(!page.includes('Total pieces'));
  assert.ok(page.includes('Total weight'));
  assert.ok(page.includes('Total (3 people)'));
  assert.ok(!page.includes('=Evil Formula'));
});

test('parts PDF: default and grouped', async () => {
  const [page] = await pdfText(await reports.partsPdf(recs, undefined, { labelOrder: 'lightest' }));
  assert.equal(page[0], 'Parts list');
  assert.equal(page[1], '4 parts, 34.5 pieces, 8 labels — in label order');
  assert.deepEqual(page.slice(2, 9), ['#', 'Element', 'Description', 'LEGO / BrickLink color', 'Pieces', 'People', 'Weight']);
  assert.deepEqual(page.slice(9, 16), ['1', '3020', 'PLATE 2X4, EXTRA', 'Bright Blue / Blue', '7', '2', '~1.1 g/pc']);
  assert.ok(page.includes('size unknown'));

  const grouped = await pdfText(await reports.partsPdf(recs, { parts: { group_by_color: true, bl_color: false, total_weight: true, order: 'element' } }));
  const g = grouped[0];
  assert.ok(g[1].endsWith('by element ID, grouped by color'));
  assert.ok(g.includes('LEGO color'));
  assert.ok(g.includes('Total weight'));
  assert.ok(g.includes('Black (1 part)'));
  assert.ok(g.indexOf('Black (1 part)') < g.indexOf('Bright Blue (1 part)'));
});

test('checklist PDF: a page per person, or continuous', async () => {
  const bytes = await reports.checklistPdf(recs, undefined, { labelOrder: 'heaviest' });
  const pages = await pdfText(bytes);
  assert.equal(pages.length, 4);
  assert.deepEqual(pages.map((p) => p[0]), ['Otto Brandt', '=Evil Formula', 'Zoë "Zed" Nakamura', 'Wendy Quill']);
  assert.equal(pages[3][1], '3 lots, 20 pieces');
  assert.deepEqual(pages[3].slice(2, 7), ['Element', 'Description', 'LEGO / BrickLink color', 'Qty', 'Label']);
  // Too long for its column: wraps onto a second line, as the server does.
  assert.deepEqual(pages[3].slice(7, 13), ['6225242', 'BRICK 1X2X5', 'Medium Stone Grey / Light Bluish', 'Gray', '3', '2 of 2']);

  const cont = await pdfText(
    await reports.checklistPdf(recs, { checklist: { layout: 'continuous', packed_by: true, weight: true, checkbox: false, title: 'Brick Club', subtitle: 'Fall 2031' } }),
  );
  assert.equal(cont.length, 1);
  const p = cont[0];
  assert.deepEqual(p.slice(0, 2), ['Brick Club', 'Fall 2031']);
  assert.equal(p.filter((t) => t === 'Packed by:').length, 4);
  assert.ok(p.includes('Weight'));
});

test('checklist PDF: long lists run onto more pages', async () => {
  const rows = [header, marker];
  for (let i = 0; i < 95; i++) rows.push([String(i + 1), String(300100 + i), '', `TILE 1X${(i % 8) + 1}`, 'White', '', '', '1', '', '', '', '', '', '', '']);
  const many = pivotSheet(rows).records;
  const letter = await PDFDocument.load(await reports.checklistPdf(many));
  assert.equal(letter.getPageCount(), 3); // 40 rows a page, as the server
  const pages = await pdfText(await reports.checklistPdf(many, { checklist: { layout: 'continuous' } }));
  assert.equal(pages[1][0], 'Wendy Quill (continued)');
});

test('photos are embedded when given, skipped when not JPEG', async () => {
  // A 1x1 baseline JPEG.
  const jpeg = Buffer.from(
    '/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=',
    'base64',
  );
  const o = { parts: { photo: true }, checklist: { photo: true } };
  assert.deepEqual(reports.imageIdsFor('parts', recs, o), ['3001', '3020', '6225242', '4211388']);
  assert.deepEqual(reports.imageIdsFor('parts', recs, null), []);
  assert.deepEqual(reports.imageIdsFor('lots', recs, o), []);
  const images = new Map([['3001', new Uint8Array(jpeg)], ['3020', new Uint8Array([1, 2, 3])]]);
  const bytes = await reports.partsPdf(recs, o, { images });
  await PDFDocument.load(bytes);
  // Image streams aren't packed into object streams, so their dicts show.
  const raw = Buffer.from(bytes).toString('latin1');
  assert.equal((raw.match(/\/Subtype \/Image/g) || []).length, 1);
  await PDFDocument.load(await reports.checklistPdf(recs, o, { images }));
});

test('column fitting', () => {
  assert.deepEqual(reports.fitColumns([280, 100, 120], [true, false, false], 527), [280, 100, 120]);
  assert.deepEqual(reports.fitColumns([24, 50, 150], [false, false, true], 222), [24, 50, 150]);
  const wide = reports.fitColumns([280, 100, 120], [true, false, false], 707);
  assert.equal(Math.round(wide.reduce((a, b) => a + b)), 707);
  const narrow = reports.fitColumns([100, 100, 100], [false, true, false], 200);
  assert.ok(narrow.reduce((a, b) => a + b) <= 200 + 1e-9);
});

test('options: defaults, clamping, junk', () => {
  const d = opts.normalizeOptions(null);
  assert.deepEqual(d, JSON.parse(JSON.stringify(opts.DEFAULTS)));
  assert.equal(opts.parseOptions('not json').lots.sort, 'last');
  const o = opts.normalizeOptions({
    checklist: { sort: 'first', order: 'element', layout: 'continuous', photo: 'yes', packed_by: true, extra: 1 },
    parts: { order: 'element', paper: 'A4', orientation: 'landscape', title: '  Big\n\tLUG  ' + 'x'.repeat(200) },
    lots: { min_lots: '12', totals: 1 },
    zip: { labels: false, check_txt: 0 },
    bogus: {},
  });
  assert.equal(o.checklist.sort, 'first');
  assert.equal(o.checklist.order, 'labels'); // 'element' is for the parts list only
  assert.equal(o.checklist.layout, 'continuous');
  assert.equal(o.checklist.photo, false);
  assert.equal(o.checklist.packed_by, true);
  assert.ok(!('extra' in o.checklist));
  assert.ok(!('bogus' in o));
  assert.equal(o.parts.order, 'element');
  assert.equal(o.parts.paper, 'letter');
  assert.equal(o.parts.orientation, 'landscape');
  assert.equal(o.parts.title.length, opts.MAX_TITLE);
  assert.ok(o.parts.title.startsWith('Big LUG x'));
  assert.equal(o.lots.min_lots, 12);
  assert.equal(o.lots.totals, false);
  assert.equal(opts.normalizeOptions({ lots: { min_lots: 1e9 } }).lots.min_lots, opts.MAX_MIN_LOTS);
  assert.equal(opts.normalizeOptions({ lots: { min_lots: -3 } }).lots.min_lots, 0);
  assert.equal(opts.normalizeOptions({ lots: { min_lots: 'x' } }).lots.min_lots, 0);
  assert.deepEqual(opts.zipSelection(o), ['checklist_pdf', 'parts_pdf', 'parts_csv', 'lots_pdf', 'lots_csv', 'check_txt']);
  assert.deepEqual(opts.pageSize({ paper: 'a4', orientation: 'portrait' }), [595.28, 841.89]);
  assert.deepEqual(opts.pageSize({ paper: 'letter', orientation: 'landscape' }), [792, 612]);
  // The largest possible options still fit the server's limit.
  const big = '\u{1F9F1}"\\'.repeat(200);
  const worst = {};
  for (const k of ['checklist', 'parts', 'lots']) worst[k] = { title: big, subtitle: big };
  assert.ok(new TextEncoder().encode(opts.serializeOptions(worst)).length <= opts.MAX_OPTIONS_BYTES);
});

test('zip: structure, CRCs, UTF-8 names', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  assert.deepEqual(dosDateTime(new Date(2031, 9, 4, 13, 45, 31)), [(13 << 11) | (45 << 5) | 15, (51 << 9) | (10 << 5) | 4]);
  const files = [
    ['Brick Club labels.pdf', new Uint8Array([37, 80, 68, 70, 45, 1, 2, 3])],
    ['Zoë parts.csv', 'a,b\r\n1,2\r\n'],
    ['empty.txt', ''],
  ];
  const out = zip(files, new Date(2031, 0, 2, 3, 4, 6));
  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  const eocd = out.length - 22;
  assert.equal(view.getUint32(eocd, true), 0x06054b50);
  assert.equal(view.getUint16(eocd + 10, true), 3);
  let cd = view.getUint32(eocd + 16, true);
  const names = [];
  for (let i = 0; i < 3; i++) {
    assert.equal(view.getUint32(cd, true), 0x02014b50);
    assert.equal(view.getUint16(cd + 8, true), 1 << 11);
    const size = view.getUint32(cd + 20, true);
    const nameLen = view.getUint16(cd + 28, true);
    const local = view.getUint32(cd + 42, true);
    const name = new TextDecoder().decode(out.subarray(cd + 46, cd + 46 + nameLen));
    names.push(name);
    assert.equal(view.getUint32(local, true), 0x04034b50);
    const data = out.subarray(local + 30 + nameLen, local + 30 + nameLen + size);
    assert.equal(crc32(data), view.getUint32(cd + 16, true));
    const want = files[i][1];
    assert.deepEqual(Buffer.from(data), Buffer.from(typeof want === 'string' ? new TextEncoder().encode(want) : want));
    cd += 46 + nameLen;
  }
  assert.deepEqual(names, files.map(([n]) => n));

  let unzip = true;
  try {
    execFileSync('unzip', ['-v'], { stdio: 'ignore' });
  } catch {
    unzip = false;
  }
  if (unzip) {
    const dir = mkdtempSync(join(tmpdir(), 'lugbulk-zip-'));
    try {
      const path = join(dir, 'test.zip');
      writeFileSync(path, out);
      execFileSync('unzip', ['-tq', path]);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  }
});

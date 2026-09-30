// Unit tests for reading sheets in the browser: static/js/spreadsheet.js
// (the .xlsx/.csv reader; its parity with the C++ is parity.test.mjs) and
// static/js/load.js (loadSheet, with a mocked fetch). All data is made up.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { deflateRawSync } from 'node:zlib';

import { LOOKUP_BATCH, LoadError, loadSheet, lookupBricklink } from '../../static/js/load.js';
import {
  MAX_UNPACKED_BYTES,
  SpreadsheetError,
  isXlsx,
  readCsv,
  readTabs,
  readXlsx,
} from '../../static/js/spreadsheet.js';

const fixture = (name) => readFileSync(new URL(`../fixtures/${name}`, import.meta.url));

// A minimal zip: [{name, data, deflate = true, claimedSize, dropTail}].
function zip(parts) {
  const out = [];
  const central = [];
  let offset = 0;
  const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];
  const u32 = (v) => [...u16(v & 0xffff), ...u16(v >>> 16)];
  for (const p of parts) {
    const data = Buffer.from(p.data);
    let body = p.deflate === false ? data : deflateRawSync(data);
    if (p.dropTail) body = body.subarray(0, body.length - p.dropTail);
    const name = Buffer.from(p.name);
    const method = p.method ?? (p.deflate === false ? 0 : 8);
    const size = p.claimedSize ?? data.length;
    const local = [...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(method), ...u32(0), ...u32(0),
      ...u32(body.length), ...u32(size), ...u16(name.length), ...u16(0)];
    central.push(Buffer.from([...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(method),
      ...u32(0), ...u32(0), ...u32(body.length), ...u32(size), ...u16(name.length), ...u32(0), ...u32(0),
      ...u32(0), ...u32(offset)]), name);
    out.push(Buffer.from(local), name, body);
    offset += local.length + name.length + body.length;
  }
  const cd = Buffer.concat(central);
  const end = Buffer.from([...u32(0x06054b50), ...u32(0), ...u16(parts.length), ...u16(parts.length),
    ...u32(cd.length), ...u32(offset), ...u16(0)]);
  return new Uint8Array(Buffer.concat([...out, cd, end]));
}

const workbook = (sheetXml, sheetName = 'Order Here', extra = []) => zip([
  { name: 'xl/workbook.xml', data: `<workbook><sheets><sheet name="Other" r:id="rId2"/><sheet name="${sheetName}" r:id="rId1"/></sheets></workbook>` },
  { name: 'xl/_rels/workbook.xml.rels', data: '<Relationships><Relationship Id="rId1" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Target="worksheets/sheet2.xml"/></Relationships>' },
  { name: 'xl/sharedStrings.xml', data: '<sst><si><t>Element ID</t></si><si><r><t>Ann</t></r><r><t> Lee</t></r></si><si><t>Caf&#233; &amp; &#x1F600;</t></si></sst>' },
  { name: 'xl/worksheets/sheet1.xml', data: sheetXml },
  { name: 'xl/worksheets/sheet2.xml', data: '<worksheet><sheetData><row><c t="inlineStr"><is><t>other</t></is></c></row></sheetData></worksheet>' },
  ...extra,
]);

const rejects = (promise, message) =>
  assert.rejects(promise, (e) => {
    assert.ok(e instanceof SpreadsheetError, `${e}`);
    assert.equal(e.message, message);
    return true;
  });

test('csv: quotes, line ends, BOM', () => {
  assert.deepEqual(readCsv('﻿a,"b,""c"""\r\n"x\ny",\rlast'), [['a', 'b,"c"'], ['x\ny', ''], ['last']]);
  assert.deepEqual(readCsv(new TextEncoder().encode('﻿Émile,2\n')), [['Émile', '2']]);
  assert.deepEqual(readCsv(''), []);
  assert.ok(!isXlsx(new TextEncoder().encode('a,b')));
});

test('xlsx: the order tab, cell kinds, sparse rows', async () => {
  const d = workbook('<worksheet><sheetData>' +
    '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="C1" t="s"><v>1</v></c></row>' +
    '<row r="3"><c r="A3"><v>4211407.0</v></c><c r="B3"><v>0.83</v></c><c r="C3" t="b"><v>1</v></c>' +
    '<c r="D3" t="s"><v>2</v></c><c r="E3"><v>1234567890123465</v></c><c r="F3" t="str"><v>a &lt; b</v></c></row>' +
    '</sheetData></worksheet>', ' order HERE');
  assert.ok(isXlsx(d));
  const tabs = await readXlsx(d, 'Order Here');
  assert.deepEqual(tabs, [[
    ['Element ID', '', 'Ann Lee'],
    [],
    ['4211407', '0.83', 'TRUE', 'Café & 😀', '1.23456789012346e+15', 'a < b'],
  ]]);
});

test('xlsx: no order tab means every tab, in workbook order', async () => {
  const tabs = await readTabs(workbook('<worksheet><sheetData/></worksheet>', 'Parts'), 'Order Here');
  assert.deepEqual(tabs, [[['other']], []]);
});

test('xlsx: the sample fixture', async () => {
  const tabs = await readTabs(fixture('sample_order.xlsx'), 'Order Here');
  assert.equal(tabs.length, 1);
  assert.ok(tabs[0].some((row) => row.includes('4211388')));
});

test('xlsx: zip bombs and damaged files are refused', async () => {
  const tooBig = 'That .xlsx file unpacks to more than 64 MB — too big.';
  await rejects(readTabs(fixture('bomb.xlsx')), tooBig);
  // A header that lies about the size: caught on what actually comes out.
  const huge = `<worksheet><sheetData>${' '.repeat(MAX_UNPACKED_BYTES)}</sheetData></worksheet>`;
  await rejects(readXlsx(workbook(huge), 'Order Here').then(() => {}), tooBig);
  const lying = zip([{ name: 'xl/workbook.xml', data: huge, claimedSize: 10 }]);
  await rejects(readXlsx(lying, 'Order Here'), tooBig);

  const truncated = zip([
    { name: 'xl/workbook.xml', data: '<workbook>'.repeat(100), dropTail: 8 },
    { name: 'xl/_rels/workbook.xml.rels', data: '<Relationships/>' },
  ]);
  await rejects(readXlsx(truncated, 'Order Here'), 'That .xlsx file is damaged.');
  await rejects(readXlsx(zip([{ name: 'xl/workbook.xml', data: 'x', method: 12 }]), 'x'),
    'That .xlsx file uses an unsupported compression.');
  await rejects(readTabs(new TextEncoder().encode('PK\x03\x04 but not a zip at all, no')),
    "That file isn't a valid .xlsx workbook.");
  await rejects(readXlsx(zip([{ name: 'a.txt', data: 'hi' }]), 'x'), "That file isn't an Excel workbook.");
});

// --- loadSheet ---------------------------------------------------------------

// A fake server: routes -> (body) => {status, body}.
function fakeFetch(routes) {
  const calls = [];
  const fetch = async (path, opts = {}) => {
    calls.push({ path, opts });
    const route = routes[path];
    if (!route) return new Response('not found', { status: 404 });
    const { status = 200, body } = await route(opts.body ? JSON.parse(opts.body) : null);
    return new Response(typeof body === 'string' ? body : JSON.stringify(body), { status });
  };
  return { fetch, calls };
}

const HEADER = ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Pat Example', '$$', 'Quinn Doe', '$$'];
const MARKER = ['', '', '', '', '', 'qty', '$$', 'qty', '$$'];
const sheetRows = [
  HEADER,
  MARKER,
  ['1', '3001', '', 'BRICK 2X4', '', '2', '', '1', ''],
  ['2', '3020', '', 'PLATE 2X4', 'Red', '', '', 'lots', ''],
];

test('loadSheet: a saved sheet via /values, with BrickLink data', async () => {
  const { fetch, calls } = fakeFetch({
    '/sheets/7/values': () => ({ body: { rows: sheetRows } }),
    '/bricklink/lookup': ({ ids }) => ({
      body: Object.fromEntries(ids.filter((id) => id === '3001').map((id) => [id, { part: id, color: 'White', weight: 2.3 }])),
    }),
  });
  const r = await loadSheet({ rowId: 7 }, { fetch });
  assert.deepEqual(calls.map((c) => c.path), ['/sheets/7/values', '/bricklink/lookup']);
  assert.deepEqual(JSON.parse(calls[1].opts.body), { ids: ['3001'] }); // 3020 has no valid qty
  assert.equal(r.lookupError, null);
  assert.deepEqual(r.records.map((x) => [x.person, x.element_id, x.qty, x.bl_color, x.catalog_weight]), [
    ['Pat Example', '3001', '2', 'White', 2.3],
    ['Quinn Doe', '3001', '1', 'White', 2.3],
  ]);
  // BrickLink filled in 3001's missing color; the bad qty stays.
  assert.deepEqual(r.summary.issues.map((i) => i.kind), ['bad_qty']);
  assert.deepEqual({ ...r.summary, issues: undefined }, { labels: 2, people: 2, parts: 1, issues: undefined });
  assert.equal(r.issues.length, 1);
});

test('loadSheet: an upload is read in the browser, not sent', async () => {
  const { fetch, calls } = fakeFetch({ '/bricklink/lookup': () => ({ body: {} }) });
  const r = await loadSheet(new Blob([fixture('sample_order.xlsx')]), { fetch });
  assert.equal(r.summary.labels, 6);
  assert.deepEqual(calls.map((c) => c.path), ['/bricklink/lookup']);

  const csv = new Blob([sheetRows.map((row) => row.join(',')).join('\n')]);
  assert.equal((await loadSheet(csv, { fetch })).summary.labels, 2);
});

test('loadSheet: errors are clear', async () => {
  const google = "Google couldn't read the 'Order Here' tab — check the sheet has a tab by that name.";
  const { fetch } = fakeFetch({
    '/sheets/1/values': () => ({ status: 502, body: google }),
    '/sheets/2/values': () => ({ status: 429, body: '' }),
    '/bricklink/lookup': () => ({ status: 429, body: 'Too many lookups — wait a moment.' }),
    '/sheets/3/values': () => ({ body: { rows: sheetRows } }),
  });
  const fails = (source, message, status) =>
    assert.rejects(loadSheet(source, { fetch }), (e) => {
      assert.ok(e instanceof LoadError);
      assert.equal(e.message, message);
      if (status !== undefined) assert.equal(e.status, status);
      return true;
    });
  await fails({ rowId: 1 }, google, 502);
  await fails({ rowId: 2 }, 'Request failed (429) — try again.', 429);
  await fails(new Blob([fixture('bomb.xlsx')]), 'That .xlsx file unpacks to more than 64 MB — too big.');
  await fails(new Blob([]), 'Choose an .xlsx or .csv file first.');
  await fails(new Blob([new Uint8Array(10 * 1024 * 1024 + 1)]), 'That file is over 10 MB — too big to upload.');

  const many = [HEADER, MARKER];
  for (let i = 0; i < 2001; i++) many.push(['', String(1000000 + i), '', 'TILE 1X1', 'White', '1', '', '', '']);
  await fails(new Blob([many.map((row) => row.join(',')).join('\n')]),
    'Too big: this sheet has 2001 labels and 2001 parts; the limit is 20000 labels / 2000 parts per run.', 413);

  // A failed BrickLink lookup doesn't stop the sheet loading.
  const r = await loadSheet({ rowId: 3 }, { fetch });
  assert.equal(r.summary.labels, 2);
  assert.equal(r.lookupError, 'Too many lookups — wait a moment.');
  assert.deepEqual(r.summary.issues.map((i) => i.kind), ['missing_color', 'bad_qty']);
});

test('lookupBricklink batches ids', async () => {
  const { fetch, calls } = fakeFetch({
    '/bricklink/lookup': ({ ids }) => ({ body: { [ids[0]]: { part: 'p', color: '', weight: null } } }),
  });
  const ids = Array.from({ length: 4500 }, (_, i) => String(1000000 + i));
  const lookup = await lookupBricklink(ids, fetch);
  assert.deepEqual(calls.map((c) => JSON.parse(c.opts.body).ids.length), [LOOKUP_BATCH, LOOKUP_BATCH, 500]);
  assert.deepEqual(Object.keys(lookup), ['1000000', '1002000', '1004000']);
});

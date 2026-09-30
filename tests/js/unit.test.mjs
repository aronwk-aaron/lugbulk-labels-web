// Unit tests for the browser modules in static/js/ that need no golden
// files (parity with the C++ is parity.test.mjs). `node --test tests/js/`

import assert from 'node:assert/strict';
import { test } from 'node:test';

import * as colors from '../../static/js/colors.js';
import * as layout from '../../static/js/layout.js';
import * as ordering from '../../static/js/ordering.js';
import { isValidElementId, parseQty, pivotSheet } from '../../static/js/pivot.js';
import * as records from '../../static/js/records.js';
import { compareBytes, trim } from '../../static/js/text.js';

const header = ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Cost', 'Total', 'Ann Lee', '', 'Bob Roe', ''];
const marker = ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$'];

test('qty marker layout', () => {
  const r = pivotSheet([
    header,
    marker,
    ['1', '4211388', '', 'BRICK 1X2, GREY', 'Light Bluish Gray', '0.05', '', '2,000', '$1', '', ''],
    ['2', '3001', '', 'BRICK 2X4', 'MED. ST-GREY', '', '', '0.0078125', '', 'x', ''],
  ]);
  assert.equal(r.records.length, 2);
  assert.equal(r.records[0].qty, '2000');
  assert.equal(r.records[0].lego_color, 'Medium Stone Grey');
  assert.equal(r.records[0].image_url, '/img/4211388.jpg');
  // printf("%f") rounds this exact tie to even; toFixed(6) would say 0.007813.
  assert.equal(r.records[1].qty, '0.007812');
  // A LEGO name in the BL column: recognized, and kept as written.
  assert.equal(r.records[1].lego_color, 'Medium Stone Grey');
  assert.equal(r.records[1].bl_color, 'MED. ST-GREY');
  assert.deepEqual(
    r.issues.map((i) => [i.row, i.kind, i.detail]),
    [[4, 'bad_qty', "Bob Roe's qty for element 3001 is non-numeric: 'x'"]],
  );
});

test('name/cost pair layout, duplicates, colors', () => {
  const r = pivotSheet([
    ['', '', '', '', '10'],
    ['Part Number', 'Description', 'LEGO Color', 'BL Color', 'Nominated for', 'Ann Lee', '12.5', 'Bob Roe', '€3', 'Notes', '1'],
    [],
    ['3001', 'BRICK 2X4', 'Fancy', '', '', '1', '', '', '', '', ''],
    ['3001', 'BRICK 2X4', '', 'Red', '', '2', '', '', '', '', ''],
    ['3002', '', '?', '', '', '', '', '4', '', '', ''],
  ]);
  assert.deepEqual(r.records.map((x) => [x.person, x.element_id, x.qty]), [
    ['Ann Lee', '3001', '1'],
    ['Ann Lee', '3001', '2'],
    ['Bob Roe', '3002', '4'],
  ]);
  assert.deepEqual(r.issues.map((i) => i.kind), ['unmapped_color', 'duplicate', 'missing_description', 'missing_color']);
  assert.equal(
    r.issues[0].detail,
    "Element 3001: don't know the BrickLink name for 'Fancy' — fill in the sheet's BL Color column",
  );
  assert.equal(r.issues[3].element_id, '3002');
});

test('parseQty and element ids', () => {
  assert.equal(parseQty('2,000'), 2000);
  assert.equal(parseQty(' 1e3 '), 1000);
  assert.equal(parseQty('0x10'), 16);
  assert.equal(parseQty('inf'), Infinity);
  for (const bad of ['', 'abc', '1e400', '12abc', '0x', '5\v']) assert.throws(() => parseQty(bad));
  assert.ok(isValidElementId('1234') && isValidElementId('12345678'));
  assert.ok(!isValidElementId('123') && !isValidElementId('123456789') && !isValidElementId('../1'));
});

test('ordering', () => {
  const rec = (person, id, qty, description, weight = null) => ({
    person, element_id: id, description, lego_color: '', bl_color: '', qty,
    image_url: '', weight, catalog_weight: null, part_seq: 0, part_total: 0,
  });
  const rs = [
    rec('Zed Adams', '1111', '5', 'PLATE 1X1'),
    rec('Amy Young', '2222', '3', 'BRICK 2X4'),
    rec('Bea Young', '2222', '3', 'BRICK 2X4'),
    rec('Cal Brown', '2222', '1', 'BRICK 2X4'),
    rec('Dee Frog', '3333', '1', 'FROG'),
  ];
  const heavy = ordering.orderRecords(rs, 'heaviest');
  assert.deepEqual(heavy.map((r) => [r.person, r.part_seq, r.part_total]), [
    ['Cal Brown', 1, 3], ['Amy Young', 2, 3], ['Bea Young', 3, 3],
    ['Zed Adams', 1, 1], ['Dee Frog', 1, 1],
  ]);
  assert.equal(rs[0].part_seq, 0); // input untouched
  const parts = ordering.summarizeParts(rs, 'lightest');
  assert.deepEqual(parts.map((p) => [p.element_id, p.lots, p.pieces, p.weight_source]), [
    ['1111', 1, 5, 'estimate'], ['2222', 3, 7, 'estimate'], ['3333', 1, 1, ''],
  ]);
  assert.ok(Math.abs(ordering.estimateWeight('BRICK 1X1X1 2/3') - (5 / 3) * 0.43) < 1e-12);
  assert.equal(ordering.parsePartOrder('sheet'), 'sheet');
  assert.equal(ordering.parsePartOrder('Sheet'), null);
});

test('records: limits, BrickLink, check summary', () => {
  const rows = [header, marker];
  for (let i = 0; i < 2001; i++) rows.push(['', String(100000 + i), '', 'X', 'Red', '', '', '1', '', '', '']);
  assert.throws(() => records.pivotRows(rows), {
    name: 'TooBigError',
    message: 'this sheet has 2001 labels and 2001 parts; the limit is 20000 labels / 2000 parts per run',
  });
  assert.equal(records.capRows(new Array(3005).fill([])).length, 3000);

  const p = records.pivotRows([header, marker, ['', '3001', '', 'BRICK 2X4', '', '', '', '1', '', '2', '']]);
  assert.deepEqual(p.issues.map((i) => i.kind), ['missing_color']);
  assert.deepEqual(records.elementIds(p), ['3001']);
  const applied = records.applyBricklink(p, { 3001: { part: '3001', color: 'Light Bluish Gray', weight: 2.3 } });
  assert.equal(applied.records[0].lego_color, 'Medium Stone Grey');
  assert.equal(applied.records[0].catalog_weight, 2.3);
  assert.equal(p.records[0].bl_color, ''); // input untouched
  assert.deepEqual(records.checkSummary(applied), { labels: 2, people: 2, parts: 1, issues: [] });
  assert.deepEqual(records.checkSummary(p).issues, [{ row: 3, kind: 'missing_color', detail: 'Element 3001 has no color' }]);
});

test('colors and text helpers', () => {
  assert.deepEqual(colors.resolve('', 'Trans-Clear'), { lego: 'Transparent', bl: 'Trans-Clear', mapped: true });
  assert.ok(colors.isTransparent('TR.', ''));
  assert.ok(colors.isLight('', 'Glow In Dark White'));
  assert.deepEqual(colors.swatchRgb('WHITE', ''), [1, 1, 1]);
  assert.equal(colors.swatchRgb('Nope', 'Nope'), null);
  assert.equal(trim('\v a \t\r\n'), '\v a');
  assert.ok(compareBytes('\u{1F600}', 'ａ') > 0); // code point, not UTF-16, order
  const find = layout.labelSpecFinder({
    specs: [{ id: 'avery5162', brand: 'Avery', part: '5162', equivalents: ['8162'] }],
  });
  assert.equal(find('Avery 8162')?.id, 'avery5162');
  assert.equal(find('nope'), null);
});

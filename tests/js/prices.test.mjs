// Prices on the labels: the sheet's price per piece on each record, and the
// "Price each" / "Lot price" label lines. Made-up data. `node --test tests/js/`

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { helveticaMeasure, layoutLabel, optionsFromHidden, priceLine } from '../../static/js/labels.js';
import { unitPrices } from '../../static/js/pivot.js';
import * as opts from '../../static/js/report_options.js';
import * as records from '../../static/js/records.js';

const rows = [
  ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Cost\nEach', 'Total', 'Ann Lee', '', 'Bob Roe', ''],
  ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$'],
  ['1', '3001', '', 'BRICK 2X4', 'Red', '$0.0696', '', '1,000', '', '3', ''],
  ['2', '3003', '', 'BRICK 2X2', 'Red', '', '', '', '', '2', ''],
];

test('records get the sheet price and currency', () => {
  assert.equal(unitPrices(rows).symbol, '$');
  const pivot = records.pivotRows(rows);
  assert.equal(pivot.rows.length, rows.length);
  const priced = records.addPrices(pivot, pivot.rows);
  assert.deepEqual(priced.records.map((r) => [r.person, r.element_id, r.price, r.currency]), [
    ['Ann Lee', '3001', 0.0696, '$'],
    ['Bob Roe', '3001', 0.0696, '$'],
    ['Bob Roe', '3003', null, '$'],
  ]);
});

test('priceLine', () => {
  const r = { qty: '1000', price: 0.0696, currency: '$' };
  assert.equal(priceLine(r, { price: true, lot_price: true }), '$0.0696 each · Lot $69.60');
  assert.equal(priceLine(r, { price: false, lot_price: true }), 'Lot $69.60');
  assert.equal(priceLine({ qty: '3', price: 0.5, currency: '' }, { price: true }), '0.50 each');
  assert.equal(priceLine({ qty: '3', price: null }, { price: true, lot_price: true }), '');
  assert.equal(priceLine(r, {}), '');
});

test('the price line goes on the label only when switched on', () => {
  const r = { person: 'Ann Lee', element_id: '3001', qty: '1000', description: 'BRICK 2X4', lego_color: 'Bright Red',
              bl_color: 'Red', price: 0.0696, currency: '$', part_seq: 1, part_total: 2 };
  const show = optionsFromHidden('qr,photo');
  const texts = (s) => layoutLabel(r, 190, 72, s, helveticaMeasure).texts.map((t) => t.text);
  assert.ok(!texts(show).some((t) => t.includes('each')));
  assert.ok(texts({ ...show, price: true, lot_price: true }).includes('$0.0696 each · Lot $69.60'));
});

test('price switches default off in the report options', () => {
  assert.deepEqual(opts.normalizeOptions(null).labels, { price: false, lot_price: false });
  assert.deepEqual(opts.normalizeOptions({ labels: { price: true, lot_price: 'yes' } }).labels, { price: true, lot_price: false });
});

test('the price switches carry over to the reports', async () => {
  const pivot = records.pivotRows(rows);
  const priced = records.addPrices(pivot, pivot.rows).records;
  const on = opts.normalizeOptions({ labels: { price: true, lot_price: true } });
  const { reportCsv, pricing } = await import('../../static/js/reports.js');
  assert.deepEqual(pricing(opts.normalizeOptions(null), priced).each, false);
  // Off (the default): the CSVs are as before.
  assert.equal(reportCsv('lots', priced, opts.normalizeOptions(null)).split('\r\n')[0], 'person,lot_count,total_pieces');
  const lots = reportCsv('lots', priced, on).split('\r\n');
  assert.deepEqual(lots.slice(0, 3), ['person,lot_count,total_pieces,total_price', 'Ann Lee,1,1000,69.60', 'Bob Roe,2,5,0.21']);
  const parts = reportCsv('parts', priced, on, 'sheet').split('\r\n');
  assert.ok(parts[0].endsWith(',price_each,total_price'));
  assert.ok(parts[1].startsWith('1,3001,') && parts[1].endsWith(',0.0696,69.81'));
  assert.ok(parts[2].endsWith(',,'));
});

test('amounts get thousands separators, except in CSVs', async () => {
  const { centsText } = await import('../../static/js/reports.js');
  assert.equal(centsText(3828, '$'), '$3,828.00');
  assert.equal(centsText(1234567.891, '$'), '$1,234,567.89');
  assert.equal(centsText(-0.29, '$'), '-$0.29');
  assert.equal(centsText(3828, '', true), '3828.00');
});

test('wrapping breaks at spaces, then at a hyphen, then (last) mid-word', async () => {
  const { wrapLines } = await import('../../static/js/labels.js');
  const w = (t) => t.length; // one unit per character
  assert.deepEqual(wrapLines('Chris Madeup-Longername', 17, w), ['Chris', 'Madeup-Longername']);
  assert.deepEqual(wrapLines('Chris Madeup-Longername', 12, w), ['Chris', 'Madeup-', 'Longername']);
  assert.deepEqual(wrapLines('BL: Trans-Clear', 11, w), ['BL:', 'Trans-Clear']); // a space break first
  assert.deepEqual(wrapLines('ABCDEFGHIJ', 4, w), ['ABCD', 'EFGH', 'IJ']);
});

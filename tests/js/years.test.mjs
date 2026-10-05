// Tests for static/js/years.js (the "Compare years" section) and the price
// column reader in pivot.js. All names and orders here are made up.
// `node --test tests/js/`

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parsePrice, unitPrices } from '../../static/js/pivot.js';
import * as years from '../../static/js/years.js';

// The "qty marker" layout with a "Cost\nEach" price column, like the live sheets.
const markerSheet = (rows) => [
  ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Cost\nEach', 'Total', 'Ann Lee', '', 'Bob Roe', ''],
  ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$'],
  ...rows,
];
// The "name/cost pair" layout with reference prices beside the real one.
const pairSheet = (rows) => [
  ['', '', '', '', '', '', '', '10'],
  ['Part Number', 'Description', 'BL Color', 'BL Price', 'B&P Price', 'Price', 'Notes', 'ann  lee', '4.00', 'Cy Doe', '1.00'],
  [],
  ...rows,
];

test('parsePrice', () => {
  assert.equal(parsePrice('$0.0696'), 0.0696);
  assert.equal(parsePrice(' 1,234.5 '), 1234.5);
  assert.equal(parsePrice('-$7.83'), -7.83);
  assert.equal(parsePrice('€2'), 2);
  assert.equal(parsePrice(''), null);
  assert.equal(parsePrice('n/a'), null);
  assert.equal(parsePrice('1.2.3'), null);
});

test('unitPrices finds the paid price, not the reference prices', () => {
  const live = unitPrices(markerSheet([
    ['1', '3001', '', 'BRICK 2X4', 'Red', '$0.10', '', '5', '', '', ''],
    ['2', '3003', '', 'BRICK 2X2', 'Red', '', '', '', '', '2', ''],
    ['', 'Shipping', '', '', '', '$40.00', '', '', '', '', ''],
  ]));
  assert.equal(live.column, 'Cost Each');
  assert.deepEqual([...live.prices], [['3001', 0.1]]);

  const master = unitPrices(pairSheet([
    ['3001', 'BRICK 2X4', 'Red', '$0.30', '0.25', '0.12', '', '5', '', '', ''],
  ]));
  assert.equal(master.column, 'Price');
  assert.deepEqual([...master.prices], [['3001', 0.12]]);

  assert.equal(unitPrices([['Element ID', 'Description'], [], ['3001', 'X']]).column, null);
});

test('yearFromName and parseYear', () => {
  assert.equal(years.yearFromName('LUGbulk2026_master.xlsx'), 2026);
  assert.equal(years.yearFromName('Copy of 2024 LUGBULK order.csv'), 2024);
  assert.equal(years.yearFromName('orders 12024.csv'), null);
  assert.equal(years.yearFromName('orders.xlsx'), null);
  assert.equal(years.parseYear(' 2023 '), 2023);
  assert.equal(years.parseYear('23'), null);
  assert.equal(years.parseYear('3000'), null);
});

const y2025 = years.readYear([markerSheet([
  ['1', '3001', '', 'BRICK 2X4', 'Red', '$0.10', '', '5', '', '10', ''],
  ['2', '3003', '', 'BRICK 2X2', 'Red', '$0.08', '', '', '', '2', ''],
  ['', 'Fees', '', '', '', '$12.00', '', '1', '', '', ''],
])], 'ArkLUG 2025.xlsx');
const y2026 = years.readYear([pairSheet([
  ['3001', 'BRICK 2X4', 'Red', '$0.30', '', '0.14', '', '20', '', '', ''],
  ['3020', 'PLATE 2X4', 'Blue', '', '', '', '', '', '', '3', ''],
])], 'LUGbulk2026_master.xlsx');

test('readYear', () => {
  assert.equal(y2025.year, 2025);
  assert.equal(y2025.priceColumn, 'Cost Each');
  assert.deepEqual(y2025.records.map((r) => [r.person, r.element_id, r.qty]), [
    ['Ann Lee', '3001', 5],
    ['Bob Roe', '3001', 10],
    ['Bob Roe', '3003', 2],
  ]);
  assert.equal(y2026.year, 2026);
  assert.equal(y2026.priceColumn, 'Price');
});

test('combine matches people by name, ignoring case and spacing', () => {
  const data = years.combine([y2026, y2025]);
  assert.deepEqual(data.years, [2025, 2026]);
  // "ann  lee" (2026) is Ann Lee; the latest spelling is shown.
  assert.deepEqual(data.people.map((p) => [p.name, p.spellings]), [
    ['Cy Doe', ['Cy Doe']],
    ['ann  lee', ['Ann Lee', 'ann  lee']],
    ['Bob Roe', ['Bob Roe']],
  ]);
  assert.equal(data.entries.length, 5);
});

test('merges, and sheets without a year are left out', () => {
  const merges = { [years.personKey('Cy Doe')]: years.personKey('Bob Roe') };
  const data = years.combine([y2025, y2026, { ...y2026, year: null }], merges);
  assert.deepEqual(data.people.map((p) => p.name), ['ann  lee', 'Bob Roe']);
  assert.deepEqual(data.people[1].spellings, ['Bob Roe', 'Cy Doe']);
  assert.equal(years.mergedKey('a', { a: 'b', b: 'a' }), 'a'); // a loop ends
});

test('duplicateYears and nameSuggestions', () => {
  assert.deepEqual(years.duplicateYears([y2025, y2025, y2026, { year: null }]), [2025]);
  const people = ['Ann-Marie Lee', 'Annmarie Lee', 'B. Roe', 'Bob Roe', 'Robert Roe', 'Katherine Smith', 'Katharine Smith']
    .map((name) => ({ key: years.personKey(name), name }));
  assert.deepEqual(years.nameSuggestions(people), [
    ['Ann-Marie Lee', 'Annmarie Lee'],
    ['B. Roe', 'Bob Roe'],
    ['Katherine Smith', 'Katharine Smith'],
  ]);
});

test('views', () => {
  const data = years.combine([y2025, y2026]);
  const summary = years.peopleSummary(data);
  const ann = summary.find((t) => t.person === 'ann  lee');
  assert.deepEqual(ann.years, { 2025: { lots: 1, pieces: 5, spent: 0.5 }, 2026: { lots: 1, pieces: 20, spent: 2.8 } });
  assert.equal(ann.spent, 3.3);
  const cy = summary.find((t) => t.person === 'Cy Doe');
  assert.equal(cy.unpriced, 1);

  assert.deepEqual(years.personInventory(data, 'ann  lee'), [
    { element_id: '3001', description: 'BRICK 2X4', color: 'Red', qty: { 2025: 5, 2026: 20 }, total: 25, spent: 3.3 },
  ]);

  assert.deepEqual(years.pricesByYear(data).map((r) => [r.element_id, r.year, r.price, r.qty, r.people]), [
    ['3003', 2025, 0.08, 2, 1],
    ['3001', 2025, 0.1, 15, 2],
    ['3001', 2026, 0.14, 20, 1],
    ['3020', 2026, null, 3, 1],
  ]);

  // A plain average of the yearly prices: 15 bought at 0.10 and 20 at 0.14
  // still average 0.12.
  const avg = years.averagePrices(data);
  assert.deepEqual(avg.map((r) => [r.element_id, r.prices, r.average, r.low, r.high, r.qty]), [
    ['3003', { 2025: 0.08 }, 0.08, 0.08, 0.08, 2],
    ['3001', { 2025: 0.1, 2026: 0.14 }, 0.12, 0.1, 0.14, 35],
    ['3020', {}, null, null, null, 3],
  ]);
});

test('CSVs', () => {
  const data = years.combine([y2025, y2026]);
  assert.equal(years.formatMoney(0.0696), '0.0696');
  assert.equal(years.formatMoney(12.5), '12.50');
  assert.equal(years.formatMoney(null), '');
  assert.equal(
    years.averagePricesCsv(data).split('\r\n')[2],
    '3001,BRICK 2X4,Red,0.10,0.14,0.12,0.10,0.14,35',
  );
  assert.equal(years.pricesByYearCsv(data).split('\r\n')[4], '3020,PLATE 2X4,Blue,2026,,3,1');
  assert.equal(years.peopleCsv(data).split('\r\n')[0],
    'person,2025 lots,2025 pieces,2025 spent,2026 lots,2026 pieces,2026 spent,lots,pieces,spent');
  assert.equal(years.inventoryCsv(data).split('\r\n')[1], 'Cy Doe,2026,3020,PLATE 2X4,Blue,3,,');
});

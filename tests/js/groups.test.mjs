// Tests for static/js/groups.js (families packed as one), its
// place in the report options, and groups in Compare years. All names and
// orders here are made up. `node --test tests/js/`

import assert from 'node:assert/strict';
import { test } from 'node:test';

import * as groups from '../../static/js/groups.js';
import { pivotSheet } from '../../static/js/pivot.js';
import * as opts from '../../static/js/report_options.js';
import * as reports from '../../static/js/reports.js';
import * as years from '../../static/js/years.js';

test('defaultName', () => {
  assert.equal(groups.defaultName(['Ann Lee', 'Bob Lee']), 'Lee family');
  assert.equal(groups.defaultName(['Ann Lee', 'Bob Lee', 'Cy lee']), 'Lee family');
  assert.equal(groups.defaultName(['Ann Lee', 'Cy Doe']), 'Ann Lee & Cy Doe');
  assert.equal(groups.defaultName(['Ann Lee', 'Bob Roe', 'Cy Doe']), 'Ann Lee, Bob Roe & Cy Doe');
  assert.equal(groups.defaultName(['Ann', 'Bob']), 'Ann & Bob');
});

test('normalizeGroups', () => {
  assert.deepEqual(groups.normalizeGroups(null), { combine: true, list: [] });
  const g = groups.normalizeGroups({
    combine: false,
    list: [
      { name: '  The\nLees ', members: ['Ann Lee', ' ann  lee', 'Bob Lee'] },
      { members: ['Bob Lee', 'Cy Doe'] }, // Bob is taken: one member left, dropped
      { name: '', members: ['Cy Doe', 'Dee Doe', 42] },
      'junk',
    ],
    extra: 1,
  });
  assert.deepEqual(g, {
    combine: false,
    list: [
      { name: 'The Lees', members: ['Ann Lee', 'Bob Lee'] },
      { name: 'Doe family', members: ['Cy Doe', 'Dee Doe'] },
    ],
  });
  const many = { list: Array.from({ length: 60 }, (_, i) => ({ members: [`A${i}`, `B${i}`] })) };
  assert.equal(groups.normalizeGroups(many).list.length, groups.MAX_GROUPS);
});

const sheet = pivotSheet([
  ['#', 'Element ID', 'Photo', 'Description', 'BL Color', 'Cost', 'Total', 'Ann Lee', '', 'Bob Lee', '', 'Cy Doe', ''],
  ['', '', '', '', '', '', '', 'qty', '$$', 'qty', '$$', 'qty', '$$'],
  ['1', '3001', '', 'BRICK 2X4', 'Red', '', '', '10', '', '5', '', '1', ''],
  ['2', '3003', '', 'BRICK 2X2', 'Red', '', '', '', '', '2.5', '', '', ''],
  ['3', '3020', '', 'PLATE 2X4', 'Red', '', '', '4', '', '', '', '3', ''],
]).records;

test('applyGroups packs a group as one person', () => {
  const g = groups.normalizeGroups({ list: [{ name: 'The Lees', members: ['ann lee', 'Bob Lee'] }] });
  const out = groups.applyGroups(sheet, g);
  assert.deepEqual(out.map((r) => [r.person, r.element_id, r.qty]), [
    ['The Lees', '3001', '15'],
    ['Cy Doe', '3001', '1'],
    ['The Lees', '3003', '2.5'],
    ['The Lees', '3020', '4'],
    ['Cy Doe', '3020', '3'],
  ]);
  assert.equal(out[0].description, 'BRICK 2X4');
  // Lot counts follow: the group is one person.
  assert.deepEqual(reports.lotCountsByPerson(out).map((t) => [t.person, t.lot_count, t.total_pieces]), [
    ['Cy Doe', 2, 4],
    ['The Lees', 3, 21.5],
  ]);
  // Off, or no groups: the records as they were.
  assert.equal(groups.applyGroups(sheet, { ...g, combine: false }), sheet);
  assert.equal(groups.applyGroups(sheet, groups.normalizeGroups(null)), sheet);
});

test('groups in the report options, within the size limit', () => {
  const g = { combine: true, list: [{ name: 'The Lees', members: ['Ann Lee', 'Bob Lee'] }] };
  assert.deepEqual(opts.normalizeOptions({ groups: g }).groups, g);
  assert.deepEqual(opts.normalizeOptions(null).groups, { combine: true, list: [] });
  // Too many long names to fit: the last groups are dropped.
  const long = (i, j) => `${'\u{1F9F1}'.repeat(50)} ${i}-${j}`;
  const big = { list: Array.from({ length: 40 }, (_, i) => ({ name: long(i, 0), members: [long(i, 1), long(i, 2)] })) };
  const o = opts.normalizeOptions({ groups: big });
  assert.ok(o.groups.list.length > 0 && o.groups.list.length < 40);
  assert.ok(opts.optionsBytes(o) <= opts.MAX_OPTIONS_BYTES);
});

test('groups in Compare years', () => {
  const y = (year, rows) => ({ ...years.readYear([[
    ['Element ID', 'Description', 'BL Color', 'Price', 'Ann Lee', '1', 'Bob Lee', '1', 'Cy Doe', '1'],
    [],
    ...rows,
  ]], `x ${year}.csv`) });
  const sheets = [
    y(2025, [['3001', 'BRICK 2X4', 'Red', '0.10', '10', '', '5', '', '1', '']]),
    y(2026, [['3001', 'BRICK 2X4', 'Red', '0.20', '', '', '1', '', '', '']]),
  ];
  const g = groups.normalizeGroups({ list: [{ name: 'The Lees', members: ['Ann Lee', 'Bob Lee'] }] });
  const data = years.combine(sheets, {}, g);
  assert.deepEqual(data.people.map((p) => [p.name, p.spellings, !!p.group]), [
    ['Cy Doe', ['Cy Doe'], false],
    ['The Lees', ['Ann Lee', 'Bob Lee'], true],
  ]);
  const lees = years.peopleSummary(data).find((t) => t.person === 'The Lees');
  assert.deepEqual(lees.years, { 2025: { lots: 1, pieces: 15, spent: 1.5 }, 2026: { lots: 1, pieces: 1, spent: 0.2 } });
  // Shown as separate people when the viewer turns combining off.
  assert.equal(years.combine(sheets, {}, { ...g, combine: false }).people.length, 3);
  // Members are matched after merges.
  const merged = years.combine(sheets, { [years.personKey('Cy Doe')]: years.personKey('Bob Lee') }, g);
  assert.deepEqual(merged.people.map((p) => p.name), ['The Lees']);
});

// Parity test: the browser modules in static/js/ must give the same results
// as the C++ server on the same input. The C++ side's results come from
// tests/golden.cpp (`lugbulk_golden <dir>`, run by the Docker build and
// exported by CI); point GOLDEN_DIR at that directory:
//
//   GOLDEN_DIR=golden node --test tests/js/
//
// Without GOLDEN_DIR the parity tests are skipped (the unit tests in
// unit.test.mjs still run); in CI it is always set.
//
// What the C++ no longer makes (the CSV reports, how people sort in reports)
// is checked against tests/js/golden/reports.json, dumped from the C++ once
// before it was retired; labels.json there is checked by labels.test.mjs.

import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import * as colors from '../../static/js/colors.js';
import * as layout from '../../static/js/layout.js';
import * as ordering from '../../static/js/ordering.js';
import * as pivot from '../../static/js/pivot.js';
import * as records from '../../static/js/records.js';
import * as reports from '../../static/js/reports.js';
import * as spreadsheet from '../../static/js/spreadsheet.js';

const dir = process.env.GOLDEN_DIR;
const skip = dir ? false : 'GOLDEN_DIR not set (see tests/golden.cpp)';

// Through JSON and back, as a page would send/receive it: undefined and
// non-finite numbers become null, -0 becomes 0, key order stops mattering.
const canon = (v) => JSON.parse(JSON.stringify(v));
const load = (name) => JSON.parse(readFileSync(join(dir, name), 'utf8'));
const withoutImage = (rs) => rs.map(({ image_url, ...r }) => r);

const fixtures = new URL('../fixtures/', import.meta.url);
const expectedReports = JSON.parse(readFileSync(new URL('./golden/reports.json', import.meta.url), 'utf8'));

const cases = dir ? readdirSync(dir).filter((f) => /^case-.*\.json$/.test(f)).sort() : [];

test('golden files are present', { skip }, () => {
  assert.ok(cases.length >= 4, `expected golden cases in ${dir}, found ${cases.length}`);
  assert.ok(readdirSync(dir).includes('units.json'));
});

for (const file of cases) {
  test(`pivot + records + ordering match C++, reports match the fixture: ${file}`, { skip }, () => {
    const g = load(file);
    const want_reports = expectedReports.cases[file.replace(/^case-|\.json$/g, '')];

    // pivot_tabs without the size check.
    let p = { records: [], issues: [] };
    for (const rows of g.tabs) {
      p = pivot.pivotSheet(records.capRows(rows));
      if (p.records.length) break;
    }
    for (const r of p.records) assert.equal(r.image_url, `/img/${r.element_id}.jpg`);
    assert.deepEqual(canon({ ...p, records: withoutImage(p.records) }), canon(g.pivot));

    if (g.too_big !== null) {
      assert.equal(want_reports, undefined);
      assert.throws(() => records.pivotTabs(g.tabs), (e) => {
        assert.ok(e instanceof records.TooBigError);
        assert.equal(e.message, g.too_big);
        return true;
      });
      return;
    }
    assert.deepEqual(canon(withoutImage(records.pivotTabs(g.tabs).records)), canon(g.pivot.records));

    assert.deepEqual(records.elementIds(p), g.lookup_ids);
    const applied = records.applyBricklink(p, g.lookup);
    assert.deepEqual(canon({ ...applied, records: withoutImage(applied.records) }), canon(g.applied));
    assert.deepEqual(canon(records.checkSummary(applied)), canon(g.check));

    for (const [order, want] of Object.entries(g.orders)) {
      assert.deepEqual(
        canon(withoutImage(ordering.orderRecords(applied.records, order))),
        canon(want.records),
        `order_records ${order}`,
      );
      assert.deepEqual(
        canon(ordering.summarizeParts(applied.records, order)),
        canon(want.parts),
        `summarize_parts ${order}`,
      );
      // CSV reports: byte for byte.
      const parts_csv = want_reports.parts_csv[order];
      assert.equal(reports.partsCsv(ordering.summarizeParts(applied.records, order)), parts_csv, `parts_csv ${order}`);
      assert.equal(reports.reportCsv('parts', applied.records, null, order), parts_csv, `reportCsv parts ${order}`);
    }
    assert.equal(reports.lotCountsCsv(applied.records, 'last'), want_reports.lots_csv_last, 'lot_counts_csv last');
    assert.equal(reports.lotCountsCsv(applied.records, 'first'), want_reports.lots_csv_first, 'lot_counts_csv first');
    assert.equal(reports.reportCsv('lots', applied.records, null), want_reports.lots_csv_last, 'reportCsv lots');
    assert.equal(reports.sheetCheckText(applied), g.reports.check_text, 'sheet check text');
  });
}

test('how people sort matches the fixture (reports.json)', () => {
  assert.ok(expectedReports.people.length >= 10);
  assert.deepEqual([...expectedReports.people].sort(ordering.comparePeople), expectedReports.people_sorted);
});

test('colors, parse_qty, element ids, weights, names, label specs match C++', { skip }, () => {
  const u = load('units.json');

  for (const c of u.colors) {
    const r = colors.resolve(c.lego_in, c.bl_in);
    assert.deepEqual(
      canon({
        lego: r.lego,
        bl: r.bl,
        mapped: r.mapped,
        transparent: colors.isTransparent(c.lego_in, c.bl_in),
        light: colors.isLight(c.lego_in, c.bl_in),
        swatch: colors.swatchRgb(c.lego_in, c.bl_in),
      }),
      canon({
        lego: c.lego,
        bl: c.bl,
        mapped: c.mapped,
        transparent: c.transparent,
        light: c.light,
        swatch: c.swatch,
      }),
      `colors for ${JSON.stringify([c.lego_in, c.bl_in])}`,
    );
  }

  for (const [input, want] of u.parse_qty) {
    let got;
    try {
      const v = pivot.parseQty(input);
      got = Number.isNaN(v) ? 'nan' : v === Infinity ? 'inf' : v === -Infinity ? '-inf' : v;
    } catch {
      got = 'error';
    }
    assert.deepEqual(canon(got), canon(want), `parseQty(${JSON.stringify(input)})`);
  }

  for (const [id, want] of u.element_ids) {
    assert.equal(pivot.isValidElementId(id), want, `isValidElementId(${JSON.stringify(id)})`);
  }

  for (const [desc, want] of u.estimate_weight) {
    assert.deepEqual(canon(ordering.estimateWeight(desc)), canon(want), `estimateWeight(${JSON.stringify(desc)})`);
  }

  const find = layout.labelSpecFinder(u.label_specs);
  for (const [name, want] of u.label_spec_lookups) {
    assert.equal(find(name)?.id ?? null, want, `labelSpecFinder(${JSON.stringify(name)})`);
  }
  assert.equal(u.label_specs.default, layout.DEFAULT_LABEL_SPEC_ID);

  assert.deepEqual(
    u.part_orders.map((o) => ordering.parsePartOrder(o) !== null),
    u.part_orders_valid,
  );
});

test('spreadsheet reading (.xlsx, .csv) matches C++', { skip }, async () => {
  const reads = load('spreadsheets.json');
  const bomb = reads.find((c) => c.file === 'bomb.xlsx');
  assert.equal(bomb?.error, 'That .xlsx file unpacks to more than 64 MB — too big.');
  assert.ok(reads.filter((c) => c.file).length >= 4, 'every fixture file is read');
  assert.ok(reads.filter((c) => c.hex !== undefined).length >= 20, 'the generated inputs are there');
  for (const c of reads) {
    const bytes = c.file ? readFileSync(new URL(c.file, fixtures)) : Buffer.from(c.hex, 'hex');
    let got;
    try {
      got = { tabs: await spreadsheet.readTabs(bytes, layout.SOURCE_TAB) };
    } catch (e) {
      if (!(e instanceof spreadsheet.SpreadsheetError)) throw e;
      got = { error: e.message };
    }
    const want = 'error' in c ? { error: c.error } : { tabs: c.tabs };
    assert.deepEqual(got, want, c.name);
  }
});

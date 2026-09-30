// "Keep each part on one sheet" (static/js/packing.js). All data invented.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  flowSummary, normalizeKeep, packRecords, packSizes, partOrderOf, slotsOf, splitParts,
} from '../../static/js/packing.js';

// A small deterministic generator.
function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 4294967296;
  };
}
const between = (r, lo, hi) => lo + Math.floor(r() * (hi - lo + 1));

// Records for parts of the given label counts: part k has element id 1000+k.
function recordsFor(counts) {
  const out = [];
  counts.forEach((n, k) => {
    for (let i = 0; i < n; ++i) {
      out.push({ person: `Person ${i + 1}`, element_id: String(1000 + k), description: `PART ${k}`, qty: '1', part_seq: i + 1, part_total: n });
    }
  });
  return out;
}

// The fewest bins, by dynamic programming over subsets (for small instances).
function bruteForce(sizes, C) {
  const m = sizes.length;
  const full = (1 << m) - 1;
  const sum = new Array(1 << m).fill(0);
  for (let mask = 1; mask <= full; ++mask) {
    const low = mask & -mask;
    sum[mask] = sum[mask ^ low] + sizes[31 - Math.clz32(low)];
  }
  const best = new Array(1 << m).fill(Infinity);
  best[0] = 0;
  for (let mask = 1; mask <= full; ++mask) {
    const low = mask & -mask; // the lowest item is in the bin removed
    for (let sub = mask; sub > 0; sub = (sub - 1) & mask) {
      if ((sub & low) && sum[sub] <= C && best[mask ^ sub] + 1 < best[mask]) best[mask] = best[mask ^ sub] + 1;
    }
  }
  return best[full];
}

function firstFitDecreasing(sizes, C) {
  const left = [];
  for (const s of [...sizes].sort((a, b) => b - a)) {
    const b = left.findIndex((l) => l >= s);
    if (b < 0) left.push(C - s);
    else left[b] -= s;
  }
  return left.length;
}

// What a layout must satisfy whatever the instance: every record exactly
// once, whole sheets, no part split that needn't be.
function checkLayout(records, per, res) {
  const { layout } = res;
  assert.equal(layout.length % per, 0);
  assert.equal(layout.length / per, res.sheets);
  assert.equal(res.blanks, layout.length - records.length);
  const seen = layout.filter((i) => i >= 0);
  assert.equal(new Set(seen).size, records.length);
  assert.equal(seen.length, records.length);
  const slots = slotsOf(records, layout);
  const counts = new Map();
  for (const r of records) counts.set(r.element_id, (counts.get(r.element_id) || 0) + 1);
  for (const [id, n] of counts) {
    const sheets = new Set();
    slots.forEach((r, i) => { if (r && r.element_id === id) sheets.add(Math.floor(i / per)); });
    assert.equal(sheets.size, Math.ceil(n / per), `part ${id} (${n} labels) spans ${sheets.size} sheets`);
  }
  return slots;
}

test('no part that fits on one sheet is ever split; oversized parts use the minimum sheets', () => {
  const r = rng(1);
  for (let t = 0; t < 300; ++t) {
    const per = between(r, 2, 14);
    const counts = Array.from({ length: between(r, 1, 14) }, () => (r() < 0.2 ? between(r, per + 1, per * 3 + 2) : between(r, 1, per)));
    const records = recordsFor(counts);
    const res = packRecords(records, per);
    const slots = checkLayout(records, per, res);
    assert.equal(splitParts(slots, per), 0);
    // Whole sheets of a big part are its own, from the top of a fresh sheet.
    counts.forEach((n, k) => {
      if (n <= per) return;
      const id = String(1000 + k);
      let whole = 0;
      for (let s = 0; s < res.sheets; ++s) {
        const sheet = slots.slice(s * per, (s + 1) * per);
        if (sheet.every((x) => x && x.element_id === id)) ++whole;
      }
      assert.equal(whole, Math.floor(n / per), `part ${id}`);
    });
  }
});

test('the number of sheets is the optimum: equal to a brute-force search', () => {
  const r = rng(2);
  let needed = 0;
  for (let t = 0; t < 400; ++t) {
    const C = between(r, 4, 16);
    const sizes = Array.from({ length: between(r, 1, 11) }, () => between(r, 1, C));
    const res = packSizes(sizes, C);
    assert.ok(res.proven);
    assert.equal(res.bins.length, bruteForce(sizes, C), JSON.stringify({ C, sizes }));
    assert.equal(res.bins.flat().length, sizes.length);
    for (const bin of res.bins) assert.ok(bin.reduce((a, i) => a + sizes[i], 0) <= C);
    if (firstFitDecreasing(sizes, C) > res.bins.length) ++needed;
  }
  assert.ok(needed > 0, 'some instances beat first-fit decreasing');
});

test('never more sheets than first-fit decreasing', () => {
  const r = rng(3);
  for (let t = 0; t < 60; ++t) {
    const C = between(r, 8, 40);
    const sizes = Array.from({ length: between(r, 20, 120) }, () => between(r, 1, C));
    const res = packSizes(sizes, C, { timeBoxMs: 300 });
    assert.ok(res.bins.length <= firstFitDecreasing(sizes, C));
    assert.ok(res.bins.length >= res.lowerBound);
    for (const bin of res.bins) assert.ok(bin.reduce((a, i) => a + sizes[i], 0) <= C);
  }
});

test('deterministic: the same input gives the same slots', () => {
  const r = rng(4);
  for (let t = 0; t < 12; ++t) {
    const per = between(r, 6, 30);
    const records = recordsFor(Array.from({ length: between(r, 10, 40) }, () => between(r, 1, per + 3)));
    const a = packRecords(records, per, { timeBoxMs: 60000 });
    const b = packRecords(records, per, { timeBoxMs: 60000 });
    assert.deepEqual(a, b);
  }
});

test('time box: a big hard instance returns in time with a valid, unproven-or-proven result', () => {
  const r = rng(5);
  const per = 40;
  const counts = Array.from({ length: 900 }, () => between(r, 7, 19));
  const records = recordsFor(counts);
  const t0 = Date.now();
  const res = packRecords(records, per, { timeBoxMs: 200 });
  const took = Date.now() - t0;
  assert.ok(took < 1500, `took ${took} ms for a 200 ms box`);
  const slots = checkLayout(records, per, res);
  assert.equal(splitParts(slots, per), 0);
  assert.ok(res.sheets >= res.lowerBound);
  assert.ok(res.sheets <= firstFitDecreasing(counts, per));
});

test('when the time box expires the best packing found is returned, not proven optimal', () => {
  // Needs a real search: first-fit decreasing uses 6 sheets, the optimum is 5.
  const sizes = [7, 10, 8, 8, 7, 12, 8, 19, 12, 14, 15, 11, 7];
  const expired = packSizes(sizes, 28, { timeBoxMs: -1, now: () => 0 });
  assert.equal(expired.proven, false);
  assert.equal(expired.bins.length, firstFitDecreasing(sizes, 28));
  assert.equal(expired.bins.flat().length, sizes.length);
  for (const bin of expired.bins) assert.ok(bin.reduce((a, i) => a + sizes[i], 0) <= 28);
  const done = packSizes(sizes, 28);
  assert.equal(done.proven, true);
  assert.equal(done.bins.length, 5);
  assert.equal(done.bins.length, bruteForce(sizes, 28));
  // Through packRecords too.
  const records = recordsFor(sizes);
  const res = packRecords(records, 28, { timeBoxMs: -1, now: () => 0 });
  assert.equal(res.proven, false);
  checkLayout(records, 28, res);
});

test('among minimum packings, the one closest to the design order', () => {
  // Already packs perfectly in order: nothing moves.
  const inOrder = recordsFor([5, 5, 4, 6, 3, 7]);
  const res = packRecords(inOrder, 10);
  assert.deepEqual(res.layout, inOrder.map((_, i) => i));
  assert.equal(res.sheets, 3);
  assert.equal(res.blanks, 0);

  // 6 | 6 | 4 | 4: 6+4 twice is the optimum. Parts: A(6) B(6) C(4) D(4), so
  // sheet 1 = A+C, sheet 2 = B+D — the earliest parts together, sheets
  // ordered by their first part, parts in design order within a sheet.
  const recs = recordsFor([6, 6, 4, 4]);
  const r2 = packRecords(recs, 10);
  const slots = slotsOf(recs, r2.layout);
  assert.deepEqual(slots.map((s) => s && s.element_id), [
    ...Array(6).fill('1000'), ...Array(4).fill('1002'),
    ...Array(6).fill('1001'), ...Array(4).fill('1003'),
  ]);
  assert.equal(r2.sheets, 2);
  assert.equal(r2.blanks, 0);
  assert.deepEqual(partOrderOf(slots), ['1000', '1002', '1001', '1003']);
});

test('sheets are ordered by their first part and parts keep the design order inside one', () => {
  const r = rng(6);
  for (let t = 0; t < 100; ++t) {
    const per = between(r, 4, 12);
    const counts = Array.from({ length: between(r, 2, 12) }, () => between(r, 1, per));
    const records = recordsFor(counts);
    const slots = slotsOf(records, packRecords(records, per).layout);
    const rank = (x) => Number(x.element_id) - 1000;
    let lastFirst = -1;
    for (let s = 0; s * per < slots.length; ++s) {
      const sheet = slots.slice(s * per, (s + 1) * per).filter(Boolean);
      const parts = [...new Set(sheet.map(rank))];
      assert.deepEqual(parts, [...parts].sort((a, b) => a - b));
      assert.ok(parts[0] > lastFirst, 'sheets in order of first part');
      lastFirst = parts[0];
      // Empty slots are at the end of a sheet.
      const cells = slots.slice(s * per, (s + 1) * per);
      const firstBlank = cells.indexOf(null);
      if (firstBlank >= 0) assert.ok(cells.slice(firstBlank).every((x) => x === null));
    }
  }
});

test('an oversized part fills whole sheets; only its remainder is packed', () => {
  // 25 labels on 10-slot sheets: two whole sheets, a remainder of 5 that shares with a 5-part.
  const records = recordsFor([25, 4, 5]);
  const res = packRecords(records, 10);
  const slots = slotsOf(records, res.layout);
  assert.equal(res.sheets, 4);
  assert.deepEqual(slots.slice(0, 20).map((s) => s.element_id), Array(20).fill('1000'));
  assert.equal(res.blanks, 40 - 34);
  checkLayout(records, 10, res);
  assert.equal(splitParts(slots, 10), 0);
  // The same instance flowing on: the 4-part is split.
  assert.deepEqual(flowSummary(records, 10), { sheets: 4, blanks: 6, splitParts: 1 });
});

test('single-label stocks and empty input are left alone', () => {
  const records = recordsFor([3, 2]);
  assert.deepEqual(packRecords(records, 1).layout, [0, 1, 2, 3, 4]);
  assert.deepEqual(packRecords([], 10).layout, []);
  assert.equal(packRecords([], 10).sheets, 0);
});

test('flow summary: sheets, blanks and split parts of the continuous flow', () => {
  const records = recordsFor([6, 6, 4, 4, 7]);
  const f = flowSummary(records, 10);
  assert.equal(f.sheets, 3);
  assert.equal(f.blanks, 30 - 27);
  assert.equal(f.splitParts, 1); // the second part (6 labels from slot 6)
  const packed = packRecords(records, 10);
  assert.equal(splitParts(slotsOf(records, packed.layout), 10), 0);
});

test('normalizeKeep', () => {
  assert.equal(normalizeKeep('optimize'), 'optimize');
  for (const v of ['off', '', undefined, null, 'Optimize', 7, {}]) assert.equal(normalizeKeep(v), 'off');
});

test('"same as the labels" follows the packed order in the reports', async () => {
  const { orderedParts } = await import('../../static/js/reports.js');
  const { orderRecords, summarizeParts } = await import('../../static/js/ordering.js');
  const records = recordsFor([6, 6, 4, 4]);
  const ordered = orderRecords(records, 'sheet');
  const slots = slotsOf(ordered, packRecords(ordered, 10).layout);
  const order = partOrderOf(slots);
  assert.deepEqual(order, ['1000', '1002', '1001', '1003']);
  assert.deepEqual(summarizeParts(records, order).map((p) => p.element_id), order);
  assert.deepEqual(orderedParts(records, 'labels', order).map((p) => p.element_id), order);
  assert.deepEqual(orderRecords(records, order).map((r) => r.element_id).filter((x, i, a) => a.indexOf(x) === i), order);
  // A part the order doesn't list goes last, in sheet order.
  assert.deepEqual(summarizeParts(records, ['1003']).map((p) => p.element_id), ['1003', '1000', '1001', '1002']);
});

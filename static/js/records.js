// What happens to a sheet's rows between reading them and rendering — the
// browser port of src/records.{h,cpp} and the server's fetch_and_pivot /
// pivot_upload: the row cap, pivoting, the per-run size limits, BrickLink
// data, and the "Check sheet" summary. Pure functions: fetching the rows
// (GET /sheets/:id/values) and the BrickLink data (POST /bricklink/lookup)
// is the caller's job.

import * as colors from './colors.js';
import { pivotSheet, unitPrices } from './pivot.js';

export const MAX_SHEET_ROWS = 3000; // rows read from the "Order Here" tab
export const MAX_LABELS = 20000; // labels in one run
export const MAX_PARTS = 2000; // distinct parts in one run (photo downloads)

// A sheet over the per-run size limits. The server answers these with
// HTTP 413 and "Too big: <message>."
export class TooBigError extends Error {
  constructor(message) {
    super(message);
    this.name = 'TooBigError';
  }
}

// Rows past MAX_SHEET_ROWS dropped.
export function capRows(rows) {
  return rows.length > MAX_SHEET_ROWS ? rows.slice(0, MAX_SHEET_ROWS) : rows;
}

// Distinct element ids, in record order: the body for POST /bricklink/lookup.
export function elementIds(pivot) {
  return [...new Set(pivot.records.map((r) => r.element_id))];
}

// Throws TooBigError if the pivot is over the per-run limits.
export function checkRunSize(pivot) {
  const parts = new Set(pivot.records.map((r) => r.element_id)).size;
  const labels = pivot.records.length;
  if (labels > MAX_LABELS || parts > MAX_PARTS) {
    throw new TooBigError(
      `this sheet has ${labels} labels and ${parts} parts; the limit is ` +
        `${MAX_LABELS} labels / ${MAX_PARTS} parts per run`,
    );
  }
}

// Pivots one tab's rows (e.g. GET /sheets/:id/values's `rows`), like the
// server's fetch_and_pivot before BrickLink data. Throws TooBigError.
export function pivotRows(rows) {
  return pivotTabs([rows]);
}

// Pivots an uploaded file's tabs, like the server's pivot_upload: each tab
// in turn until one has orders (a workbook's "Order Here" tab comes first;
// a CSV is one tab). Throws TooBigError. The result's `rows` are the tab's
// rows it used (for addPrices).
export function pivotTabs(tabs) {
  let pivot = { records: [], issues: [] };
  let used = [];
  for (const rows of tabs) {
    used = capRows(rows);
    pivot = pivotSheet(used);
    if (pivot.records.length) break;
  }
  checkRunSize(pivot);
  return { ...pivot, rows: used };
}

// Each record's price per piece from the sheet's price column (null where
// it has none) and the currency sign the sheet writes prices with:
// {records, issues} with `price` and `currency` on every record.
export function addPrices(pivot, rows) {
  const { prices, symbol } = unitPrices(rows);
  const records = pivot.records.map((r) => ({
    ...r,
    price: prices.has(r.element_id) ? prices.get(r.element_id) : null,
    currency: symbol,
  }));
  return { records, issues: pivot.issues };
}

// Adds BrickLink data from a POST /bricklink/lookup response
// ({"<id>": {part, color, weight|null}}): each record's catalog_weight, and
// BrickLink/LEGO color names the sheet left blank (clearing the
// "missing_color" issues that fixes). Returns a new pivot.
export function applyBricklink(pivot, lookup) {
  const colored = new Set();
  const records = pivot.records.map((r) => {
    const info = lookup && Object.hasOwn(lookup, r.element_id) ? lookup[r.element_id] : null;
    if (!info) return r;
    const out = { ...r, catalog_weight: info.weight ?? null };
    if (!out.bl_color && info.color) {
      out.bl_color = info.color;
      if (!out.lego_color) out.lego_color = colors.resolve('', out.bl_color).lego;
      colored.add(r.element_id);
    }
    return out;
  });
  // A color BrickLink supplied is no longer missing.
  const issues = pivot.issues.filter((i) => !(i.kind === 'missing_color' && colored.has(i.element_id)));
  return { records, issues };
}

// The "Check sheet" summary, as GET /sheets/:id/check and POST
// /upload/check return it: {labels, people, parts, issues:[{row, kind, detail}]}.
export function checkSummary(pivot) {
  return {
    labels: pivot.records.length,
    people: new Set(pivot.records.map((r) => r.person)).size,
    parts: new Set(pivot.records.map((r) => r.element_id)).size,
    issues: pivot.issues.map(({ row, kind, detail }) => ({ row, kind, detail })),
  };
}

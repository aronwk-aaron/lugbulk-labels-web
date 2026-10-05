// Several years' order sheets side by side — the "Compare years" section:
// who got what across the years, and what each part cost. Pure functions;
// reading the files (spreadsheet.js) or a saved sheet's cells (GET
// /sheets/:id/values) is the caller's job, and nothing here leaves the
// browser.
//
// A year is {name, year, records, prices, priceColumn, issues}: one sheet
// pivoted as for labels (pivot.js), plus the price paid per piece from its
// price column (pivot.unitPrices). Only parts with a LEGO element ID count,
// so fee, shipping and notes rows never do. People are matched across years
// by name (case and spacing ignored), plus the merges the user makes; parts
// by element ID.

import { groupOf, personKey } from './groups.js';
import { parseQty, pivotSheet, unitPrices } from './pivot.js';
import { capRows } from './records.js';
import { csvField, formatCount, formatMoney, personSortKey } from './reports.js';

export { formatMoney };
import { compareBytes, trim } from './text.js';

// The first plausible year (1990-2099) in a file or sheet name, else null.
export function yearFromName(name) {
  const m = /(?<![0-9])(199[0-9]|20[0-9][0-9])(?![0-9])/.exec(String(name ?? ''));
  return m ? Number(m[1]) : null;
}

// A year typed by the user: a whole number from 1990 to 2099, else null.
export function parseYear(text) {
  const t = trim(String(text ?? ''));
  if (!/^[0-9]{4}$/.test(t)) return null;
  const y = Number(t);
  return y >= 1990 && y <= 2099 ? y : null;
}

// One sheet's tabs (a workbook's, or a saved sheet's single tab) as a year:
// the first tab with orders, as for labels. `year` is from the name, or
// null for the user to fill in.
export function readYear(tabs, name) {
  let rows = [];
  let pivot = { records: [], issues: [] };
  for (const t of tabs) {
    rows = capRows(t);
    pivot = pivotSheet(rows);
    if (pivot.records.length) break;
  }
  const { column, prices } = unitPrices(rows);
  const records = pivot.records.map((r) => ({
    person: r.person,
    element_id: r.element_id,
    description: r.description,
    lego_color: r.lego_color,
    bl_color: r.bl_color,
    qty: parseQty(r.qty),
  }));
  return { name, year: yearFromName(name), records, prices, priceColumn: column, issues: pivot.issues };
}

// ---- people ----------------------------------------------------------------

// The key people are matched by: lower case, spacing collapsed.
export { personKey };

// Follows merges ({key: key it was merged into}) to the end of the chain.
export function mergedKey(key, merges = {}) {
  const seen = new Set();
  while (Object.hasOwn(merges, key) && !seen.has(key)) {
    seen.add(key);
    key = merges[key];
  }
  return key;
}

const comparePeople = (a, b) => {
  const ka = personSortKey(a);
  const kb = personSortKey(b);
  return compareBytes(ka[0], kb[0]) || compareBytes(ka[1], kb[1]) || compareBytes(a, b);
};

// ---- combining -------------------------------------------------------------

// The years as one data set: {years: [sorted], people: [{key, name,
// spellings}], entries: [{year, person, element_id, description, color,
// qty, price}]}. `person` is the display name: the latest spelling of the
// person a name was merged into (theirs, not the merged-in name's). Years without a year number
// are left out; two sheets for the same year are an error for the caller to
// show (duplicateYears) before this is used.
//
// With `groups` (groups.js) whose `combine` is on, each group shows as one
// person: its members' parts added up, under the group's name. Members are
// matched after merges.
export function combine(sheets, merges = {}, groups = null) {
  const usable = sheets.filter((s) => s.year !== null).sort((a, b) => a.year - b.year);
  const spelling = new Map(); // key -> latest spelling, its own over merged-in ones
  const spellings = new Map(); // key -> Set of spellings
  for (const s of usable) {
    for (const r of s.records) {
      const own = personKey(r.person);
      const k = mergedKey(own, merges);
      if (own === k || !spelling.has(k) || personKey(spelling.get(k)) !== k) spelling.set(k, trim(r.person));
      if (!spellings.has(k)) spellings.set(k, new Set());
      spellings.get(k).add(trim(r.person));
    }
  }
  const entries = [];
  for (const s of usable) {
    for (const r of s.records) {
      const price = s.prices.get(r.element_id);
      entries.push({
        year: s.year,
        person: spelling.get(mergedKey(personKey(r.person), merges)),
        element_id: r.element_id,
        description: r.description,
        color: r.bl_color || r.lego_color,
        qty: r.qty,
        price: price === undefined ? null : price,
      });
    }
  }
  let people = [...spelling.entries()]
    .map(([key, name]) => ({ key, name, spellings: [...spellings.get(key)].sort(compareBytes) }));
  const years = [...new Set(usable.map((s) => s.year))];
  const of = groupOf(groups, (m) => mergedKey(personKey(m), merges));
  if (!of.size) return { years, people: people.sort((a, b) => comparePeople(a.name, b.name)), entries };

  // Groups: one person each, their members' entries added up.
  const groupFor = new Map(); // display name -> group name
  const members = new Map(); // group name -> member display names
  for (const p of people) {
    const g = of.get(p.key);
    if (g === undefined) continue;
    groupFor.set(p.name, g);
    if (!members.has(g)) members.set(g, []);
    members.get(g).push(p.name);
  }
  const merged = new Map(); // JSON [year, person, element_id] -> entry
  const grouped = [];
  for (const e of entries) {
    const person = groupFor.get(e.person) ?? e.person;
    const k = JSON.stringify([e.year, person, e.element_id]);
    const at = merged.get(k);
    if (at) {
      at.qty += e.qty;
    } else {
      const g = { ...e, person };
      merged.set(k, g);
      grouped.push(g);
    }
  }
  people = people.filter((p) => !groupFor.has(p.name));
  for (const [name, names] of members) {
    people.push({ key: personKey(name), name, spellings: names.sort(compareBytes), group: true });
  }
  return { years, people: people.sort((a, b) => comparePeople(a.name, b.name)), entries: grouped };
}

// Years that more than one sheet claims.
export function duplicateYears(sheets) {
  const count = new Map();
  for (const s of sheets) if (s.year !== null) count.set(s.year, (count.get(s.year) || 0) + 1);
  return [...count].filter(([, n]) => n > 1).map(([y]) => y).sort((a, b) => a - b);
}

// Pairs of people who may be the same person: the same letters ("Ann-Marie
// Lee" / "Annmarie Lee"), the same last name and first initial ("Bob Roe" /
// "Robert Roe" doesn't; "B. Roe" / "Bob Roe" does), or names a typo apart.
export function nameSuggestions(people) {
  const letters = (k) => k.replace(/[^a-z0-9]/g, '');
  const words = (k) => k.replace(/[^a-z0-9 ]/g, '').split(' ').filter(Boolean);
  const out = [];
  for (let i = 0; i < people.length; i++) {
    for (let j = i + 1; j < people.length; j++) {
      const a = people[i].key;
      const b = people[j].key;
      const wa = words(a);
      const wb = words(b);
      const sameLetters = letters(a) === letters(b);
      const initial =
        wa.length > 1 && wb.length > 1 && wa.at(-1) === wb.at(-1) && wa[0][0] === wb[0][0] &&
        (wa[0].length === 1 || wb[0].length === 1);
      const typo = Math.min(a.length, b.length) >= 6 && editDistance(a, b) <= 2;
      if (sameLetters || initial || typo) out.push([people[i].name, people[j].name]);
    }
  }
  return out;
}

function editDistance(a, b) {
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j);
  for (let i = 1; i <= a.length; i++) {
    const cur = [i];
    for (let j = 1; j <= b.length; j++) {
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    }
    prev = cur;
  }
  return prev[b.length];
}

// ---- views -----------------------------------------------------------------

// Parts sorted by description, then element ID; each part's description and
// color are its latest year's.
function partInfo(entries) {
  const info = new Map();
  for (const e of entries) info.set(e.element_id, { element_id: e.element_id, description: e.description, color: e.color });
  return info;
}
const compareParts = (a, b) =>
  compareBytes(a.description, b.description) || compareBytes(a.element_id, b.element_id);

// A whole-cent-or-finer amount without float noise.
const money = (x) => Math.round(x * 1e6) / 1e6;

// Each person's totals: [{person, years: {year: {lots, pieces, spent}},
// lots, pieces, spent}]. `spent` counts only parts with a price;
// `unpriced` is how many of their lots had none.
export function peopleSummary(data) {
  const by = new Map(data.people.map((p) => [p.name, { person: p.name, years: {}, lots: 0, pieces: 0, spent: 0, unpriced: 0 }]));
  for (const e of data.entries) {
    const t = by.get(e.person);
    const y = (t.years[e.year] ??= { lots: 0, pieces: 0, spent: 0 });
    y.lots++;
    y.pieces += e.qty;
    t.lots++;
    t.pieces += e.qty;
    if (e.price === null) {
      t.unpriced++;
    } else {
      y.spent = money(y.spent + e.qty * e.price);
      t.spent = money(t.spent + e.qty * e.price);
    }
  }
  return [...by.values()];
}

// One person's parts across the years: [{element_id, description, color,
// qty: {year: n}, total, spent}].
export function personInventory(data, person) {
  const info = partInfo(data.entries);
  const rows = new Map();
  for (const e of data.entries) {
    if (e.person !== person) continue;
    let r = rows.get(e.element_id);
    if (!r) rows.set(e.element_id, (r = { ...info.get(e.element_id), qty: {}, total: 0, spent: 0 }));
    r.qty[e.year] = (r.qty[e.year] || 0) + e.qty;
    r.total += e.qty;
    if (e.price !== null) r.spent = money(r.spent + e.qty * e.price);
  }
  return [...rows.values()].sort(compareParts);
}

// One row per part and year it was bought: [{element_id, description,
// color, year, price, qty, people}], qty and people over everyone.
export function pricesByYear(data) {
  const info = partInfo(data.entries);
  const rows = new Map();
  for (const e of data.entries) {
    const k = `${e.element_id}\n${e.year}`;
    let r = rows.get(k);
    if (!r) rows.set(k, (r = { ...info.get(e.element_id), year: e.year, price: e.price, qty: 0, people: 0 }));
    r.qty += e.qty;
    r.people++;
  }
  return [...rows.values()].sort((a, b) => compareParts(a, b) || a.year - b.year);
}

// One row per part: [{element_id, description, color, prices: {year:
// price}, average, low, high, qty}]. The average is the plain average of
// the yearly prices (each year counts once, however many were bought);
// null when no year had a price.
export function averagePrices(data) {
  const rows = new Map();
  for (const r of pricesByYear(data)) {
    let a = rows.get(r.element_id);
    if (!a) {
      a = { element_id: r.element_id, description: r.description, color: r.color, prices: {}, qty: 0 };
      rows.set(r.element_id, a);
    }
    a.qty += r.qty;
    if (r.price !== null) a.prices[r.year] = r.price;
  }
  for (const a of rows.values()) {
    const p = Object.values(a.prices);
    a.average = p.length ? money(p.reduce((s, x) => s + x, 0) / p.length) : null;
    a.low = p.length ? Math.min(...p) : null;
    a.high = p.length ? Math.max(...p) : null;
  }
  return [...rows.values()].sort(compareParts);
}

// ---- text ------------------------------------------------------------------

const csvRow = (fields) => fields.map((f) => csvField(f ?? '')).join(',') + '\r\n';

// Every person's parts: one row per person, part and year.
export function inventoryCsv(data) {
  let out = csvRow(['person', 'year', 'element_id', 'description', 'color', 'qty', 'price', 'spent']);
  const order = new Map(data.people.map((p, i) => [p.name, i]));
  const rows = [...data.entries].sort(
    (a, b) => order.get(a.person) - order.get(b.person) || a.year - b.year ||
      compareParts(a, b),
  );
  for (const e of rows) {
    out += csvRow([e.person, e.year, e.element_id, e.description, e.color, formatCount(e.qty),
      formatMoney(e.price), e.price === null ? '' : formatMoney(e.qty * e.price)]);
  }
  return out;
}

export function peopleCsv(data) {
  let out = csvRow(['person', ...data.years.flatMap((y) => [`${y} lots`, `${y} pieces`, `${y} spent`]),
    'lots', 'pieces', 'spent']);
  for (const t of peopleSummary(data)) {
    out += csvRow([t.person, ...data.years.flatMap((y) => {
      const v = t.years[y];
      return v ? [v.lots, formatCount(v.pieces), formatMoney(v.spent)] : ['', '', ''];
    }), t.lots, formatCount(t.pieces), formatMoney(t.spent)]);
  }
  return out;
}

export function pricesByYearCsv(data) {
  let out = csvRow(['element_id', 'description', 'color', 'year', 'price', 'qty', 'people']);
  for (const r of pricesByYear(data)) {
    out += csvRow([r.element_id, r.description, r.color, r.year, formatMoney(r.price), formatCount(r.qty), r.people]);
  }
  return out;
}

export function averagePricesCsv(data) {
  let out = csvRow(['element_id', 'description', 'color', ...data.years.map(String), 'average', 'low', 'high', 'qty']);
  for (const r of averagePrices(data)) {
    out += csvRow([r.element_id, r.description, r.color, ...data.years.map((y) => formatMoney(r.prices[y])),
      formatMoney(r.average), formatMoney(r.low), formatMoney(r.high), formatCount(r.qty)]);
  }
  return out;
}

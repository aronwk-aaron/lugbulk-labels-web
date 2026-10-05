// The packing checklist, parts list and lot counts (CSV and PDF) and the
// sheet check text — the browser port of src/reports.{h,cpp} and the
// check text build_bundle (src/main.cpp) puts in the zip.
//
// With the default options (report_options.js DEFAULTS) the CSVs are byte
// for byte what the server writes (tests/js/parity.test.mjs) and the PDFs
// have the same layout: US Letter, 15 mm margins, Helvetica 9 pt rows
// 16 pt apart (taller where a cell wraps: text is never cut off), a gray
// header row and zebra stripes. Options add paper
// sizes, sorting, columns, photos and so on.
//
// Everything here is a pure function of the loaded records (load.js's
// `records`) and the options. Part photos are passed in as JPEG bytes
// (`images`: Map element id -> Uint8Array); fetching them is the caller's
// job (imageIdsFor says which).

import { helveticaMeasure } from './afm.js';
import { PDFDocument, StandardFonts, rgb } from './vendor/pdf-lib.js';
import { orderRecords, personSortKey as orderingSortKey, summarizeParts } from './ordering.js';
import { parseQty } from './pivot.js';
import { formatF, formatG } from './printf.js';
import { DEFAULTS, normalizeOptions, pageSize } from './report_options.js';
import { compareBytes } from './text.js';

// ---- text and numbers -------------------------------------------------------

// Sort key for a "First Last" name, as reports::person_sort_key: 'last'
// sorts on the last word (the whole name when there's only one), 'first'
// on the name as written; ties on the full lower-cased name.
export function personSortKey(person, sortBy = 'last') {
  return orderingSortKey(person, sortBy !== 'first');
}

function comparePeopleBy(sortBy) {
  return (a, b) => {
    const ka = personSortKey(a, sortBy);
    const kb = personSortKey(b, sortBy);
    return compareBytes(ka[0], kb[0]) || compareBytes(ka[1], kb[1]) || compareBytes(a, b);
  };
}

// A count the way the server prints one: whole numbers without ".0"
// (below 1e15), anything else as C++'s default stream format (%g).
export function formatCount(value) {
  if (Number.isFinite(value) && value === Math.floor(value) && Math.abs(value) < 1e15) {
    return BigInt(value).toString();
  }
  return formatG(value, 6);
}

// A price or amount: at least 2 decimals, up to 4 ("0.0696", "12.50").
export function formatMoney(x) {
  if (x === null || x === undefined) return '';
  let s = (Math.round(x * 1e6) / 1e6).toFixed(4);
  while (s.endsWith('0') && s.length - s.indexOf('.') > 3) s = s.slice(0, -1);
  return s;
}

// One RFC 4180 CSV field, with the server's guard against formula
// injection: a field starting with = + - @ tab or CR gets a leading
// apostrophe (names come from the shared sheet's column headers).
export function csvField(s) {
  let field = String(s);
  if (field !== '' && '=+-@\t\r'.includes(field[0])) field = `'${field}`;
  if (!/[,"\n\r]/.test(field)) return field;
  return `"${field.replaceAll('"', '""')}"`;
}

// "~4.6 g/pc" (estimated), "12 g/pc" (from the sheet or BrickLink), or
// "size unknown" — reports::weight_text.
export function weightText(part) {
  if (part.weight === null || part.weight === undefined) return 'size unknown';
  const n = part.weight >= 10 ? formatF(part.weight, 0) : formatG(part.weight, 2);
  return `${part.weight_source === 'estimate' ? '~' : ''}${n} g/pc`;
}

// A total mass: "850 g", "12.40 kg".
export function massText(grams) {
  return grams < 1000 ? `${formatF(grams, 0)} g` : `${formatF(grams / 1000, 2)} kg`;
}

const WINANSI_EXTRA = new Set([...'€‚ƒ„…†‡ˆ‰Š‹ŒŽ‘’“”•–—˜™š›œžŸ']);

// Text the standard PDF fonts (WinAnsi) can show: control characters
// become spaces, anything else outside WinAnsi a '?', as the server's
// pdf_text::to_winansi does, rather than failing the whole PDF.
export function winAnsi(s) {
  let out = '';
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    if ((c >= 0x20 && c < 0x7f) || (c >= 0xa0 && c <= 0xff) || WINANSI_EXTRA.has(ch)) out += ch;
    else if (c < 0x20 || (c >= 0x7f && c < 0xa0)) out += ' ';
    else out += '?';
  }
  return out;
}

// A download's file name stem for a sheet's display name — the server's
// safe_filename_stem: ASCII letters, digits, '-', '_' and space kept, any
// other byte (of the UTF-8) replaced by '_', then " _" trimmed off the
// ends. An upload's name loses its extension first and gets "order sheet"
// instead of "sheet" when nothing is left.
export function fileStem(name, upload = false) {
  let base = String(name);
  if (upload) {
    const dot = base.lastIndexOf('.');
    if (dot >= 0) base = base.slice(0, dot);
  }
  let out = '';
  for (const b of new TextEncoder().encode(base)) {
    const c = String.fromCharCode(b);
    out += /[A-Za-z0-9 _-]/.test(c) ? c : '_';
  }
  out = out.replace(/^[ _]+/, '').replace(/[ _]+$/, '');
  if (!out) return upload ? 'order sheet' : 'sheet';
  return out;
}

// ---- data -------------------------------------------------------------------

// Parts in a report's order: a PART_ORDERS value, or 'labels' for the
// label design's `labelOrder`, or 'element' (by element id, numerically).
export function orderedParts(records, order, labelOrder = 'heaviest') {
  if (order === 'element') {
    return summarizeParts(records, 'sheet').sort(
      (a, b) => a.element_id.length - b.element_id.length || compareBytes(a.element_id, b.element_id),
    );
  }
  return summarizeParts(records, order === 'labels' ? labelOrder : order);
}

// ---- prices -----------------------------------------------------------------

// Whether the reports show prices: the label switches (options.labels,
// "Price each" and "Lot price") carry over to them, when some record has a
// price (records.addPrices). {each, lot, sign, priceOf: Map id -> price}.
export function pricing(options, records) {
  const o = normalizeOptions(options).labels;
  const priceOf = new Map();
  let sign = '';
  for (const r of records) {
    if (r.price === null || r.price === undefined) continue;
    if (!priceOf.has(r.element_id)) priceOf.set(r.element_id, r.price);
    if (!sign && r.currency) sign = r.currency;
  }
  const priced = priceOf.size > 0;
  return { each: o.price && priced, lot: o.lot_price && priced, sign, priceOf };
}

// An amount in whole cents, with the sheet's currency sign: "$12.50".
export function centsText(x, sign = '') {
  return `${x < 0 ? '-' : ''}${sign}${(Math.round(Math.abs(x) * 100) / 100).toFixed(2)}`;
}

// What a record's lot costs (qty x price), or null without a price.
function lotCost(r, prices) {
  const price = prices.priceOf.get(r.element_id);
  return price === undefined ? null : parseQty(r.qty) * price;
}

// Per-part summaries by element id (for weights).
function partIndex(records) {
  return new Map(summarizeParts(records, 'sheet').map((p) => [p.element_id, p]));
}

// One row per person: {person, lot_count, total_pieces, grams, estimated,
// unknown}, sorted by `sortBy` ('last' | 'first') — reports::lot_counts_by_person
// plus total weights (grams of the parts with a known weight; `estimated`
// if any of those is an estimate, `unknown` if some part has no weight).
export function lotCountsByPerson(records, sortBy = 'last') {
  const parts = partIndex(records);
  const byPerson = new Map();
  for (const r of records) {
    let t = byPerson.get(r.person);
    if (!t) {
      t = { person: r.person, lot_count: 0, total_pieces: 0, grams: 0, estimated: false, unknown: false, cost: 0 };
      byPerson.set(r.person, t);
    }
    const qty = parseQty(r.qty);
    t.lot_count += 1;
    t.total_pieces += qty;
    if (r.price !== null && r.price !== undefined) t.cost += qty * r.price;
    const p = parts.get(r.element_id);
    if (p && p.weight !== null) {
      t.grams += qty * p.weight;
      if (p.weight_source === 'estimate') t.estimated = true;
    } else {
      t.unknown = true;
    }
  }
  const people = comparePeopleBy(sortBy);
  return [...byPerson.values()].sort((a, b) => people(a.person, b.person));
}

// A person's (or everyone's) total weight: "~1.20 kg+" style — "~" when
// part of it is estimated, "+" when some parts' weights aren't known.
function totalMassText(t) {
  if (t.grams === 0 && t.unknown) return '?';
  return `${t.estimated ? '~' : ''}${massText(t.grams)}${t.unknown ? '+' : ''}`;
}

// reports::lot_counts_csv: "person,lot_count,total_pieces" + a row per
// person with at least `minLots` lots.
// With `prices` (pricing()) showing any price, a total_price column too.
export function lotCountsCsv(records, sortBy = 'last', minLots = 0, prices = null) {
  const money = prices && (prices.each || prices.lot);
  let out = `person,lot_count,total_pieces${money ? ',total_price' : ''}\r\n`;
  for (const t of lotCountsByPerson(records, sortBy)) {
    if (t.lot_count < minLots) continue;
    out += `${csvField(t.person)},${t.lot_count},${formatCount(t.total_pieces)}`;
    if (money) out += `,${centsText(t.cost)}`;
    out += '\r\n';
  }
  return out;
}

// reports::parts_csv, one row per part in the order given.
// With `prices` (pricing()): price_each and/or total_price columns too.
export function partsCsv(parts, prices = null) {
  const each = !!(prices && prices.each);
  const lot = !!(prices && prices.lot);
  let out = 'order,element_id,description,lego_color,bl_color,total_pieces,people,grams_per_piece,weight_source' +
    `${each ? ',price_each' : ''}${lot ? ',total_price' : ''}\r\n`;
  parts.forEach((p, i) => {
    const grams = p.weight === null || p.weight === undefined ? '' : formatG(p.weight, 3);
    out +=
      `${i + 1},${csvField(p.element_id)},${csvField(p.description)},${csvField(p.lego_color)},` +
      `${csvField(p.bl_color)},${formatCount(p.pieces)},${p.lots},${grams},${p.weight_source}`;
    const price = prices ? prices.priceOf.get(p.element_id) : undefined;
    if (each) out += `,${price === undefined ? '' : formatMoney(price)}`;
    if (lot) out += `,${price === undefined ? '' : centsText(price * p.pieces)}`;
    out += '\r\n';
  });
  return out;
}

// The "sheet check.txt" in the zip: the label count, then each issue.
export function sheetCheckText(loaded) {
  let out = `${loaded.records.length} labels\n`;
  if (!loaded.issues.length) out += 'No issues found.\n';
  for (const i of loaded.issues) out += `Row ${i.row} (${i.kind}): ${i.detail}\n`;
  return out;
}

// ---- the reports, with options ------------------------------------------------

// The CSV of a report ('parts' | 'lots') with its options.
export function reportCsv(kind, records, options, labelOrder = 'heaviest') {
  const o = normalizeOptions(options);
  const prices = pricing(o, records);
  if (kind === 'parts') return partsCsv(orderedParts(records, o.parts.order, labelOrder), prices);
  if (kind === 'lots') return lotCountsCsv(records, o.lots.sort, o.lots.min_lots, prices);
  throw new Error(`no CSV for ${kind}`);
}

// Element ids whose photos a report needs with these options (none unless
// its photo column is on), in first-use order.
export function imageIdsFor(kind, records, options) {
  const o = normalizeOptions(options);
  if (!(kind === 'checklist' || kind === 'parts') || !o[kind].photo) return [];
  return [...new Set(records.map((r) => r.element_id))];
}

const joinColors = (lego, bl) => (!lego ? bl : !bl ? lego : `${lego} / ${bl}`);

// Colors cell per the parts list's color columns.
function colorCells(o, p) {
  if (o.lego_color && o.bl_color) return [joinColors(p.lego_color, p.bl_color)];
  if (o.lego_color) return [p.lego_color];
  if (o.bl_color) return [p.bl_color];
  return [];
}

function colorColumns(o, width) {
  if (o.lego_color && o.bl_color) return [{ header: 'LEGO / BrickLink color', width }];
  if (o.lego_color) return [{ header: 'LEGO color', width: 110 }];
  if (o.bl_color) return [{ header: 'BrickLink color', width: 110 }];
  return [];
}

const PHOTO_WIDTH = 34;
const PHOTO_ROW = 30;
const ROW = 16;
// Each wrapped line past a row's first adds this much to its height.
const LINE = 11;

// A table row's height for text `lines` lines tall.
export const rowHeightFor = (lines) => ROW + Math.max(0, lines - 1) * LINE;

// Breaks `text` into lines no wider than `maxW` as `width` measures them,
// at spaces; a word wider than a whole line is split between characters
// as a last resort. Nothing is dropped but the spaces lines break at.
// Always at least one line.
export function wrapText(text, maxW, width) {
  const lines = [];
  let line = '';
  for (const word of String(text).split(' ')) {
    const candidate = line ? `${line} ${word}` : word;
    if (width(candidate) <= maxW) {
      line = candidate;
      continue;
    }
    if (line) {
      lines.push(line);
      line = '';
      if (width(word) <= maxW) {
        line = word;
        continue;
      }
    }
    // Split by characters (code points), at least one per line.
    let chars = [...word];
    while (chars.length) {
      let n = 1;
      while (n < chars.length && width(chars.slice(0, n + 1).join('')) <= maxW) n++;
      if (n === chars.length) break;
      lines.push(chars.slice(0, n).join(''));
      chars = chars.slice(n);
    }
    line = chars.join('');
  }
  if (line || !lines.length) lines.push(line);
  return lines;
}

// The packing checklist: one section per person (last-name order by
// default) listing their labels in label order, with a tick box per line.
export async function checklistPdf(records, options = DEFAULTS, { labelOrder = 'heaviest', images } = {}) {
  const o = normalizeOptions(options).checklist;
  const prices = pricing(options, records);
  const ordered = orderRecords(records, o.order === 'labels' ? labelOrder : o.order);
  const parts = partIndex(records);
  const byPerson = new Map();
  for (const r of ordered) {
    if (!byPerson.has(r.person)) byPerson.set(r.person, []);
    byPerson.get(r.person).push(r);
  }
  const people = [...byPerson.keys()].sort(comparePeopleBy(o.sort));

  const columns = [];
  if (o.checkbox) columns.push({ header: '', width: 22, kind: 'check' });
  if (o.photo) columns.push({ header: 'Photo', width: PHOTO_WIDTH, kind: 'photo' });
  columns.push({ header: 'Element', width: 55 }, { header: 'Description', width: 160, flex: true });
  if (o.color) columns.push({ header: 'LEGO / BrickLink color', width: 145 });
  if (o.weight) columns.push({ header: 'Weight', width: 60 });
  columns.push({ header: 'Qty', width: 45 });
  if (prices.each) columns.push({ header: 'Each', width: 50 });
  if (prices.lot) columns.push({ header: 'Lot price', width: 58 });
  columns.push({ header: 'Label', width: 55 });

  const sections = people.map((person) => {
    const rows = [];
    let pieces = 0;
    let cost = 0;
    for (const r of byPerson.get(person)) {
      pieces += parseQty(r.qty);
      const lot = lotCost(r, prices);
      if (lot !== null) cost += lot;
      const cells = [];
      if (o.checkbox) cells.push('');
      if (o.photo) cells.push('');
      cells.push(r.element_id, r.description);
      if (o.color) cells.push(joinColors(r.lego_color, r.bl_color));
      if (o.weight) cells.push(weightText(parts.get(r.element_id) ?? { weight: null }));
      cells.push(r.qty);
      if (prices.each) {
        const price = prices.priceOf.get(r.element_id);
        cells.push(price === undefined ? '' : `${prices.sign}${formatMoney(price)}`);
      }
      if (prices.lot) cells.push(lot === null ? '' : centsText(lot, prices.sign));
      cells.push(r.part_total > 0 ? `${r.part_seq} of ${r.part_total}` : '');
      rows.push({ cells, image: o.photo ? r.element_id : null });
    }
    if (o.packed_by) rows.push({ signoff: true });
    const n = byPerson.get(person).length;
    const money = prices.lot ? `, ${centsText(cost, prices.sign)}` : '';
    return { title: person, lines: [`${n} lots, ${formatCount(pieces)} pieces${money}`], rows };
  });

  return tablePdf({
    docTitle: o.title || 'Packing checklist',
    size: pageSize(o),
    running: o.title || o.subtitle ? { title: o.title, subtitle: o.subtitle } : null,
    columns,
    rowHeight: o.photo ? PHOTO_ROW : ROW,
    continuous: o.layout === 'continuous',
    sections,
    images,
  });
}

const ORDER_WORDS = {
  labels: 'in label order',
  heaviest: 'heaviest first',
  lightest: 'lightest first',
  sheet: 'in sheet order',
  element: 'by element ID',
};

// The parts list: one row per part.
export async function partsPdf(records, options = DEFAULTS, { labelOrder = 'heaviest', images } = {}) {
  const o = normalizeOptions(options).parts;
  const prices = pricing(options, records);
  let parts = orderedParts(records, o.order, labelOrder);
  // Grouped by the color as shown (both names, or the one column that's on).
  const colorKey = (p) => colorCells(o, p)[0] ?? joinColors(p.lego_color, p.bl_color);
  if (o.group_by_color) {
    // Stable: parts keep their order within a color. No color sorts last.
    parts = parts
      .map((p, i) => [p, i])
      .sort(([a, i], [b, j]) => {
        const ka = colorKey(a).toLowerCase();
        const kb = colorKey(b).toLowerCase();
        if (!ka !== !kb) return ka ? -1 : 1;
        return compareBytes(ka, kb) || i - j;
      })
      .map(([p]) => p);
  }

  const columns = [{ header: '#', width: 24 }];
  if (o.photo) columns.push({ header: 'Photo', width: PHOTO_WIDTH, kind: 'photo' });
  columns.push({ header: 'Element', width: 50 }, { header: 'Description', width: 150, flex: true });
  columns.push(...colorColumns(o, 150));
  if (o.pieces) columns.push({ header: 'Pieces', width: 45 });
  if (o.people) columns.push({ header: 'People', width: 40 });
  if (o.weight) columns.push({ header: 'Weight', width: 70 });
  if (o.total_weight) columns.push({ header: 'Total weight', width: 70 });
  if (prices.each) columns.push({ header: 'Each', width: 50 });
  if (prices.lot) columns.push({ header: 'Total price', width: 62 });

  let totalPieces = 0;
  let totalLabels = 0;
  let totalCost = 0;
  const rows = [];
  let group = null;
  parts.forEach((p, i) => {
    totalPieces += p.pieces;
    totalLabels += p.lots;
    if (o.group_by_color) {
      const key = colorKey(p);
      if (group === null || key.toLowerCase() !== group.toLowerCase()) {
        group = key;
        const count = parts.filter((q) => colorKey(q).toLowerCase() === key.toLowerCase()).length;
        rows.push({ group: `${key || 'No color'} (${count} part${count === 1 ? '' : 's'})` });
      }
    }
    const cells = [String(i + 1)];
    if (o.photo) cells.push('');
    cells.push(p.element_id, p.description, ...colorCells(o, p));
    if (o.pieces) cells.push(formatCount(p.pieces));
    if (o.people) cells.push(String(p.lots));
    if (o.weight) cells.push(weightText(p));
    if (o.total_weight) {
      cells.push(p.weight === null ? '?' : `${p.weight_source === 'estimate' ? '~' : ''}${massText(p.weight * p.pieces)}`);
    }
    const price = prices.priceOf.get(p.element_id);
    if (price !== undefined) totalCost += price * p.pieces;
    if (prices.each) cells.push(price === undefined ? '' : `${prices.sign}${formatMoney(price)}`);
    if (prices.lot) cells.push(price === undefined ? '' : centsText(price * p.pieces, prices.sign));
    rows.push({ cells, image: o.photo ? p.element_id : null });
  });

  let stats =
    `${parts.length} parts, ${formatCount(totalPieces)} pieces, ${totalLabels} labels — ` +
    ORDER_WORDS[o.order];
  if (o.group_by_color) stats += ', grouped by color';
  if (prices.lot) stats += ` — ${centsText(totalCost, prices.sign)} in all`;
  return tablePdf({
    docTitle: o.title || 'Parts list',
    size: pageSize(o),
    columns,
    rowHeight: o.photo ? PHOTO_ROW : ROW,
    sections: [{ title: o.title || 'Parts list', lines: [o.subtitle, stats].filter(Boolean), rows }],
    images,
  });
}

// Lot counts: one row per person.
export async function lotCountsPdf(records, options = DEFAULTS) {
  const o = normalizeOptions(options).lots;
  const prices = pricing(options, records);
  const money = prices.each || prices.lot;
  const all = lotCountsByPerson(records, o.sort);
  const totals = all.filter((t) => t.lot_count >= o.min_lots);

  const columns = [{ header: 'Person', width: 280, flex: true }];
  if (o.lots) columns.push({ header: 'Lots', width: 100 });
  if (o.pieces) columns.push({ header: 'Total pieces', width: 120 });
  if (o.total_weight) columns.push({ header: 'Total weight', width: 100 });
  if (money) columns.push({ header: 'Total price', width: 90 });
  const cellsFor = (label, t) => {
    const cells = [label];
    if (o.lots) cells.push(String(t.lot_count));
    if (o.pieces) cells.push(formatCount(t.total_pieces));
    if (o.total_weight) cells.push(totalMassText(t));
    if (money) cells.push(centsText(t.cost, prices.sign));
    return cells;
  };

  const sum = { lot_count: 0, total_pieces: 0, grams: 0, estimated: false, unknown: false, cost: 0 };
  const rows = totals.map((t) => {
    sum.cost += t.cost;
    sum.lot_count += t.lot_count;
    sum.total_pieces += t.total_pieces;
    sum.grams += t.grams;
    sum.estimated ||= t.estimated;
    sum.unknown ||= t.unknown;
    return { cells: cellsFor(t.person, t) };
  });
  if (o.totals) rows.push({ total: cellsFor(`Total (${totals.length} people)`, sum) });

  let stats = `${totals.length} people, ${sum.lot_count} lots total — sorted by ${o.sort} name`;
  if (o.min_lots > 1) stats += ` (${all.length - totals.length} with fewer than ${o.min_lots} lots left out)`;
  return tablePdf({
    docTitle: o.title || 'Lot counts by person',
    size: pageSize(o),
    columns,
    rowHeight: ROW,
    sections: [{ title: o.title || 'Lot counts by person', lines: [o.subtitle, stats].filter(Boolean), rows }],
  });
}

// A report's PDF bytes by kind ('checklist' | 'parts' | 'lots').
export function reportPdf(kind, records, options, context = {}) {
  if (kind === 'checklist') return checklistPdf(records, options, context);
  if (kind === 'parts') return partsPdf(records, options, context);
  if (kind === 'lots') return lotCountsPdf(records, options);
  throw new Error(`no PDF for ${kind}`);
}

// ---- the table renderer ---------------------------------------------------------

const MARGIN = 15 * (72 / 25.4); // 15 mm in points
const BLACK = rgb(0, 0, 0);
const HEADER_FILL = rgb(0.85, 0.85, 0.85);
const ZEBRA_FILL = rgb(0.96, 0.96, 0.96);
const GROUP_FILL = rgb(0.9, 0.9, 0.9);

// Column widths for the page: as given when they fit (within 5 pt over)
// and don't leave a lot of room (60 pt or more) unused; otherwise the flex
// column(s) take up the difference, and if that's not enough every column
// shrinks in proportion.
export function fitColumns(widths, flex, available) {
  const total = widths.reduce((a, b) => a + b, 0);
  if (total <= available + 5 && total >= available - 60) return widths.slice();
  const flexTotal = widths.reduce((a, w, i) => a + (flex[i] ? w : 0), 0);
  let out = widths.slice();
  if (flexTotal > 0) {
    const diff = available - total;
    out = widths.map((w, i) => (flex[i] ? Math.max(40, w + (diff * w) / flexTotal) : w));
  }
  const now = out.reduce((a, b) => a + b, 0);
  if (now > available) out = out.map((w) => (w * available) / now);
  return out;
}

// Draws paginated tables. Each section is a title, subtitle lines and rows
// under a header row; `continuous` runs sections on after each other
// instead of starting each on a new page. Title, subtitles and header
// repeat on every page. Rows: {cells, image} (data), {group: text},
// {total: cells} or {signoff: true} ("Packed by / Date" line).
async function tablePdf({ docTitle, size, running = null, columns, rowHeight, continuous = false, sections, images }) {
  const doc = await PDFDocument.create();
  doc.setTitle(winAnsi(docTitle));
  doc.setCreator('lugbulk-labels-web');
  doc.setProducer('lugbulk-labels-web');
  const bold = await doc.embedFont(StandardFonts.HelveticaBold);
  const regular = await doc.embedFont(StandardFonts.Helvetica);
  const [pageW, pageH] = size;
  // Widths as labels measure them (afm.js): the AFM's, no kerning.
  const widthOf = (font, t, sz) => helveticaMeasure(font === bold ? 'bold' : 'regular', t, sz);

  const widths = fitColumns(
    columns.map((c) => c.width),
    columns.map((c) => !!c.flex),
    pageW - 2 * MARGIN,
  );
  const tableW = widths.reduce((a, b) => a + b, 0);

  // Photos, embedded once each. Anything that isn't a usable JPEG is skipped.
  const embedded = new Map();
  if (images) {
    for (const s of sections) {
      for (const r of s.rows) {
        if (!r.image || embedded.has(r.image)) continue;
        const bytes = images.get(r.image);
        let img = null;
        if (bytes && bytes.length) {
          try {
            img = await doc.embedJpg(bytes);
          } catch {
            img = null;
          }
        }
        embedded.set(r.image, img);
      }
    }
  }

  // Cell text wraps to its column at 9 pt (see wrapText): nothing is cut.
  const wrap = (font, s, maxW, sz = 9) =>
    wrapText(winAnsi(s), maxW, (t) => widthOf(font, t, sz));
  const text = (page, s, x, y, font, sz) => {
    if (s) page.drawText(s, { x, y, size: sz, font, color: BLACK });
  };
  const band = (page, y, h, color) =>
    page.drawRectangle({ x: MARGIN, y: y - h + 4, width: tableW, height: h, color });

  // Each cell's lines (none for tick boxes and photos) and the row height
  // they need: one line fits a 16 pt row, each more adds LINE.
  const cellLines = (font, cells, kinds) =>
    cells.slice(0, widths.length).map((s, c) => {
      const kind = kinds ? columns[c].kind : undefined;
      return kind === 'check' || kind === 'photo' ? [] : wrap(font, s, widths[c] - 6);
    });
  const cellsHeight = (lines) => rowHeightFor(Math.max(1, ...lines.map((l) => l.length)));

  // A row's top is y + 4 (its band runs from there down h). Every cell's
  // lines are centred in the row, so a one-line row is drawn as before.
  const drawCells = (page, font, y, h, lines, kinds, image) => {
    const middle = y + 4 - h / 2 - 4;
    let x = MARGIN;
    for (let c = 0; c < lines.length; c++) {
      const kind = kinds ? columns[c].kind : undefined;
      if (kind === 'check') {
        page.drawRectangle({ x: x + 4, y: middle - 2, width: 8, height: 8, borderColor: BLACK, borderWidth: 0.8 });
      } else if (kind === 'photo') {
        const img = image ? embedded.get(image) : null;
        if (img) {
          const boxW = widths[c] - 4;
          const boxH = h - 4;
          const scale = Math.min(boxW / img.width, boxH / img.height);
          const w = img.width * scale;
          const ih = img.height * scale;
          page.drawImage(img, { x: x + 2 + (boxW - w) / 2, y: y + 4 - h + 2 + (boxH - ih) / 2, width: w, height: ih });
        }
      } else {
        let baseline = middle + ((lines[c].length - 1) * LINE) / 2;
        for (const line of lines[c]) {
          text(page, line, x + 3, baseline, font, 9);
          baseline -= LINE;
        }
      }
      x += widths[c];
    }
  };

  // Rows laid out once: their lines and heights.
  const headerLines = cellLines(bold, columns.map((c) => c.header), false);
  const headerH = cellsHeight(headerLines);
  const laid = new Map();
  for (const s of sections) {
    for (const r of s.rows) {
      if (r.group) {
        const lines = wrap(bold, r.group, tableW - 6);
        laid.set(r, { lines, h: rowHeightFor(lines.length) });
      } else if (r.total) {
        const lines = cellLines(bold, r.total, false);
        laid.set(r, { lines, h: cellsHeight(lines) });
      } else if (r.signoff) {
        laid.set(r, { h: 30 });
      } else {
        const lines = cellLines(regular, r.cells, true);
        laid.set(r, { lines, h: Math.max(rowHeight, cellsHeight(lines)) });
      }
    }
  }
  const rowH = (r) => laid.get(r).h;

  let page = null;
  let y = 0;
  let zebra = 0;
  // Title (14 pt) and subtitle lines wrap to the table's width.
  const headingLines = (s, cont) => ({
    title: wrap(bold, s.title + (cont && continuous ? ' (continued)' : ''), tableW, 14),
    lines: s.lines.flatMap((line) => wrap(regular, line, tableW)),
  });
  const headingHeight = (s, cont) => {
    const h = headingLines(s, cont);
    return 16 * h.title.length + 16 * h.lines.length + 16 + headerH;
  };

  const startPage = () => {
    page = doc.addPage([pageW, pageH]);
    y = pageH - MARGIN;
    if (running) {
      // Title and subtitle on one line when they fit, else each wrapped.
      const title = winAnsi(running.title);
      const sub = winAnsi(running.subtitle);
      const titleW = title ? widthOf(bold, title, 10) + 8 : 0;
      if (titleW + widthOf(regular, sub, 9) <= tableW) {
        text(page, title, MARGIN, y - 10, bold, 10);
        text(page, sub, MARGIN + titleW, y - 10, regular, 9);
        y -= 20;
      } else {
        const lines = [
          ...(title ? wrap(bold, running.title, tableW, 10).map((l) => [l, bold, 10]) : []),
          ...(sub ? wrap(regular, running.subtitle, tableW).map((l) => [l, regular, 9]) : []),
        ];
        lines.forEach(([l, font, sz], i) => text(page, l, MARGIN, y - 10 - 12 * i, font, sz));
        y -= 20 + 12 * (lines.length - 1);
      }
    }
  };
  const drawHeading = (s, cont) => {
    const h = headingLines(s, cont);
    for (const line of h.title) {
      text(page, line, MARGIN, y - 14, bold, 14);
      y -= 16;
    }
    for (const line of h.lines) {
      text(page, line, MARGIN, y - 10, regular, 9);
      y -= 16;
    }
    y -= 16;
    band(page, y, headerH, HEADER_FILL);
    drawCells(page, bold, y, headerH, headerLines, false, null);
    y -= headerH;
    zebra = 0;
  };

  sections.forEach((s, si) => {
    const first = s.rows.length ? rowH(s.rows[0]) : 0;
    if (!page || !continuous || y - 12 - headingHeight(s, false) - first < MARGIN) {
      startPage();
    } else if (si > 0) {
      y -= 12; // gap between people on the same page
    }
    drawHeading(s, false);
    s.rows.forEach((r, ri) => {
      let need = rowH(r);
      // A color heading keeps at least one of its parts with it.
      if (r.group && ri + 1 < s.rows.length) need += rowH(s.rows[ri + 1]);
      if (y - need < MARGIN) {
        startPage();
        drawHeading(s, true);
      }
      drawRow(r);
    });
  });
  if (!page) doc.addPage([pageW, pageH]);

  function drawRow(r) {
    const { h, lines } = laid.get(r);
    if (r.group) {
      band(page, y, h, GROUP_FILL);
      lines.forEach((line, i) => text(page, line, MARGIN + 3, y - 8 - LINE * i, bold, 9));
      zebra = 0;
    } else if (r.total) {
      page.drawLine({ start: { x: MARGIN, y: y + 4 }, end: { x: MARGIN + tableW, y: y + 4 }, thickness: 0.8, color: BLACK });
      drawCells(page, bold, y, h, lines, false, null);
    } else if (r.signoff) {
      const base = y - 18;
      text(page, 'Packed by:', MARGIN + 3, base, regular, 9);
      page.drawLine({ start: { x: MARGIN + 52, y: base - 2 }, end: { x: MARGIN + 250, y: base - 2 }, thickness: 0.6, color: BLACK });
      text(page, 'Date:', MARGIN + 265, base, regular, 9);
      page.drawLine({ start: { x: MARGIN + 292, y: base - 2 }, end: { x: MARGIN + 400, y: base - 2 }, thickness: 0.6, color: BLACK });
    } else {
      if (zebra % 2 === 1) band(page, y, h, ZEBRA_FILL);
      zebra++;
      drawCells(page, regular, y, h, lines, true, r.image);
    }
    y -= h;
  }

  return doc.save();
}

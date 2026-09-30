// Pivots the wide per-person qty matrix (the "Order Here" tab) into one
// label record per (person, part) pair where qty > 0 — the browser port of
// src/sheet_pivot.{h,cpp}, with the same issue kinds and messages. Two
// sheet layouts are handled:
//
//   - "qty marker": person names on the header row, and the row below
//     marks each person's qty column "qty" (paired with a "$$" column).
//   - "name/cost pair": each person is a header cell holding their name
//     followed by one holding their running cost total, with a totals row
//     above the header.
//
// Front-matter columns (element id, description, LEGO/BL color, weight)
// are found by header text, falling back to fixed positions.
//
// A record: {person, element_id, description, lego_color, bl_color, qty,
// image_url, weight, catalog_weight, part_seq, part_total} (weights are
// grams per piece or null; part_seq/part_total are filled in by
// orderRecords). An issue: {row, kind, detail, element_id} with `row`
// 1-indexed like the Sheets UI and `kind` one of "duplicate" | "bad_qty" |
// "bad_element_id" | "missing_description" | "missing_color" |
// "unmapped_color" | "bad_weight" (element_id is set for "missing_color",
// so a later BrickLink lookup can clear it, else "").

import * as colors from './colors.js';
import * as layout from './layout.js';
import { C_SPACE, asciiLower, str, trim } from './text.js';

function cell(row, idx) {
  if (idx === null || idx === undefined || idx >= row.length) return '';
  return trim(str(row[idx]));
}

// "232.45", "$232.45", "€1,234" — a person's running cost header cell.
const NUMBER = /^(\$|€|£)?[ \t\n\v\f\r]*-?[0-9,]*\.?[0-9]+$/;
function isNumber(text) {
  return NUMBER.test(trim(text));
}

// First header matching a candidate, in candidate priority order. If more
// than one matches, prefer one whose column actually has data.
function findCol(header, candidates, rows = [], dataStart = 0) {
  const matches = [];
  for (const name of candidates) {
    const want = asciiLower(name);
    for (let col = 0; col < header.length; col++) {
      if (asciiLower(trim(str(header[col]))) === want) {
        matches.push(col);
        break;
      }
    }
  }
  for (const col of matches) {
    for (let r = dataStart; r < rows.length && r < dataStart + 50; r++) {
      if (cell(rows[r], col) !== '') return col;
    }
  }
  return matches.length ? matches[0] : null;
}

function findHeaderRow(rows) {
  const order = [layout.HEADER_ROW];
  for (let r = 0; r < layout.HEADER_SEARCH_ROWS; r++) order.push(r);
  for (const r of order) {
    if (r < rows.length && findCol(rows[r], layout.ELEMENT_ID_HEADERS) !== null) return r;
  }
  return null;
}

// [qty column, name] pairs.
function qtyMarkerColumns(header, subheader) {
  const people = [];
  for (let col = 0; col < subheader.length; col++) {
    if (asciiLower(trim(str(subheader[col]))) === layout.QTY_MARKER) {
      const name = cell(header, col);
      if (name) people.push([col, name]);
    }
  }
  return people;
}

// A contiguous run of (name, cost) header pairs, starting at the first
// one at or after scanStart and stopping at the first break in the run.
function nameCostPairColumns(header, scanStart) {
  const isPair = (col) => {
    if (col + 1 >= header.length) return false;
    const name = cell(header, col);
    return name !== '' && !isNumber(name) && isNumber(cell(header, col + 1));
  };
  let col = scanStart;
  while (col < header.length && !isPair(col)) col++;
  const people = [];
  for (; col < header.length && isPair(col); col += 2) people.push([col, cell(header, col)]);
  return people;
}

// ---- std::stod / printf("%f") equivalents --------------------------------

const DBL_MIN = 2.2250738585072014e-308;

// std::stod on the whole of `s`: the number, or undefined where the C++
// throws (not a number, trailing junk, or out of double range).
function stod(s) {
  let i = 0;
  while (i < s.length && C_SPACE.includes(s[i])) i++; // strtod skips leading space
  const t = s.slice(i);
  let m;
  if ((m = /^([+-]?)inf(inity)?$/i.exec(t))) return m[1] === '-' ? -Infinity : Infinity;
  if (/^[+-]?nan(\([A-Za-z0-9_]*\))?$/i.test(t)) return NaN;
  let value;
  let nonzeroDigits;
  if ((m = /^([+-]?)0x([0-9a-f]*)(?:\.([0-9a-f]*))?(?:p([+-]?[0-9]+))?$/i.exec(t))) {
    const digits = m[2] + (m[3] ?? '');
    if (!digits) return undefined; // "0x" parses as 0 then stops at "x"
    const exp = Number(m[4] ?? '0') - 4 * (m[3] ?? '').length;
    value = Number(BigInt('0x' + digits)) * 2 ** exp;
    nonzeroDigits = /[1-9a-f]/i.test(digits);
    if (m[1] === '-') value = -value;
  } else if (/^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)(e[+-]?[0-9]+)?$/i.test(t)) {
    value = Number(t);
    nonzeroDigits = /[1-9]/.test(t.replace(/e.*$/i, ''));
  } else {
    return undefined;
  }
  // ERANGE (std::out_of_range): overflow, or underflow to a subnormal/zero.
  if (!Number.isFinite(value)) return undefined;
  if (nonzeroDigits && Math.abs(value) < DBL_MIN) return undefined;
  return value;
}

// printf("%.6f", x), exactly: the double's exact decimal value rounded
// half-to-even, as glibc does (toFixed rounds exact ties up instead).
function fixed6(x) {
  if (Number.isNaN(x)) return 'nan';
  if (!Number.isFinite(x)) return x < 0 ? '-inf' : 'inf';
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, x);
  const bits = view.getBigUint64(0);
  const negative = bits >> 63n === 1n;
  const biased = Number((bits >> 52n) & 0x7ffn);
  let mant = bits & ((1n << 52n) - 1n);
  let exp;
  if (biased === 0) {
    exp = -1074;
  } else {
    mant |= 1n << 52n;
    exp = biased - 1075;
  }
  // x = mant * 2^exp; scaled = round(x * 10^6)
  let scaled;
  if (exp >= 0) {
    scaled = (mant << BigInt(exp)) * 1000000n;
  } else {
    const k = BigInt(-exp);
    const num = mant * 1000000n;
    scaled = num >> k;
    const rem = num - (scaled << k);
    const half = 1n << (k - 1n);
    if (rem > half || (rem === half && (scaled & 1n) === 1n)) scaled += 1n;
  }
  let digits = scaled.toString().padStart(7, '0');
  digits = `${digits.slice(0, -6)}.${digits.slice(-6)}`;
  return negative ? `-${digits}` : digits;
}

// Normalized display text for a quantity, like the C++ format_qty: whole
// numbers without a decimal point, else %f with trailing zeros dropped.
function formatQty(qty) {
  // static_cast<long long>(qty) == qty: whole and within long long range
  // (x86 gives LLONG_MIN for anything out of range).
  if (Number.isInteger(qty) && qty >= -(2 ** 63) && qty < 2 ** 63) {
    return BigInt(qty).toString();
  }
  const s = fixed6(qty); // std::to_string(double)
  let end = s.length;
  while (end > 0 && s[end - 1] === '0') end--;
  return s.slice(0, end);
}

// ---- public ---------------------------------------------------------------

// Parses a Sheets-formatted quantity string ("2,000", "150", etc.) into a
// number, stripping thousands separators. Throws if it's not numeric after
// stripping. A record's own qty always parses.
export function parseQty(qty) {
  const stripped = trim(str(qty).replaceAll(',', ''));
  if (!stripped) throw new Error('empty qty');
  const value = stod(stripped);
  if (value === undefined) throw new Error(`non-numeric qty: ${qty}`);
  return value;
}

// LEGO element IDs are 4-8 digits. Anything else is a typo or a stray
// notes/footer row — and the ID becomes part of the image URL.
export function isValidElementId(elementId) {
  return /^[0-9]{4,8}$/.test(elementId);
}

// What sheets put in a color cell when they don't know it; treated as blank.
const PLACEHOLDERS = new Set(['unknown', 'n/a', 'na', '?', '-', 'tbd', 'none']);
function unlessPlaceholder(color) {
  return PLACEHOLDERS.has(asciiLower(color)) ? '' : color;
}

function parseWeight(raw, elementId, sheetRow, issues) {
  let text = asciiLower(trim(raw));
  if (text.endsWith('g')) text = trim(text.slice(0, -1));
  if (!text) return null;
  let weight;
  try {
    weight = parseQty(text);
  } catch {
    issues.push(issue(sheetRow, 'bad_weight', `Element ${elementId} has a non-numeric weight: '${raw}'`));
    return null;
  }
  return weight > 0 ? weight : null;
}

function issue(row, kind, detail, elementId = '') {
  return { row, kind, detail, element_id: elementId };
}

// Pivots raw sheet rows (arrays of cell strings, as GET /sheets/:id/values
// returns) into {records, issues}. Never throws on bad data — issues are
// how bad data surfaces.
export function pivotSheet(rows) {
  const result = { records: [], issues: [] };
  if (!rows.length) return result;

  const foundHeader = findHeaderRow(rows);
  const headerRow = foundHeader ?? layout.HEADER_ROW;
  if (headerRow >= rows.length) return result;
  const header = rows[headerRow];
  const subheader = headerRow + 1 < rows.length ? rows[headerRow + 1] : [];
  const dataStart = headerRow + 2;

  let colId = null;
  let colDesc = null;
  let colLego = null;
  let colBl = null;
  let colWeight = null;
  if (foundHeader === null) {
    colId = layout.COL_ELEMENT_ID;
    colDesc = layout.COL_DESCRIPTION;
    colBl = layout.COL_COLOR;
  } else {
    colId = findCol(header, layout.ELEMENT_ID_HEADERS);
    colDesc = findCol(header, layout.DESCRIPTION_HEADERS, rows, dataStart);
    colLego = findCol(header, layout.LEGO_COLOR_HEADERS, rows, dataStart);
    colBl = findCol(header, layout.BL_COLOR_HEADERS, rows, dataStart);
    colWeight = findCol(header, layout.WEIGHT_HEADERS, rows, dataStart);
    if (colDesc === null) colDesc = layout.COL_DESCRIPTION;
  }

  let people = qtyMarkerColumns(header, subheader);
  if (!people.length) {
    let lastFront = 0;
    for (const c of [colId, colDesc, colLego, colBl, colWeight]) {
      if (c !== null) lastFront = Math.max(lastFront, c);
    }
    people = nameCostPairColumns(header, lastFront + 1);
  }

  const seen = new Set(); // JSON [person, element_id]

  for (let offset = dataStart; offset < rows.length; offset++) {
    const sheetRow = offset + 1; // 1-indexed, matches Sheets UI
    const row = rows[offset];

    const elementId = cell(row, colId);
    if (!elementId) continue; // blank/footer row
    if (!isValidElementId(elementId)) {
      if (people.some(([col]) => cell(row, col) !== '')) {
        result.issues.push(
          issue(sheetRow, 'bad_element_id', `'${elementId}' isn't a LEGO element ID; row skipped`),
        );
      }
      continue;
    }

    const description = cell(row, colDesc);
    if (!description) {
      result.issues.push(issue(sheetRow, 'missing_description', `Element ${elementId} has no description`));
    }

    const wanted = []; // [person, qty]
    for (const [qtyCol, person] of people) {
      const qty = cell(row, qtyCol);
      if (!qty) continue; // blank cell, not a mistake
      let qtyNum;
      try {
        qtyNum = parseQty(qty);
      } catch {
        result.issues.push(
          issue(sheetRow, 'bad_qty', `${person}'s qty for element ${elementId} is non-numeric: '${qty}'`),
        );
        continue;
      }
      if (qtyNum > 0) wanted.push([person, formatQty(qtyNum)]);
    }
    if (!wanted.length) continue; // nobody ordered it; don't nag about its colors

    let lego = unlessPlaceholder(cell(row, colLego));
    let bl = unlessPlaceholder(cell(row, colBl));
    if (!lego && !bl) {
      result.issues.push(issue(sheetRow, 'missing_color', `Element ${elementId} has no color`, elementId));
    } else {
      const c = colors.resolve(lego, bl);
      if (!c.mapped) {
        const known = c.lego ? c.lego : c.bl;
        result.issues.push(
          issue(
            sheetRow,
            'unmapped_color',
            `Element ${elementId}: don't know the ${c.lego ? 'BrickLink' : 'LEGO'} name for '${known}'` +
              ` — fill in the sheet's ${c.lego ? 'BL Color' : 'LEGO Color'} column`,
          ),
        );
      }
      lego = c.lego;
      bl = c.bl;
    }

    const weight = parseWeight(cell(row, colWeight), elementId, sheetRow, result.issues);
    const imageUrl = layout.imageUrlFor(elementId);

    for (const [person, qty] of wanted) {
      const k = JSON.stringify([person, elementId]);
      if (seen.has(k)) {
        result.issues.push(
          issue(sheetRow, 'duplicate', `${person} has more than one qty entry for element ${elementId}`),
        );
      }
      seen.add(k);
      result.records.push({
        person,
        element_id: elementId,
        description,
        lego_color: lego,
        bl_color: bl,
        qty,
        image_url: imageUrl,
        weight,
        catalog_weight: null,
        part_seq: 0,
        part_total: 0,
      });
    }
  }
  return result;
}

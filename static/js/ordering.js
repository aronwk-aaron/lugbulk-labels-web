// Part sizing, label ordering, and per-part summaries — the browser port
// of src/ordering.{h,cpp}.
//
// Labels come out grouped by part: parts ordered by (estimated) weight,
// heaviest first by default, and within a part by quantity, smallest first.
// Each label is numbered within its part ("3 of 10").
//
// Weight comes from the sheet's "Weight" column (grams per piece) when
// present, else the BrickLink catalog, else an estimate from the
// description's stud dimensions ("PLATE 4X8", "BRICK 1X2X5",
// "BRICK 1X1X1 2/3"). Parts with none of these sort after everything
// else, in sheet order.

import { parseQty } from './pivot.js';
import { asciiLower, asciiUpper, compareBytes, splitWs } from './text.js';

export const PART_ORDERS = ['heaviest', 'lightest', 'sheet'];

// "heaviest" / "lightest" / "sheet" as is; null for anything else.
export function parsePartOrder(s) {
  return PART_ORDERS.includes(s) ? s : null;
}

// Rough mass of one 1x1x1 brick-volume of ABS, in grams.
const GRAMS_PER_BRICK_UNIT = 0.43;

// footprint in studs, then an optional height in bricks: "1X2X5",
// "1X1X1 2/3", "2X2X2/3" (2/3 of a brick), or "1X2X3/73°" (3 bricks tall,
// 73° slope — not 3/73). (Whitespace is the C locale's, as in the C++.)
const DIMS =
  /([0-9]+)[ \t\n\v\f\r]*X[ \t\n\v\f\r]*([0-9]+)(?:[ \t\n\v\f\r]*X[ \t\n\v\f\r]*([0-9]+)(?:[ \t\n\v\f\r]+([0-9]+)\/([0-9]+)|\/([0-9]+)(°)?)?)?/;
const FULL_HEIGHT = /\b(BRICK|ROOF|DUPLO)\b/;
const THIRD_HEIGHT = /\b(PLATE|TILE|PLADE)\b/;

// Grams per piece estimated from a description's stud dimensions, or null.
export function estimateWeight(description) {
  const desc = asciiUpper(description);
  const m = DIMS.exec(desc);
  if (!m) return null;

  let height = 1.0;
  if (m[3] !== undefined) {
    height = Number(m[3]);
    if (m[4] !== undefined) {
      height += Number(m[4]) / Number(m[5]);
    } else if (m[6] !== undefined && m[7] === undefined && m[6] === '3') {
      height /= 3;
    }
  } else if (!FULL_HEIGHT.test(desc) && THIRD_HEIGHT.test(desc)) {
    height = 1.0 / 3; // plates and tiles are a third of a brick tall
  }

  let volume = Number(m[1]) * Number(m[2]) * height;
  if (desc.includes('DUPLO')) volume *= 8; // twice the size every way
  return volume * GRAMS_PER_BRICK_UNIT;
}

// One summary per distinct element, in label order:
// {element_id, description, lego_color, bl_color, lots, pieces, weight,
//  weight_source ("sheet" | "bricklink" | "estimate" | "")}.
export function summarizeParts(records, order) {
  const parts = []; // sheet order
  const index = new Map();
  for (const r of records) {
    let p = parts[index.get(r.element_id)];
    if (p === undefined) {
      p = {
        element_id: r.element_id,
        description: r.description,
        lego_color: r.lego_color,
        bl_color: r.bl_color,
        lots: 0,
        pieces: 0,
        weight: null,
        weight_source: '',
      };
      if (r.weight != null) {
        p.weight = r.weight;
        p.weight_source = 'sheet';
      } else if (r.catalog_weight != null) {
        p.weight = r.catalog_weight;
        p.weight_source = 'bricklink';
      } else if ((p.weight = estimateWeight(r.description)) !== null) {
        p.weight_source = 'estimate';
      }
      index.set(r.element_id, parts.length);
      parts.push(p);
    }
    p.lots += 1;
    p.pieces += parseQty(r.qty);
  }

  if (order === 'sheet') return parts;
  const sign = order === 'heaviest' ? -1 : 1;
  // Stable: unknown-weight parts keep sheet order after the rest.
  return parts.sort((a, b) => {
    const ha = a.weight !== null;
    const hb = b.weight !== null;
    if (ha !== hb) return ha ? -1 : 1;
    if (!ha) return 0;
    const x = sign * a.weight;
    const y = sign * b.weight;
    return x < y ? -1 : y < x ? 1 : 0;
  });
}

// Sort key for a person: last name, then full name (lower-cased).
export function personSortKey(person, byLastName = true) {
  const parts = splitWs(person);
  const primary = parts.length && byLastName ? asciiLower(parts[parts.length - 1]) : asciiLower(person);
  return [primary, asciiLower(person)];
}

export function comparePeople(a, b) {
  const ka = personSortKey(a);
  const kb = personSortKey(b);
  return compareBytes(ka[0], kb[0]) || compareBytes(ka[1], kb[1]);
}

// Records grouped by part in `order`, smallest qty first within a part
// (ties by last name), with part_seq/part_total filled in. Returns new
// record objects; the input is left alone.
export function orderRecords(records, order) {
  const byPart = new Map();
  for (const r of records) {
    if (!byPart.has(r.element_id)) byPart.set(r.element_id, []);
    byPart.get(r.element_id).push({ ...r });
  }
  const ordered = [];
  for (const part of summarizeParts(records, order)) {
    const group = byPart.get(part.element_id);
    group.sort((a, b) => {
      const qa = parseQty(a.qty);
      const qb = parseQty(b.qty);
      if (qa !== qb) return qa < qb ? -1 : qb < qa ? 1 : 0;
      // Same tie-break as the lot-count report: last name, then full name.
      return comparePeople(a.person, b.person);
    });
    group.forEach((r, i) => {
      r.part_seq = i + 1;
      r.part_total = group.length;
      ordered.push(r);
    });
  }
  return ordered;
}

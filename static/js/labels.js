// Label PDFs in the browser (the server's old C++ renderer is retired; this
// is the reference now). Layout, scaled to the label size:
//
//     [thumb]  6225242 (bold)            Qty: 150
//              LEGO: Medium Stone Grey
//              BL: Light Bluish Gray
//              BRICK 1X1X1 2/3 W/2 KNOBS
//          Person Name (bold, centered)    3 of 10
//
// layoutLabel() decides where everything goes (a pure function of the
// record, the label size, the options and a text measurer, afm.js's by
// default); tests/js/labels.test.mjs compares it with the checked-in
// tests/js/golden/labels.json. buildLabelsPdf() draws it with
// pdf-lib. Text is never cut off: a field too long for its line shrinks,
// then wraps, then shrinks further.
//
// Part photos are passed in (`images`: Map element id -> JPEG bytes, as
// fetched from /img/<id>.jpg); a missing or unreadable photo leaves the
// thumbnail space blank and the text as it is.

import { PDFDocument, StandardFonts, rgb } from './vendor/pdf-lib.js';
import qrcodegen from './vendor/qrcodegen.js';
import { isLight, isTransparent, swatchRgb } from './colors.js';
import { helveticaMeasure } from './afm.js';
import { centsText, formatMoney, winAnsi } from './reports.js';

export { helveticaMeasure };

export const MM_TO_PT = 72 / 25.4;

// Parts of a label that can be switched on and off; names match the CLI's
// LABEL_PARTS and label_options.h's kLabelPartNames.
export const LABEL_PARTS = [
  'photo', 'element_id', 'qty', 'lego_color', 'bl_color', 'description', 'name', 'count',
  'backdrop', 'swatch', 'qr',
];

// Which parts to draw: {part: bool}. Everything on except those listed in
// `hidden` (a comma-separated list or an array), as LabelOptions::
// from_hidden. Unknown names throw.
export function optionsFromHidden(hidden = '') {
  const list = Array.isArray(hidden) ? hidden : String(hidden).split(',');
  const show = Object.fromEntries(LABEL_PARTS.map((p) => [p, true]));
  for (const raw of list) {
    const name = raw.trim();
    if (!name) continue;
    if (!(name in show)) throw new Error(`unknown label part '${name}'`);
    show[name] = false;
  }
  return show;
}

// The price line, when `show.price` (the price per piece) or
// `show.lot_price` (this label's qty times it) is on and the record has a
// price: "$0.10 each · Lot $1.50". '' otherwise. These two aren't in
// LABEL_PARTS: they are off unless turned on (saved with the report
// options), so designs saved before them don't change.
export function priceLine(record, show) {
  if (record.price === null || record.price === undefined) return '';
  const sign = record.currency || '';
  const parts = [];
  if (show.price) parts.push(`${sign}${formatMoney(record.price)} each`);
  if (show.lot_price) {
    const lot = Number(record.qty.replaceAll(',', '')) * record.price;
    if (Number.isFinite(lot)) parts.push(`Lot ${centsText(lot, sign)}`);
  }
  return parts.join(' \u00b7 ');
}

// Where a label's QR code points: BrickLink's search, which resolves LEGO
// element IDs to the right part and color.
export function bricklinkUrl(elementId) {
  return `https://www.bricklink.com/v2/search.page?q=${elementId}`;
}

// Helvetica's cap height and descender, as fractions of the font size, and
// the baseline-to-baseline distance of wrapped lines.
const CAP_HEIGHT = 0.72;
const DESCENDER = 0.22;
const LEADING = 1.1;

// wrap_lines: at spaces; a word too wide for a line of its own is broken
// after a hyphen inside it ("Montgomery-" / "Smith"), and a piece still
// too wide is split between characters as a last resort. Always at least
// one line.
export function wrapLines(text, maxWidth, width) {
  const lines = [];
  let line = '';
  const place = (piece, glue) => {
    const candidate = line === '' ? piece : `${line}${glue}${piece}`;
    if (width(candidate) <= maxWidth) {
      line = candidate;
      return;
    }
    if (line !== '') {
      lines.push(line);
      line = '';
      if (width(piece) <= maxWidth) {
        line = piece;
        return;
      }
    }
    let rest = piece;
    while (rest !== '') {
      let n = 1;
      while (n < rest.length && width(rest.slice(0, n + 1)) <= maxWidth) ++n;
      if (n === rest.length) break;
      lines.push(rest.slice(0, n));
      rest = rest.slice(n);
    }
    line = rest;
  };
  for (const word of text.split(' ')) {
    const fitsAlone = width(word) <= maxWidth;
    if (fitsAlone || !word.includes('-')) place(word, ' ');
    else word.split(/(?<=-)(?=.)/).forEach((piece, k) => place(piece, k === 0 ? ' ' : ''));
  }
  if (line !== '' || lines.length === 0) lines.push(line);
  return lines;
}

// fitText: one line shrunk from maxSize down to minSize in
// half-point steps when that fits; otherwise wrapped, shrinking further
// until the lines fit maxHeight. Before any word would be split between
// letters, it shrinks (to 60% of minSize at most) until the longest word,
// or its hyphenated pieces, fit a line. Nothing is ever cut off.
// Returns {lines, size, leading}.
export function fitText(text, maxSize, minSize, maxWidth, maxHeight, width) {
  let size = maxSize;
  while (size > minSize && width(text, size) > maxWidth) size -= 0.5;
  if (width(text, size) <= maxWidth) return { lines: [text], size, leading: size * LEADING };
  size = minSize;
  const pieces = text.split(/[ ]|(?<=-)(?=.)/).filter(Boolean);
  const widest = (s) => Math.max(...pieces.map((p) => width(p, s)));
  while (size > minSize * 0.6 && widest(size) > maxWidth) size -= 0.25;
  for (;;) {
    const lines = wrapLines(text, maxWidth, (t) => width(t, size));
    const height = size * (CAP_HEIGHT + DESCENDER) + (lines.length - 1) * size * LEADING;
    if (height <= maxHeight || size <= 0.1) return { lines, size, leading: size * LEADING };
    size = Math.max(0.1, size > 2 ? size - 0.25 : size * 0.9);
  }
}

// Positions and font sizes scaled to the label's height (compute_layout).
export function computeLayout(width, height, show, nLines) {
  const L = {};
  L.pad = Math.min(height * 0.07, 9);
  const inner = height - 2 * L.pad;
  L.id_size = Math.min(inner * 0.2, 26);
  L.small = Math.min(inner * 0.12, 15);
  L.name_size = Math.min(inner * 0.22, 30);
  const topRow = show.element_id || show.qty;
  const nameRow = show.name || show.count;
  L.y_id = height - L.pad - L.id_size * 0.8;
  // The text lines (colors, price, description) between the top row and
  // the name: L.line is their size, L.small unless that many lines don't
  // fit above the name, when they shrink (to 55% at most) rather than run
  // into it.
  const firstAt = (s) => (topRow ? L.y_id - L.id_size * 0.2 - s * 1.25 : height - L.pad - s * 0.85);
  const floor = nameRow ? L.pad + L.name_size * 0.22 + L.name_size * 0.75 + L.small * 0.15 : L.pad;
  const bottomAt = (s) => firstAt(s) - (nLines - 1) * s * 1.2 - s * DESCENDER;
  L.line = L.small;
  while (nLines > 1 && L.line > L.small * 0.55 && bottomAt(L.line) < floor) L.line *= 0.95;
  const first = firstAt(L.line);
  L.lines = [];
  for (let i = 0; i < nLines; ++i) L.lines.push(first - i * L.line * 1.2);
  const lastText = L.lines.length ? L.lines[L.lines.length - 1] : topRow ? L.y_id : height - L.pad;
  L.y_name = L.pad + L.name_size * 0.22;
  let lift = 0;
  if (nameRow) {
    const slack = lastText - L.small * 0.3 - (L.y_name + L.name_size * 0.75);
    lift = Math.max(0, slack) * 0.45;
    L.y_name += lift;
  }
  const artHeight = inner - (nameRow ? L.name_size * 1.3 + lift : 0);
  L.img = show.photo ? Math.min(artHeight, width * 0.32) : 0;
  L.qr = show.qr ? Math.min(artHeight * 0.85, width * 0.17) : 0;
  L.text_x = L.img > 0 ? L.pad + L.img + L.pad * 0.8 : L.pad;
  L.text_right = width - L.pad - (L.qr > 0 ? L.qr + L.pad * 0.8 : 0);
  return L;
}

// What goes on one label, in label-local points (origin bottom-left):
//   {image: {x, y, size} | null, qr: {x, y, size, url} | null,
//    swatch: {x, y, side, rgb, trans} | null,
//    texts: [{font: 'regular'|'bold', size, x, y, text}]}
// `measure(font, text, size)` is the text's width in points. Text is
// already WinAnsi-safe (reports.winAnsi).
export function layoutLabel(record, width, height, show, measure) {
  const regular = (t, s) => measure('regular', t, s);
  const bold = (t, s) => measure('bold', t, s);
  const colorLines = [];
  if (show.lego_color && record.lego_color) colorLines.push(winAnsi(`LEGO: ${record.lego_color}`));
  if (show.bl_color && record.bl_color) colorLines.push(winAnsi(`BL: ${record.bl_color}`));
  const texts = colorLines.slice();
  const price = priceLine(record, show);
  if (price) texts.push(winAnsi(price));
  if (show.description && record.description) texts.push(winAnsi(record.description));
  const L = computeLayout(width, height, show, texts.length);
  const out = { image: null, qr: null, swatch: null, texts: [] };
  const draw = (font, size, x, y, text) => out.texts.push({ font, size, x, y, text });

  if (L.img > 0) out.image = { x: L.pad, y: height - L.pad - L.img, size: L.img };
  if (L.qr > 0) {
    out.qr = { x: width - L.pad - L.qr, y: height - L.pad - L.qr, size: L.qr, url: bricklinkUrl(record.element_id) };
  }

  const textX = L.text_x;
  const right = L.text_right;
  const textMax = right - textX;

  // Element ID and qty share the top row; both shrink together until they
  // fit — the ID is never cut short.
  const idText = show.element_id ? winAnsi(record.element_id) : '';
  const qtyText = show.qty ? winAnsi(`Qty: ${record.qty}`) : '';
  const rowWidth = (k) =>
    (idText === '' ? 0 : bold(idText, L.id_size * k)) +
    (qtyText === '' ? 0 : bold(qtyText, L.id_size * 0.85 * k)) +
    (idText !== '' && qtyText !== '' ? L.pad : 0);
  let scale = 1;
  while (scale > 0.4 && rowWidth(scale) > textMax) scale -= 0.05;
  while (scale > 0.01 && rowWidth(scale) > textMax) scale *= 0.9;
  if (qtyText !== '') {
    const size = L.id_size * 0.85 * scale;
    draw('bold', size, right - bold(qtyText, size), L.y_id, qtyText);
  }
  if (idText !== '') draw('bold', L.id_size * scale, textX, L.y_id, idText);

  // Swatch: a square of the part's color beside the color name lines.
  let swatchW = 0;
  const color = show.swatch ? swatchRgb(record.lego_color, record.bl_color) : null;
  if (color && colorLines.length) {
    const side = L.line * 1.2 * (colorLines.length - 1) + L.line * 0.95;
    const bottom = L.lines[colorLines.length - 1] - L.line * 0.22;
    out.swatch = { x: textX, y: bottom, side, rgb: color, trans: isTransparent(record.lego_color, record.bl_color) };
    swatchW = side + L.pad * 0.5;
  }

  const nameRow = show.name || show.count;
  texts.forEach((text, i) => {
    const x = textX + (i < colorLines.length ? swatchW : 0);
    const top = L.lines[i] + L.line * CAP_HEIGHT;
    let bottom = L.lines[i] - L.line * (1.2 - CAP_HEIGHT);
    if (i + 1 === texts.length) {
      const limit = nameRow ? L.y_name + L.name_size * 0.75 + L.small * 0.15 : L.pad;
      bottom = Math.min(bottom, limit);
    }
    const fit = fitText(text, L.line, L.line * 0.7, right - x, top - bottom, regular);
    if (fit.lines.length === 1) {
      draw('regular', fit.size, x, L.lines[i], fit.lines[0]);
      return;
    }
    let baseline = top - fit.size * CAP_HEIGHT;
    for (const line of fit.lines) {
      draw('regular', fit.size, x, baseline, line);
      baseline -= fit.leading;
    }
  });

  let counterW = 0;
  if (show.count && record.part_total > 0) {
    const counter = `${record.part_seq} of ${record.part_total}`;
    counterW = bold(counter, L.small);
    draw('bold', L.small, width - L.pad - counterW, L.y_name, counter);
  }

  if (show.name) {
    // Centered on the label, clear of the counter on both sides; when it
    // doesn't fit that way at full size, centered in all the room left of
    // the counter instead, if that gives it a bigger size or fewer lines.
    const top = L.y_name + L.name_size * CAP_HEIGHT;
    const name = winAnsi(record.person);
    const fitIn = (w) => fitText(name, L.name_size, L.name_size * 0.55, w, top - L.pad * 0.5, bold);
    let fit = fitIn(width - 2 * L.pad - 2 * (counterW + L.pad));
    let center = width / 2;
    if (counterW > 0 && (fit.lines.length > 1 || fit.size < L.name_size)) {
      const room = width - 2 * L.pad - counterW - L.pad;
      const wide = fitIn(room);
      if (wide.lines.length < fit.lines.length || wide.size > fit.size) {
        fit = wide;
        center = L.pad + room / 2;
      }
    }
    let baseline = fit.lines.length === 1 ? L.y_name : top - fit.size * CAP_HEIGHT;
    for (const line of fit.lines) {
      draw('bold', fit.size, center - bold(line, fit.size) / 2, baseline, line);
      baseline -= fit.leading;
    }
  }
  return out;
}

// Where each label goes on a page, in page points: [{x, y, w, h}] for
// slots [0, count) (for_each_slot).
export function pageSlots(spec, count) {
  const pageH = spec.sheet_height_mm * MM_TO_PT;
  const w = spec.label_width_mm * MM_TO_PT;
  const h = spec.label_height_mm * MM_TO_PT;
  const out = [];
  for (let slot = 0; slot < count; ++slot) {
    const col = slot % spec.columns;
    const row = Math.floor(slot / spec.columns);
    const x = spec.left_margin_mm * MM_TO_PT + col * (w + spec.column_gap_mm * MM_TO_PT);
    const yTop = pageH - spec.top_margin_mm * MM_TO_PT - row * (h + spec.row_gap_mm * MM_TO_PT);
    out.push({ x, y: yTop - h, w, h });
  }
  return out;
}

export const perSheet = (spec) => spec.columns * spec.rows;

// The records a run draws: all of them, or the first maxPages pages' worth.
export function recordsFor(records, spec, maxPages = 0) {
  return maxPages > 0 ? records.slice(0, maxPages * perSheet(spec)) : records;
}

// The QR matrix for `text` (ECC medium): rows of booleans,
// row 0 at the top.
export function qrMatrix(text) {
  const qr = qrcodegen.QrCode.encodeText(text, qrcodegen.QrCode.Ecc.MEDIUM);
  const rows = [];
  for (let y = 0; y < qr.size; ++y) {
    const row = [];
    for (let x = 0; x < qr.size; ++x) row.push(qr.getModule(x, y));
    rows.push(row);
  }
  return rows;
}

async function embedFonts(doc) {
  return {
    regular: await doc.embedFont(StandardFonts.Helvetica),
    bold: await doc.embedFont(StandardFonts.HelveticaBold),
  };
}

const BLACK = rgb(0, 0, 0);
const GRAY = rgb(0.35, 0.35, 0.35);

function drawLabel(page, ox, oy, lay, fonts, image) {
  if (lay.image && image) {
    page.drawImage(image, { x: ox + lay.image.x, y: oy + lay.image.y, width: lay.image.size, height: lay.image.size });
  }
  if (lay.qr) {
    const m = qrMatrix(lay.qr.url);
    const n = m.length;
    const s = lay.qr.size / n;
    for (let row = 0; row < n; ++row) {
      for (let col = 0; col < n; ++col) {
        if (!m[row][col]) continue;
        // Slight overlap so adjacent modules don't show hairline gaps.
        page.drawRectangle({
          x: ox + lay.qr.x + col * s, y: oy + lay.qr.y + (n - 1 - row) * s,
          width: s * 1.02, height: s * 1.02, color: BLACK,
        });
      }
    }
  }
  if (lay.swatch) {
    const { x, y, side, rgb: c, trans } = lay.swatch;
    page.drawRectangle({
      x: ox + x, y: oy + y, width: side, height: side,
      color: rgb(c[0], c[1], c[2]), borderColor: GRAY, borderWidth: 0.5,
    });
    if (trans) {
      // See-through colors get a diagonal, like a pane of glass.
      page.drawLine({ start: { x: ox + x, y: oy + y }, end: { x: ox + x + side, y: oy + y + side }, thickness: 0.5, color: GRAY });
    }
  }
  for (const t of lay.texts) {
    page.drawText(t.text, { x: ox + t.x, y: oy + t.y, size: t.size, font: fonts[t.font], color: BLACK });
  }
}

// The photo to draw for a record, embedded once per element and variant:
// the product photo, or for light parts a copy on a gray tile. `prepare`
// (optional, async; (bytes, trans, light, elementId)) turns JPEG bytes
// into the backdrop JPEG, or null to use the photo as it is.
async function labelImage(doc, record, show, images, cache, prepare) {
  const bytes = images && images.get(record.element_id);
  if (!bytes || !bytes.length) return null;
  const trans = isTransparent(record.lego_color, record.bl_color);
  const light = isLight(record.lego_color, record.bl_color);
  const key = show.backdrop ? `${record.element_id}|${trans}|${light}` : record.element_id;
  if (cache.has(key)) return cache.get(key);
  let img = null;
  try {
    let jpeg = bytes;
    if (show.backdrop && prepare) jpeg = (await prepare(bytes, trans, light, record.element_id)) || bytes;
    img = await doc.embedJpg(jpeg);
  } catch {
    img = null; // corrupt/unreadable image: skip the thumbnail, keep the text
  }
  cache.set(key, img);
  return img;
}

// The labels PDF (Uint8Array). Records are drawn in the order given, one
// per slot; a null in the list leaves its slot empty (packing.js uses that to
// keep parts on one sheet).
// opts: {show (optionsFromHidden), images, maxPages, prepareImage,
// onProgress({phase: 'pages', page, pages})}.
export async function buildLabelsPdf(records, spec, opts = {}) {
  const show = opts.show || optionsFromHidden('qr');
  const used = recordsFor(records, spec, opts.maxPages || 0);
  const doc = await PDFDocument.create();
  const fonts = await embedFonts(doc);
  const cache = new Map();
  const per = perSheet(spec);
  const pageW = spec.sheet_width_mm * MM_TO_PT;
  const pageH = spec.sheet_height_mm * MM_TO_PT;
  const pages = used.length ? Math.ceil(used.length / per) : 1;
  let idx = 0;
  for (let p = 0; p < pages; ++p) {
    if (opts.onProgress) opts.onProgress({ phase: 'pages', page: p + 1, pages });
    const page = doc.addPage([pageW, pageH]);
    for (const slot of pageSlots(spec, Math.min(per, used.length - idx))) {
      const record = used[idx++];
      if (!record) continue;
      const lay = layoutLabel(record, slot.w, slot.h, show, helveticaMeasure);
      const image = lay.image ? await labelImage(doc, record, show, opts.images, cache, opts.prepareImage) : null;
      drawLabel(page, slot.x, slot.y, lay, fonts, image);
    }
  }
  return doc.save();
}

// build_test_page: every label position outlined and numbered, to print
// on plain paper and hold against the stock to check alignment.
export async function buildTestPage(spec) {
  const doc = await PDFDocument.create();
  const font = await doc.embedFont(StandardFonts.Helvetica);
  const page = doc.addPage([spec.sheet_width_mm * MM_TO_PT, spec.sheet_height_mm * MM_TO_PT]);
  const size = `${(spec.label_height_mm / 25.4).toFixed(2)}" x ${(spec.label_width_mm / 25.4).toFixed(2)}"`;
  pageSlots(spec, perSheet(spec)).forEach(({ x, y, w, h }, slot) => {
    // Label outline (rounded corners, radius 4) and a crosshair at its centre.
    const r = 4;
    const [x0, y0, x1, y1] = [x + 0.5, y + 0.5, x + w - 0.5, y + h - 0.5];
    const path = `M ${x0 + r} ${-y0} L ${x1 - r} ${-y0} Q ${x1} ${-y0} ${x1} ${-(y0 + r)} ` +
      `L ${x1} ${-(y1 - r)} Q ${x1} ${-y1} ${x1 - r} ${-y1} L ${x0 + r} ${-y1} ` +
      `Q ${x0} ${-y1} ${x0} ${-(y1 - r)} L ${x0} ${-(y0 + r)} Q ${x0} ${-y0} ${x0 + r} ${-y0} Z`;
    page.drawSvgPath(path, { x: 0, y: 0, borderColor: BLACK, borderWidth: 0.75 });
    const cx = x + w / 2;
    const cy = y + h / 2;
    page.drawLine({ start: { x: cx - 6, y: cy }, end: { x: cx + 6, y: cy }, thickness: 0.5, color: BLACK });
    page.drawLine({ start: { x: cx, y: cy - 6 }, end: { x: cx, y: cy + 6 }, thickness: 0.5, color: BLACK });
    const textSize = Math.min(h * 0.12, 10);
    const text = winAnsi(`${spec.brand} ${spec.part} #${slot + 1}  ${size}`);
    const tw = helveticaMeasure('regular', text, textSize);
    page.drawText(text, { x: x + (w - tw) / 2, y: cy - textSize * 2.2, size: textSize, font, color: BLACK });
  });
  return doc.save();
}

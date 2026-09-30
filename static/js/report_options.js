// Settings for the packing checklist, parts list and lot counts, and which
// files go in the "Download selected" zip. Pure: no DOM, no storage.
//
// The dashboard keeps one options object per open sheet: in localStorage
// for uploads, and with the sheet's label design on the server for saved
// Google Sheets (PUT /sheets/<id>/design's `report_options`, stored as
// sent). Anything read back from either place goes through
// normalizeOptions(), which keeps only known keys, clamps every value to
// what it allows and fills in the defaults. The defaults give the same
// reports the server makes.

// Longest custom title / subtitle, in characters. Keeps the saved JSON well
// under the server's 4 KB limit (MAX_OPTIONS_BYTES) even in the worst case.
export const MAX_TITLE = 80;
export const MAX_SUBTITLE = 120;
export const MAX_MIN_LOTS = 9999;

// The server refuses a report_options body over this many bytes.
export const MAX_OPTIONS_BYTES = 4096;

export const PAPERS = ['letter', 'a4'];
export const ORIENTATIONS = ['portrait', 'landscape'];
export const PERSON_SORTS = ['last', 'first'];
// 'labels' = the label design's part order.
export const CHECKLIST_ORDERS = ['labels', 'heaviest', 'lightest', 'sheet'];
export const PARTS_ORDERS = ['labels', 'heaviest', 'lightest', 'sheet', 'element'];
export const CHECKLIST_LAYOUTS = ['page', 'continuous'];

// Files the zip can hold, in the order they go in it.
export const ZIP_FILES = [
  ['labels', 'Labels (PDF)'],
  ['checklist_pdf', 'Packing checklist (PDF)'],
  ['parts_pdf', 'Parts list (PDF)'],
  ['parts_csv', 'Parts list (CSV)'],
  ['lots_pdf', 'Lot counts (PDF)'],
  ['lots_csv', 'Lot counts (CSV)'],
  ['check_txt', 'Sheet check (.txt)'],
];

const PAGE = { title: '', subtitle: '', paper: 'letter', orientation: 'portrait' };

export const DEFAULTS = Object.freeze({
  checklist: Object.freeze({
    ...PAGE,
    sort: 'last',
    order: 'labels',
    layout: 'page', // a new page per person
    color: true,
    weight: false,
    photo: false,
    checkbox: true,
    packed_by: false,
  }),
  parts: Object.freeze({
    ...PAGE,
    order: 'labels',
    photo: false,
    lego_color: true,
    bl_color: true,
    pieces: true,
    people: true,
    weight: true,
    total_weight: false,
    group_by_color: false,
  }),
  lots: Object.freeze({
    ...PAGE,
    sort: 'last',
    lots: true,
    pieces: true,
    total_weight: false,
    totals: false,
    min_lots: 0,
  }),
  zip: Object.freeze(Object.fromEntries(ZIP_FILES.map(([id]) => [id, true]))),
});

const CHOICES = {
  paper: PAPERS,
  orientation: ORIENTATIONS,
  sort: PERSON_SORTS,
  layout: CHECKLIST_LAYOUTS,
};

const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);

// A title or subtitle: a string with control characters turned into spaces,
// runs of spaces collapsed, trimmed and cut to `max` characters (code points).
export function cleanText(v, max) {
  if (typeof v !== 'string') return '';
  const s = v.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, ' ').replace(/ {2,}/g, ' ').trim();
  return [...s].slice(0, max).join('').trim();
}

// A whole number in [lo, hi]; anything else (NaN, strings that aren't
// numbers, objects) gives `fallback`.
export function clampInt(v, lo, hi, fallback) {
  const n = typeof v === 'string' && v.trim() !== '' ? Number(v) : v;
  if (typeof n !== 'number' || !Number.isFinite(n)) return fallback;
  return Math.min(hi, Math.max(lo, Math.trunc(n)));
}

function normalizeSection(name, raw) {
  const defaults = DEFAULTS[name];
  const src = isObject(raw) ? raw : {};
  const out = {};
  for (const [key, def] of Object.entries(defaults)) {
    const v = Object.hasOwn(src, key) ? src[key] : undefined;
    if (typeof def === 'boolean') {
      out[key] = typeof v === 'boolean' ? v : def;
    } else if (key === 'title') {
      out[key] = cleanText(v, MAX_TITLE);
    } else if (key === 'subtitle') {
      out[key] = cleanText(v, MAX_SUBTITLE);
    } else if (key === 'min_lots') {
      out[key] = clampInt(v, 0, MAX_MIN_LOTS, def);
    } else if (key === 'order') {
      const allowed = name === 'parts' ? PARTS_ORDERS : CHECKLIST_ORDERS;
      out[key] = allowed.includes(v) ? v : def;
    } else {
      out[key] = CHOICES[key].includes(v) ? v : def;
    }
  }
  return out;
}

// A complete, valid options object from anything (parsed JSON, a partial
// object, null...). Never throws.
export function normalizeOptions(raw) {
  const src = isObject(raw) ? raw : {};
  const out = {};
  for (const name of Object.keys(DEFAULTS)) out[name] = normalizeSection(name, src[name]);
  return out;
}

// normalizeOptions() of a JSON string; bad JSON gives the defaults.
export function parseOptions(json) {
  if (typeof json !== 'string' || json.length > MAX_OPTIONS_BYTES * 4) return normalizeOptions(null);
  try {
    return normalizeOptions(JSON.parse(json));
  } catch {
    return normalizeOptions(null);
  }
}

// The JSON to save (normalized first, so it is always within the limits).
export function serializeOptions(options) {
  return JSON.stringify(normalizeOptions(options));
}

// Page size in PDF points (1/72 in), [width, height].
export function pageSize(section) {
  const [w, h] = section.paper === 'a4' ? [595.28, 841.89] : [612, 792];
  return section.orientation === 'landscape' ? [h, w] : [w, h];
}

// The zip entries chosen, in ZIP_FILES order.
export function zipSelection(options) {
  const zip = normalizeOptions(options).zip;
  return ZIP_FILES.map(([id]) => id).filter((id) => zip[id]);
}

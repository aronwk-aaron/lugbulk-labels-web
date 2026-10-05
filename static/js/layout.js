// "Order Here" tab layout constants and label stock lookup — the browser
// port of src/sheet_layout.{h,cpp} (see there for the reasoning).

export const SOURCE_TAB = 'Order Here';

// Front-matter columns are found by header text (first match wins, in
// priority order). The 0-indexed positions are only the fallback for a
// sheet with no recognizable header row at all.
export const ELEMENT_ID_HEADERS = ['Element ID', 'Part Number'];
export const DESCRIPTION_HEADERS = ['Description'];
export const LEGO_COLOR_HEADERS = ['LEGO Color', 'LEGO Colour'];
export const BL_COLOR_HEADERS = ['BL Color', 'BrickLink Color', 'BL Colour', 'Color'];
export const WEIGHT_HEADERS = ['Weight', 'Weight (g)', 'Weight g'];
// The price paid per piece (the multi-year price views). Matched with runs
// of whitespace as one space ("Cost\nEach" counts). Not "BL Price" or
// "B&P Price": those are reference prices, not what was paid.
export const PRICE_HEADERS = ['Price', 'Cost Each', 'Unit Price', 'Price Each', 'Cost Per Piece', 'Each'];

export const COL_ELEMENT_ID = 1;
export const COL_DESCRIPTION = 3;
export const COL_COLOR = 4; // "BL Color"
export const HEADER_ROW = 0; // person names live here
export const HEADER_SEARCH_ROWS = 10;

// In the "qty marker" layout, the cell below a person's header reads "qty".
export const QTY_MARKER = 'qty';

export const DEFAULT_LABEL_SPEC_ID = 'avery5162';

// A part photo, through this app's own cache (GET /img/<id>.jpg) rather
// than LEGO's CDN directly — same-origin, so the page's CSP allows it and
// canvas/PDF rendering can read the pixels. `elementId` is digits only
// (isValidElementId), so it's safe in a path.
export function imageUrlFor(elementId) {
  return `/img/${elementId}.jpg`;
}

function normalize(s) {
  return s.replace(/[^A-Za-z0-9]/g, '').toLowerCase();
}

// Builds a stock lookup over the /label-specs.json document ({specs:[...]}):
// by id, "Avery 8162", an equivalent part number, or a bare part number
// ("5162"), like find_label_spec. Returns name -> spec or null.
export function labelSpecFinder(specsDoc) {
  const specs = specsDoc.specs;
  const aliases = new Map();
  const add = (key, i) => {
    if (!aliases.has(key)) aliases.set(key, i);
  };
  specs.forEach((s, i) => add(s.id, i));
  specs.forEach((s, i) => {
    for (const part of [...s.equivalents, s.part]) {
      add(normalize(s.brand + part), i);
      add(normalize(part), i); // bare part number
    }
  });
  return (name) => {
    const i = aliases.get(normalize(name));
    return i === undefined ? null : specs[i];
  };
}

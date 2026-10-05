// Loads and checks an order sheet in the browser — what the server's
// /upload/check and /sheets/:id/check do, done here instead:
//
//   - an uploaded file (a File or Blob) is read and parsed right here
//     (spreadsheet.js); it isn't sent anywhere for this;
//   - a saved Google Sheet's cells come from GET /sheets/<id>/values.
//
// Then the rows are pivoted, BrickLink data is added (POST
// /bricklink/lookup) and the run size checked. Resolves to
// {records, issues, summary} — summary is the "Check sheet" JSON shape
// ({labels, people, parts, issues}) — or rejects with a LoadError whose
// message is meant for the user.

import { SOURCE_TAB } from './layout.js';
import * as records from './records.js';
import { SpreadsheetError, readTabs } from './spreadsheet.js';

// The server's request body limit for uploads, kept for files read here
// too: they are still sent for previews and downloads.
export const MAX_UPLOAD_BYTES = 10 * 1024 * 1024;

// Element ids per POST /bricklink/lookup (the server's limit).
export const LOOKUP_BATCH = 2000;

export class LoadError extends Error {
  constructor(message, status = 0) {
    super(message);
    this.name = 'LoadError';
    this.status = status; // HTTP status of a failed request, else 0
  }
}

// A failed response's plain-text message (the server's are meant for users).
async function responseError(res) {
  const text = await res.text().catch(() => '');
  return new LoadError(text.trim() || `Request failed (${res.status}) — try again.`, res.status);
}

const isBlob = (x) => x && typeof x.arrayBuffer === 'function' && typeof x.size === 'number';

async function uploadTabs(file) {
  if (file.size === 0) throw new LoadError('Choose an .xlsx or .csv file first.');
  if (file.size > MAX_UPLOAD_BYTES) throw new LoadError('That file is over 10 MB — too big to upload.');
  const bytes = new Uint8Array(await file.arrayBuffer());
  try {
    return await readTabs(bytes, SOURCE_TAB);
  } catch (e) {
    if (e instanceof SpreadsheetError) throw new LoadError(e.message);
    throw new LoadError("Couldn't read that file — try again.");
  }
}

async function sheetRows(rowId, fetchFn) {
  let res;
  try {
    res = await fetchFn(`/sheets/${encodeURIComponent(rowId)}/values`, { credentials: 'same-origin' });
  } catch (e) {
    if (e instanceof LoadError) throw e;
    throw new LoadError("Couldn't reach the server — check your connection and try again.");
  }
  if (!res.ok) throw await responseError(res);
  const data = await res.json().catch(() => null);
  if (!data || !Array.isArray(data.rows)) throw new LoadError('The server sent an unexpected answer — try again.');
  return data.rows;
}

// {"<id>": {part, color, weight}} for the ids, in batches of LOOKUP_BATCH.
export async function lookupBricklink(ids, fetchFn = globalThis.fetch) {
  const lookup = {};
  for (let i = 0; i < ids.length; i += LOOKUP_BATCH) {
    const res = await fetchFn('/bricklink/lookup', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ids: ids.slice(i, i + LOOKUP_BATCH) }),
    });
    if (!res.ok) throw await responseError(res);
    Object.assign(lookup, await res.json());
  }
  return lookup;
}

// source: a File/Blob (an upload) or {rowId} (a saved Google Sheet).
// options.fetch: the fetch to use (the dashboard's, which handles sign-in).
//
// Resolves to {records, issues, summary, lookupError}. BrickLink data is
// a nicety, as on the server: if the lookup fails the sheet still loads,
// without it, and lookupError says why.
export async function loadSheet(source, options = {}) {
  const fetchFn = options.fetch ?? globalThis.fetch.bind(globalThis);
  let pivot;
  try {
    if (isBlob(source)) {
      pivot = records.pivotTabs(await uploadTabs(source));
    } else if (source && source.rowId !== undefined && source.rowId !== null) {
      pivot = records.pivotRows(await sheetRows(source.rowId, fetchFn));
    } else {
      throw new LoadError('Choose an .xlsx or .csv file first.');
    }
  } catch (e) {
    // The server's wording for a sheet over the per-run limits (HTTP 413).
    if (e instanceof records.TooBigError) throw new LoadError(`Too big: ${e.message}.`, 413);
    throw e;
  }

  pivot = records.addPrices(pivot, pivot.rows);

  let lookupError = null;
  const ids = records.elementIds(pivot);
  if (ids.length) {
    try {
      pivot = records.applyBricklink(pivot, await lookupBricklink(ids, fetchFn));
    } catch (e) {
      lookupError = (e && e.message) || 'BrickLink data is unavailable.';
    }
  }
  return {
    records: pivot.records,
    issues: pivot.issues,
    summary: records.checkSummary(pivot),
    lookupError,
  };
}

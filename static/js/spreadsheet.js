// Reads an order sheet file — an .xlsx workbook or a .csv — into rows of
// strings, the shape GET /sheets/:id/values returns: the browser port of
// src/spreadsheet.{h,cpp}, with the same limits and error messages
// (tests/js/parity.test.mjs checks the two agree on every fixture).
//
// Files are untrusted, so reading is bounded: an .xlsx (a zip of XML
// files) may not expand past MAX_UNPACKED_BYTES — a zip bomb is refused,
// not unpacked. Zip entries are inflated with DecompressionStream
// ('deflate-raw'), which browsers and Node 21+ both have. The XML is read
// with the same small tag scanner the C++ uses rather than DOMParser (Node
// has none): the SpreadsheetML parts needed are flat and regular.
//
// Like the C++, which works on bytes, the zip and XML are scanned as
// "binary strings" (one char per byte), and cell text is decoded from
// UTF-8 at the end, so offsets, limits and entity handling match exactly.

export const MAX_UNPACKED_BYTES = 64 * 1024 * 1024;

// Not a readable spreadsheet (the message is safe to show the user).
export class SpreadsheetError extends Error {
  constructor(message) {
    super(message);
    this.name = 'SpreadsheetError';
  }
}

const DAMAGED = 'That .xlsx file is damaged.';
const NOT_XLSX = "That file isn't a valid .xlsx workbook.";
const TOO_BIG = 'That .xlsx file unpacks to more than 64 MB — too big.';

const C_SPACE = ' \t\n\v\f\r'; // isspace() in the "C" locale

function bytesOf(data) {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  throw new TypeError('expected bytes (Uint8Array or ArrayBuffer)');
}

// Bytes -> a string with one char (0-255) per byte. (TextDecoder('latin1')
// won't do: it is windows-1252, which remaps 0x80-0x9f.)
function binary(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return out;
}

const utf8Decoder = new TextDecoder('utf-8', { ignoreBOM: true });

// A binary string holding UTF-8 -> text.
function utf8(s) {
  if (!/[\x80-\xff]/.test(s)) return s;
  const bytes = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i);
  return utf8Decoder.decode(bytes);
}

// --- zip -------------------------------------------------------------------

function u16(d, at) {
  if (at + 2 > d.length) throw new SpreadsheetError(DAMAGED);
  return d[at] | (d[at + 1] << 8);
}
function u32(d, at) {
  return (u16(d, at) | (u16(d, at + 2) << 16)) >>> 0;
}

function centralDirectory(d) {
  // End of central directory: signature 0x06054b50 within the last 64 KB.
  if (d.length < 22) throw new SpreadsheetError(NOT_XLSX);
  const min = d.length > 65557 ? d.length - 65557 : 0;
  let eocd = -1;
  for (let i = d.length - 22; ; --i) {
    if (u32(d, i) === 0x06054b50) {
      eocd = i;
      break;
    }
    if (i === min) break;
  }
  if (eocd < 0) throw new SpreadsheetError(NOT_XLSX);
  const count = u16(d, eocd + 10);
  const offset = u32(d, eocd + 16);
  if (count > 10000) throw new SpreadsheetError('That .xlsx file has too many parts.');
  const entries = [];
  let at = offset;
  for (let i = 0; i < count; ++i) {
    if (u32(d, at) !== 0x02014b50) throw new SpreadsheetError(DAMAGED);
    const nameLen = u16(d, at + 28);
    const extra = u16(d, at + 30);
    const comment = u16(d, at + 32);
    const e = {
      method: u16(d, at + 10),
      compressed: u32(d, at + 20),
      size: u32(d, at + 24),
      localOffset: u32(d, at + 42),
      name: '',
    };
    if (at + 46 + nameLen > d.length) throw new SpreadsheetError(DAMAGED);
    e.name = binary(d.subarray(at + 46, at + 46 + nameLen));
    entries.push(e);
    at += 46 + nameLen + extra + comment;
  }
  return entries;
}

// Raw DEFLATE -> bytes, refusing (TOO_BIG) once the output passes the
// budget. Enforced on what actually comes out, not the header's claimed
// size — a zip bomb can lie about that.
async function inflateRaw(input, budget) {
  const stream = new DecompressionStream('deflate-raw');
  const writer = stream.writable.getWriter();
  writer.write(input).catch(() => {});
  writer.close().catch(() => {});
  const reader = stream.readable.getReader();
  const chunks = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.length > budget.left) throw new SpreadsheetError(TOO_BIG);
      budget.left -= value.length;
      total += value.length;
      chunks.push(value);
    }
  } catch (e) {
    reader.cancel().catch(() => {});
    if (e instanceof SpreadsheetError) throw e;
    throw new SpreadsheetError(DAMAGED); // corrupt or truncated data
  }
  const out = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) {
    out.set(c, at);
    at += c.length;
  }
  return out;
}

async function extract(d, e, budget) {
  const at = e.localOffset;
  if (u32(d, at) !== 0x04034b50) throw new SpreadsheetError(DAMAGED);
  const dataAt = at + 30 + u16(d, at + 26) + u16(d, at + 28);
  if (dataAt + e.compressed > d.length) throw new SpreadsheetError(DAMAGED);
  if (e.size > budget.left) throw new SpreadsheetError(TOO_BIG);
  const body = d.subarray(dataAt, dataAt + e.compressed);
  if (e.method === 0) {
    budget.left -= e.compressed;
    return binary(body);
  }
  if (e.method !== 8) throw new SpreadsheetError('That .xlsx file uses an unsupported compression.');
  return binary(await inflateRaw(body, budget));
}

// --- minimal XML scanning (on binary strings, like the C++ on bytes) -------

// strtoul(s, nullptr, base) cast to uint32_t.
function strtoul32(s, base) {
  let i = 0;
  while (i < s.length && C_SPACE.includes(s[i])) i++;
  let negative = false;
  if (s[i] === '+' || s[i] === '-') negative = s[i++] === '-';
  if (base === 16 && s[i] === '0' && (s[i + 1] === 'x' || s[i + 1] === 'X') &&
      /[0-9a-f]/i.test(s[i + 2] ?? '')) {
    i += 2;
  }
  const digits = (base === 16 ? /^[0-9a-f]+/i : /^[0-9]+/).exec(s.slice(i));
  if (!digits) return 0;
  let v = BigInt((base === 16 ? '0x' : '') + digits[0]);
  const ULONG_MAX = (1n << 64n) - 1n;
  if (v > ULONG_MAX) return 0xffffffff; // ERANGE: ULONG_MAX, whatever the sign
  if (negative) v = (1n << 64n) - v;
  return Number(v & 0xffffffffn);
}

// A code point as UTF-8 bytes, the C++'s way (no checks: it's what the file says).
function utf8Bytes(cp) {
  const b = (x) => String.fromCharCode(x & 0xff);
  if (cp < 0x80) return b(cp);
  if (cp < 0x800) return b(0xc0 | (cp >>> 6)) + b(0x80 | (cp & 0x3f));
  if (cp < 0x10000) return b(0xe0 | (cp >>> 12)) + b(0x80 | ((cp >>> 6) & 0x3f)) + b(0x80 | (cp & 0x3f));
  return b(0xf0 | (cp >>> 18)) + b(0x80 | ((cp >>> 12) & 0x3f)) + b(0x80 | ((cp >>> 6) & 0x3f)) +
    b(0x80 | (cp & 0x3f));
}

const NAMED = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };

function decodeEntities(s) {
  if (!s.includes('&')) return s;
  let out = '';
  for (let i = 0; i < s.length; ++i) {
    if (s[i] !== '&') {
      out += s[i];
      continue;
    }
    const semi = s.indexOf(';', i);
    if (semi < 0 || semi - i > 10) {
      out += '&';
      continue;
    }
    const ent = s.slice(i + 1, semi);
    let cp;
    if (Object.hasOwn(NAMED, ent)) {
      cp = NAMED[ent].charCodeAt(0);
    } else if (ent !== '' && ent[0] === '#') {
      const hex = ent[1] === 'x';
      cp = strtoul32(ent.slice(hex ? 2 : 1), hex ? 16 : 10);
    } else {
      out += s.slice(i, semi + 1);
      i = semi;
      continue;
    }
    out += utf8Bytes(cp);
    i = semi;
  }
  return out;
}

const isSpace = (c) => c !== undefined && C_SPACE.includes(c);

// Value of attribute `name` in a start tag (the text between '<' and '>').
function attr(tag, name) {
  let at = 0;
  while ((at = tag.indexOf(`${name}=`, at)) >= 0) {
    const boundary = at === 0 || isSpace(tag[at - 1]);
    const q = at + name.length + 1;
    if (boundary && q < tag.length && (tag[q] === '"' || tag[q] === "'")) {
      const end = tag.indexOf(tag[q], q + 1);
      if (end < 0) return null;
      return decodeEntities(tag.slice(q + 1, end));
    }
    at = q;
  }
  return null;
}

// Calls f(tagText, body) for each <name ...>body</name> (or <name .../>)
// element. Matches the local name, with or without a namespace prefix.
function eachElement(xml, name, f) {
  let at = 0;
  while ((at = xml.indexOf('<', at)) >= 0) {
    const nameStart = at + 1;
    const tagEnd = xml.indexOf('>', at);
    if (tagEnd < 0) return;
    let nameEnd = nameStart;
    while (nameEnd < xml.length && !' \t\r\n/>'.includes(xml[nameEnd])) nameEnd++;
    const qname = xml.slice(nameStart, nameEnd);
    const colon = qname.indexOf(':');
    const local = colon < 0 ? qname : qname.slice(colon + 1);
    if (local !== name || xml[nameStart] === '/') {
      at = tagEnd + 1;
      continue;
    }
    const tag = xml.slice(nameStart, tagEnd);
    if (tag.endsWith('/')) {
      f(tag, '');
      at = tagEnd + 1;
      continue;
    }
    const close = `</${qname}>`;
    const bodyEnd = xml.indexOf(close, tagEnd + 1);
    if (bodyEnd < 0) return;
    f(tag, xml.slice(tagEnd + 1, bodyEnd));
    at = bodyEnd + close.length;
  }
}

// All <t> text inside `xml`, concatenated (a rich-text string is several runs).
function textOf(xml) {
  let out = '';
  eachElement(xml, 't', (_, body) => {
    out += decodeEntities(body);
  });
  return out;
}

// "BC12" -> 54 (0-based column)
function columnIndex(ref) {
  let col = 0;
  for (const c of ref) {
    if (!/[A-Za-z]/.test(c)) break;
    col = col * 26 + (c.toUpperCase().charCodeAt(0) - 64);
    if (col > 1e12) break; // far past the 2000-column cutoff either way
  }
  return col === 0 ? 0 : col - 1;
}

// std::atol: leading space, a sign, digits; 0 if none.
function atol(s) {
  const m = /^[ \t\n\v\f\r]*([+-]?[0-9]+)/.exec(s);
  return m ? Number(m[1]) : 0;
}

// strtod's longest numeric prefix of `s`, or null if there's none.
function strtodPrefix(s) {
  let i = 0;
  while (i < s.length && C_SPACE.includes(s[i])) i++;
  const t = s.slice(i);
  let m;
  if ((m = /^([+-]?)inf(inity)?/i.exec(t))) return m[1] === '-' ? -Infinity : Infinity;
  if (/^[+-]?nan/i.test(t)) return NaN;
  if ((m = /^([+-]?)0x(?:([0-9a-f]+)(?:\.([0-9a-f]*))?|\.([0-9a-f]+))(?:p([+-]?[0-9]+))?/i.exec(t))) {
    const int = m[2] ?? '';
    const frac = m[3] ?? m[4] ?? '';
    const exp = Number(m[5] ?? '0') - 4 * frac.length;
    const v = Number(BigInt(`0x${int}${frac}`)) * 2 ** exp;
    return m[1] === '-' ? -v : v;
  }
  if ((m = /^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)(e[+-]?[0-9]+)?/i.exec(t))) return Number(m[0]);
  return null;
}

// printf("%.15g", x), exactly: from the double's exact decimal value,
// rounded half-to-even as glibc does.
function formatG15(x) {
  if (Number.isNaN(x)) return 'nan';
  if (!Number.isFinite(x)) return x < 0 ? '-inf' : 'inf';
  const sign = x < 0 || Object.is(x, -0) ? '-' : '';
  if (x === 0) return `${sign}0`;
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, Math.abs(x));
  const bits = view.getBigUint64(0);
  const biased = Number(bits >> 52n);
  let mant = bits & ((1n << 52n) - 1n);
  let exp;
  if (biased === 0) {
    exp = -1074;
  } else {
    mant |= 1n << 52n;
    exp = biased - 1075;
  }
  // |x| = digits * 10^-k, exactly.
  let digits;
  let k = 0;
  if (exp >= 0) {
    digits = (mant << BigInt(exp)).toString();
  } else {
    k = -exp;
    digits = (mant * 5n ** BigInt(k)).toString();
  }
  let e10 = digits.length - 1 - k; // exponent of the leading digit
  const P = 15;
  let kept;
  if (digits.length > P) {
    kept = digits.slice(0, P);
    const rest = digits.slice(P);
    const up = rest[0] > '5' || (rest[0] === '5' && (/[1-9]/.test(rest.slice(1)) || Number(kept[P - 1]) % 2 === 1));
    if (up) {
      kept = (BigInt(kept) + 1n).toString();
      if (kept.length > P) {
        kept = kept.slice(0, P);
        e10 += 1;
      }
    }
  } else {
    kept = digits.padEnd(P, '0');
  }
  const strip = (s) => (s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s);
  if (e10 < -4 || e10 >= P) {
    const mantissa = strip(`${kept[0]}.${kept.slice(1)}`);
    const e = Math.abs(e10);
    return `${sign}${mantissa}e${e10 < 0 ? '-' : '+'}${e < 10 ? '0' : ''}${e}`;
  }
  if (e10 < 0) return `${sign}${strip(`0.${'0'.repeat(-e10 - 1)}${kept}`)}`;
  return `${sign}${strip(`${kept.slice(0, e10 + 1)}.${kept.slice(e10 + 1)}`)}`;
}

// Numbers as a person would type them: 4211407.0 -> "4211407", 0.83 -> "0.83".
function formatNumber(raw) {
  const v = strtodPrefix(raw);
  if (v === null) return raw;
  if (Number.isNaN(v)) return /^[ \t\n\v\f\r]*-/.test(raw) ? '-nan' : 'nan'; // glibc keeps NaN's sign
  if (Math.abs(v) < 1e15 && v === Math.floor(v)) return BigInt(v).toString();
  return formatG15(v);
}

function readSheet(xml, shared) {
  const rows = [];
  eachElement(xml, 'row', (rowTag, rowBody) => {
    let r = rows.length;
    const rref = attr(rowTag, 'r');
    if (rref !== null) r = Math.max(1, atol(rref)) - 1;
    if (r > 100000) return; // far beyond any real order sheet
    while (rows.length <= r) rows.push([]);
    const row = rows[r];
    eachElement(rowBody, 'c', (cTag, cBody) => {
      let col = row.length;
      const cref = attr(cTag, 'r');
      if (cref !== null) col = columnIndex(cref);
      if (col > 2000) return;
      const type = attr(cTag, 't') ?? 'n';
      let value;
      if (type === 'inlineStr') {
        value = textOf(cBody);
      } else {
        let v = '';
        eachElement(cBody, 'v', (_, body) => {
          v = decodeEntities(body);
        });
        if (type === 's') {
          const i = atol(v);
          value = i >= 0 && i < shared.length ? shared[i] : '';
        } else if (type === 'n') {
          value = v === '' ? '' : formatNumber(v);
        } else if (type === 'b') {
          value = v === '1' ? 'TRUE' : 'FALSE';
        } else {
          value = v; // "str" (formula result), "e" (error)
        }
      }
      while (row.length <= col) row.push('');
      row[col] = value;
    });
  });
  return rows.map((row) => row.map(utf8));
}

function normalize(s) {
  let out = '';
  for (const c of s) if (!C_SPACE.includes(c)) out += c >= 'A' && c <= 'Z' ? c.toLowerCase() : c;
  return out;
}

// ---- public ---------------------------------------------------------------

// True if `data` looks like an .xlsx (zip) rather than text.
export function isXlsx(data) {
  const d = bytesOf(data);
  return d.length >= 4 && d[0] === 0x50 && d[1] === 0x4b && d[2] === 0x03 && d[3] === 0x04;
}

// RFC 4180 CSV: quoted fields, doubled quotes, CR/LF line ends, optional
// UTF-8 BOM. Takes bytes (UTF-8) or a string.
export function readCsv(input) {
  let data = typeof input === 'string' ? input : utf8Decoder.decode(bytesOf(input));
  if (data.startsWith('﻿')) data = data.slice(1);
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;
  let any = false;
  for (let i = 0; i < data.length; ++i) {
    const c = data[i];
    if (quoted) {
      if (c === '"' && i + 1 < data.length && data[i + 1] === '"') {
        field += '"';
        ++i;
      } else if (c === '"') {
        quoted = false;
      } else {
        field += c;
      }
    } else if (c === '"') {
      quoted = true;
      any = true;
    } else if (c === ',') {
      row.push(field);
      field = '';
      any = true;
    } else if (c === '\n' || c === '\r') {
      if (c === '\r' && i + 1 < data.length && data[i + 1] === '\n') ++i;
      row.push(field);
      field = '';
      rows.push(row);
      row = [];
      any = false;
    } else {
      field += c;
      any = true;
    }
  }
  if (any || field !== '') {
    row.push(field);
    rows.push(row);
  }
  return rows;
}

// The rows of the workbook's order tab: the sheet named like `tab`
// (ignoring case and spaces, so "OrderHere" matches "Order Here"), or if
// there's none, every sheet in workbook order — the caller tries each.
// Throws SpreadsheetError.
export async function readXlsx(data, tab) {
  const d = bytesOf(data);
  const entries = centralDirectory(d);
  const budget = { left: MAX_UNPACKED_BYTES };
  const part = async (name) => {
    const e = entries.find((x) => x.name === name);
    return e ? extract(d, e, budget) : null;
  };

  const workbook = await part('xl/workbook.xml');
  const rels = await part('xl/_rels/workbook.xml.rels');
  if (workbook === null || rels === null) throw new SpreadsheetError("That file isn't an Excel workbook.");

  const shared = [];
  const sst = await part('xl/sharedStrings.xml');
  if (sst !== null) eachElement(sst, 'si', (_, body) => shared.push(textOf(body)));

  const targets = new Map(); // relationship id -> part path
  eachElement(rels, 'Relationship', (tag) => {
    const id = attr(tag, 'Id');
    const target = attr(tag, 'Target');
    if (id === null || target === null) return;
    let path = target;
    if (path.startsWith('/')) path = path.slice(1);
    else if (!path.startsWith('xl/')) path = `xl/${path}`;
    targets.set(id, path);
  });

  const sheets = []; // [name, part path]
  eachElement(workbook, 'sheet', (tag) => {
    const name = attr(tag, 'name');
    const rid = attr(tag, 'r:id');
    if (name !== null && rid !== null && targets.has(rid)) sheets.push([name, targets.get(rid)]);
  });
  if (!sheets.length) throw new SpreadsheetError('That workbook has no sheets.');

  // The tab name is compared as UTF-8 bytes, like the sheet names.
  const want = normalize(binary(new TextEncoder().encode(tab)));
  const out = [];
  for (const [name, path] of sheets) {
    if (normalize(name) === want) {
      const xml = await part(path);
      if (xml !== null) out.push(readSheet(xml, shared));
      return out;
    }
  }
  for (const [, path] of sheets) {
    const xml = await part(path);
    if (xml !== null) out.push(readSheet(xml, shared));
  }
  return out;
}

// An uploaded file's tabs, as the server's pivot_upload reads them: a
// workbook's order tab (or every tab), or a CSV's one. Throws
// SpreadsheetError.
export async function readTabs(data, tab) {
  return isXlsx(data) ? readXlsx(data, tab) : [readCsv(data)];
}

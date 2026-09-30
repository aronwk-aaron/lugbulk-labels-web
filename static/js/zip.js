// Writes a .zip archive in memory — the browser port of src/zip_writer.cpp,
// for Download when several files are ticked. Entries are stored
// uncompressed, as on the server: the PDFs inside are already compressed and the CSVs are tiny.
// File names are UTF-8, with the zip flag that says so.

let crcTable = null;

// CRC-32 (IEEE 802.3, as zlib's crc32()) of a Uint8Array.
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

// MS-DOS time and date words for a Date (local time), as zip stores them.
export function dosDateTime(date) {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
  const year = Math.min(Math.max(date.getFullYear(), 1980), 2107);
  const day = ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return [time & 0xffff, day & 0xffff];
}

const toBytes = (data) =>
  typeof data === 'string'
    ? new TextEncoder().encode(data)
    : data instanceof Uint8Array
      ? data
      : new Uint8Array(data);

// files: [[name, contents]] where contents is a string (written as UTF-8),
// a Uint8Array or an ArrayBuffer. Returns the .zip file's bytes.
export function zip(files, date = new Date()) {
  const [dosTime, dosDate] = dosDateTime(date);
  const UTF8_NAMES = 1 << 11; // general-purpose flag: names are UTF-8
  const entries = files.map(([name, data]) => {
    const nameBytes = new TextEncoder().encode(name);
    if (nameBytes.length > 0xffff) throw new Error('zip: file name too long');
    const bytes = toBytes(data);
    return { nameBytes, bytes, crc: crc32(bytes) };
  });
  if (entries.length > 0xffff) throw new Error('zip: too many files');

  let size = 22; // end of central directory
  for (const e of entries) size += 30 + 46 + 2 * e.nameBytes.length + e.bytes.length;
  if (size > 0xffffffff) throw new Error('zip: bundle too large'); // no zip64 here

  const out = new Uint8Array(size);
  const view = new DataView(out.buffer);
  let pos = 0;
  const put16 = (v) => { view.setUint16(pos, v, true); pos += 2; };
  const put32 = (v) => { view.setUint32(pos, v >>> 0, true); pos += 4; };
  const putBytes = (b) => { out.set(b, pos); pos += b.length; };

  const offsets = [];
  for (const e of entries) {
    offsets.push(pos);
    put32(0x04034b50); // local file header
    put16(20); // version needed
    put16(UTF8_NAMES);
    put16(0); // method: stored
    put16(dosTime);
    put16(dosDate);
    put32(e.crc);
    put32(e.bytes.length);
    put32(e.bytes.length);
    put16(e.nameBytes.length);
    put16(0); // extra length
    putBytes(e.nameBytes);
    putBytes(e.bytes);
  }
  const centralOffset = pos;
  entries.forEach((e, i) => {
    put32(0x02014b50); // central directory header
    put16(20); // version made by
    put16(20); // version needed
    put16(UTF8_NAMES);
    put16(0);
    put16(dosTime);
    put16(dosDate);
    put32(e.crc);
    put32(e.bytes.length);
    put32(e.bytes.length);
    put16(e.nameBytes.length);
    put16(0); // extra
    put16(0); // comment
    put16(0); // disk
    put16(0); // internal attrs
    put32(0); // external attrs
    put32(offsets[i]);
    putBytes(e.nameBytes);
  });
  const centralSize = pos - centralOffset;
  put32(0x06054b50); // end of central directory
  put16(0);
  put16(0);
  put16(entries.length);
  put16(entries.length);
  put32(centralSize);
  put32(centralOffset);
  put16(0); // comment length
  return out;
}

// String helpers that behave like the C++ server's (src/*.cpp), which works
// on UTF-8 bytes in the "C" locale: only ASCII letters change case, only
// ASCII whitespace counts as space, and strings sort by byte (= code point)
// order. JavaScript's own trim()/toLowerCase()/< differ on non-ASCII text,
// so the ports use these instead to give identical results.

// isspace() in the "C" locale.
export const C_SPACE = ' \t\n\v\f\r';

// The servers' trim(): strips " \t\r\n" (not \v or \f) from both ends.
export function trim(s) {
  let start = 0;
  let end = s.length;
  while (start < end && ' \t\r\n'.includes(s[start])) start++;
  while (end > start && ' \t\r\n'.includes(s[end - 1])) end--;
  return s.slice(start, end);
}

export function asciiLower(s) {
  return s.replace(/[A-Z]/g, (c) => c.toLowerCase());
}

export function asciiUpper(s) {
  return s.replace(/[a-z]/g, (c) => c.toUpperCase());
}

// std::string's operator<, i.e. UTF-8 byte order, which is code point
// order. (UTF-16 code unit order, what < on JS strings uses, differs for
// characters above U+FFFF.) Returns <0, 0 or >0.
export function compareBytes(a, b) {
  const ia = a[Symbol.iterator]();
  const ib = b[Symbol.iterator]();
  for (;;) {
    const x = ia.next();
    const y = ib.next();
    if (x.done || y.done) return (x.done ? 0 : 1) - (y.done ? 0 : 1);
    const cx = x.value.codePointAt(0);
    const cy = y.value.codePointAt(0);
    if (cx !== cy) return cx - cy;
  }
}

// Splits on C whitespace, like reading words with `istringstream >> tok`.
export function splitWs(s) {
  return s.split(/[ \t\n\v\f\r]+/).filter((t) => t !== '');
}

// A cell as a string (rows from JSON may hold numbers or null).
export function str(v) {
  return v === undefined || v === null ? '' : String(v);
}

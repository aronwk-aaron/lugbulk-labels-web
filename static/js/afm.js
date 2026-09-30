// Text widths for the standard PDF fonts, so labels and reports wrap and
// fit the same way everywhere.
//
// Each width is the Adobe AFM advance (Helvetica.afm, Helvetica-Bold.afm,
// 1/1000 em) of the glyph that WinAnsiEncoding gives that byte, for bytes
// 0x20-0xFF — the glyph pdf-lib draws, so what is measured is what is
// shown. No kerning: viewers don't kern the text we draw (pdf-lib's
// widthOfTextAtSize does, so it measures a little short). Bytes WinAnsi
// leaves undefined (0x7F, 0x81, 0x8D, 0x8F, 0x90, 0x9D; winAnsi() turns
// them into spaces before drawing) count as a space (278).
//
// (The server's old PoDoFo 0.9.8 measurer looked bytes up by
// StandardEncoding code instead, giving ' ` and most bytes >= 0x80 the
// wrong width; that is not reproduced.)

// AFM_WIDTHS[font][byte - 0x20]
export const AFM_WIDTHS = {
  regular: [
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584, 278,
    556, 278, 222, 556, 333, 1000, 556, 556, 333, 1000, 667, 333, 1000, 278, 611, 278,
    278, 222, 222, 333, 333, 350, 556, 1000, 333, 1000, 500, 333, 944, 278, 500, 500,
    278, 333, 556, 556, 556, 556, 260, 556, 333, 737, 370, 556, 584, 333, 737, 333,
    400, 584, 333, 333, 333, 556, 537, 278, 333, 333, 365, 556, 834, 834, 834, 611,
    667, 667, 667, 667, 667, 667, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 500, 556, 556, 556, 556, 278, 278, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 584, 611, 556, 556, 556, 556, 500, 556, 500,
  ],
  bold: [
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584, 278,
    556, 278, 278, 556, 500, 1000, 556, 556, 333, 1000, 667, 333, 1000, 278, 611, 278,
    278, 278, 278, 500, 500, 350, 556, 1000, 333, 1000, 556, 333, 944, 278, 500, 556,
    278, 333, 556, 556, 556, 556, 280, 556, 333, 737, 370, 556, 584, 333, 737, 333,
    400, 584, 333, 333, 333, 611, 556, 278, 333, 333, 365, 556, 834, 834, 834, 611,
    722, 722, 722, 722, 722, 722, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 556, 556, 556, 556, 556, 278, 278, 278, 278,
    611, 611, 611, 611, 611, 611, 611, 584, 611, 611, 611, 611, 611, 556, 611, 556,
  ],
};

// WinAnsi bytes 0x80-0x9F as Unicode (unassigned bytes as their C1 code).
const WINANSI_HIGH = '\u20ac\u0081\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\u008d\u017d\u008f' +
  '\u0090\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\u009d\u017e\u0178';

// The WinAnsi byte for a character (anything else is drawn as '?').
export function winAnsiByte(ch) {
  const c = ch.codePointAt(0);
  if (c < 0x80 || (c >= 0xa0 && c <= 0xff)) return c;
  const i = WINANSI_HIGH.indexOf(ch);
  return i >= 0 ? 0x80 + i : 0x3f;
}

// The width of `text` (WinAnsi-safe, see reports.winAnsi) in points, drawn
// in Helvetica ('regular') or Helvetica-Bold ('bold') at `size`.
export function helveticaMeasure(font, text, size) {
  const widths = AFM_WIDTHS[font];
  let w = 0;
  for (const ch of text) {
    const b = winAnsiByte(ch);
    w += b >= 0x20 ? widths[b - 0x20] : 278;
  }
  return (w * size) / 1000;
}

// Text widths (static/js/afm.js): Adobe's AFM widths for Helvetica and
// Helvetica-Bold by WinAnsi code, no kerning. Labels, the alignment test
// page and the reports all measure with them.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { PDFDocument, PDFRawStream, StandardFonts, decodePDFRawStream } from '../../static/js/vendor/pdf-lib.js';
import { AFM_WIDTHS, helveticaMeasure, winAnsiByte } from '../../static/js/afm.js';
import * as labels from '../../static/js/labels.js';
import { winAnsi } from '../../static/js/reports.js';

// Per-character AFM widths (1/1000 em, [Helvetica, Helvetica-Bold]), written
// out by hand from Helvetica.afm and Helvetica-Bold.afm.
const AFM = {
  ' ': [278, 278], "'": [191, 238], C: [722, 722], F: [611, 611], Z: [611, 611], a: [556, 556],
  b: [556, 611], c: [500, 556], e: [556, 556], i: [222, 278], l: [222, 278], m: [833, 889],
  o: [556, 611], r: [333, 389], s: [500, 556], t: [278, 333],
  è: [556, 556], é: [556, 556], ë: [556, 556], í: [278, 278], û: [556, 611],
};
const afmWidth = (text, font) => [...text].reduce((sum, ch) => sum + AFM[ch][font === 'bold' ? 1 : 0], 0);

test('measures "Zoë Fictícia\'s" and "Crème brûlée" with the AFM widths', () => {
  for (const text of ["Zoë Fictícia's", 'Crème brûlée']) {
    for (const font of ['regular', 'bold']) {
      for (const size of [1000, 9, 12.5]) {
        const want = (afmWidth(text, font) * size) / 1000;
        assert.ok(Math.abs(helveticaMeasure(font, text, size) - want) < 1e-9, `${text} ${font} ${size}`);
      }
    }
  }
  // The totals, so a wrong table can't be matched by a wrong helper.
  assert.equal(helveticaMeasure('regular', "Zoë Fictícia's", 1000), 5859);
  assert.equal(helveticaMeasure('bold', "Zoë Fictícia's", 1000), 6296);
  assert.equal(helveticaMeasure('regular', 'Crème brûlée', 1000), 6057);
  assert.equal(helveticaMeasure('bold', 'Crème brûlée', 1000), 6391);
});

test("the characters PoDoFo's StandardEncoding lookup got wrong are right", () => {
  // quotesingle, grave, eacute, edieresis, ntilde ... looked up by the wrong code before.
  assert.equal(helveticaMeasure('regular', "'", 1000), 191);
  assert.equal(helveticaMeasure('bold', "'", 1000), 238);
  assert.equal(helveticaMeasure('regular', '`', 1000), 333);
  for (const ch of 'éëñçüöäÉÑ') {
    assert.ok(helveticaMeasure('regular', ch, 1000) >= 500, ch);
  }
  assert.equal(helveticaMeasure('regular', 'ñ', 1000), 556);
  assert.equal(helveticaMeasure('bold', 'ñ', 1000), 611);
  assert.equal(helveticaMeasure('regular', '€', 1000), 556);
  assert.equal(helveticaMeasure('regular', ' ', 1000), 278);
  assert.equal(helveticaMeasure('regular', '', 12), 0);
});

test('every byte 0x20-0xFF has the width pdf-lib draws it with', async () => {
  const doc = await PDFDocument.create();
  const fonts = {
    regular: await doc.embedFont(StandardFonts.Helvetica),
    bold: await doc.embedFont(StandardFonts.HelveticaBold),
  };
  for (const font of ['regular', 'bold']) {
    assert.equal(AFM_WIDTHS[font].length, 0xe0);
    for (let b = 0x20; b <= 0xff; ++b) {
      // The character WinAnsi draws for that byte (0x7F and the undefined
      // bytes aren't drawn: winAnsi() turns them into spaces).
      if ([0x7f, 0x81, 0x8d, 0x8f, 0x90, 0x9d].includes(b)) continue;
      const ch = new TextDecoder('windows-1252').decode(new Uint8Array([b]));
      assert.equal(winAnsiByte(ch), b);
      // One character has nothing to kern with.
      const want = fonts[font].widthOfTextAtSize(ch, 1000);
      assert.ok(Math.abs(helveticaMeasure(font, ch, 1000) - want) < 1e-6, `${font} 0x${b.toString(16)} ${ch}: ${helveticaMeasure(font, ch, 1000)} vs ${want}`);
    }
  }
});

test('bytes WinAnsi leaves undefined are drawn (and measured) as spaces', () => {
  for (const ch of ['\u007f', '\u0081', '\u008d', '\u008f', '\u0090', '\u009d']) {
    assert.equal(winAnsi(ch), ' ');
  }
  assert.equal(helveticaMeasure('regular', winAnsi('\u0081'), 1000), 278);
});

test('text outside WinAnsi is measured as the "?" it is drawn as', () => {
  const drawn = winAnsi('Łódź 😀');
  assert.equal(helveticaMeasure('regular', drawn, 10), helveticaMeasure('regular', '?ód? ?', 10));
});

test('no kerning: a string is the sum of its characters', () => {
  for (const text of ['Avery 5160', 'AV To', 'Ty. Wa,']) {
    const sum = [...text].reduce((w, ch) => w + helveticaMeasure('regular', ch, 11), 0);
    assert.ok(Math.abs(helveticaMeasure('regular', text, 11) - sum) < 1e-9, text);
  }
});

// ---- the alignment test page --------------------------------------------------

async function firstCaption(bytes) {
  const doc = await PDFDocument.load(bytes);
  for (const [, obj] of doc.context.enumerateIndirectObjects()) {
    if (!(obj instanceof PDFRawStream)) continue;
    let raw;
    try {
      raw = Buffer.from(decodePDFRawStream(obj).decode()).toString('latin1');
    } catch {
      continue;
    }
    const m = raw.match(/1 0 0 1 ([\d.]+) ([\d.]+) Tm\s+\/\S+ ([\d.]+) Tf\s+<([0-9A-Fa-f]*)> Tj/) ||
      raw.match(/1 0 0 1 ([\d.]+) ([\d.]+) Tm\s+<([0-9A-Fa-f]*)> Tj/);
    if (m) return { x: Number(m[1]), text: Buffer.from(m[m.length - 1], 'hex').toString('latin1') };
  }
  throw new Error('no caption found');
}

test('alignment test page: the caption is centred by helveticaMeasure (no kerning)', async () => {
  const specs = JSON.parse(readFileSync(new URL('../../data/label_specs.json', import.meta.url), 'utf8')).specs;
  const spec = specs.find((s) => s.id === 'avery5160');
  const { x, text } = await firstCaption(await labels.buildTestPage(spec));
  assert.ok(text.startsWith('Avery 5160 #1 '), text);
  const w = spec.label_width_mm * labels.MM_TO_PT;
  const centre = spec.left_margin_mm * labels.MM_TO_PT + w / 2;
  const textSize = Math.min(spec.label_height_mm * labels.MM_TO_PT * 0.12, 10);
  const tw = helveticaMeasure('regular', text, textSize);
  assert.ok(Math.abs(x + tw / 2 - centre) < 0.01, `caption starts at ${x}, centre ${x + tw / 2} vs ${centre}`);
});

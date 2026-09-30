// LEGO <-> BrickLink color name lookup — the browser port of
// src/colors.{h,cpp} (same table, same order; keep them in sync).
//
// Order sheets name colors two ways, and a given sheet may only fill in
// one: LEGO's own (usually abbreviated, "MED. ST-GREY") names, or
// BrickLink's ("Light Bluish Gray"). Lookups ignore punctuation, spacing
// and case.

import { trim } from './text.js';

// [LEGO name, BrickLink name, LEGO spellings seen on LUGBulk lists]
const COLORS = [
  ['White', 'White', ['WHITE']],
  ['Black', 'Black', ['BLACK']],
  ['Bright Red', 'Red', ['BR.RED', 'BR. RED']],
  ['Bright Blue', 'Blue', ['BR.BLUE', 'BR. BLUE']],
  ['Bright Yellow', 'Yellow', ['BR.YEL', 'BR. YEL', 'BR.YELLOW']],
  ['Bright Green', 'Bright Green', ['BR.GREEN', 'BR. GREEN']],
  ['Dark Green', 'Green', ['DK.GREEN', 'DK. GREEN']],
  ['Earth Green', 'Dark Green', ['EARTH GREEN']],
  ['Earth Blue', 'Dark Blue', ['EARTH BLUE']],
  ['Medium Stone Grey', 'Light Bluish Gray', ['MED. ST-GREY', 'MED.ST-GREY', 'MED. ST. GREY', 'M. ST. GREY', 'LT. ST. GREY', 'LT.ST.GREY']],
  ['Dark Stone Grey', 'Dark Bluish Gray', ['DK. ST. GREY', 'DK.ST.GREY', 'DK. ST-GREY']],
  ['Brick Yellow', 'Tan', ['BRICK-YEL', 'BRICK YEL', 'BRICK-YELLOW']],
  ['Sand Yellow', 'Dark Tan', ['SAND YELLOW']],
  ['Reddish Brown', 'Reddish Brown', ['RED. BROWN', 'RED.BROWN']],
  ['Dark Brown', 'Dark Brown', ['DK. BROWN', 'DK.BROWN']],
  ['New Dark Red', 'Dark Red', ['NEW DARK RED']],
  ['Sand Green', 'Sand Green', ['SAND GREEN']],
  ['Sand Blue', 'Sand Blue', ['SAND BLUE']],
  ['Olive Green', 'Olive Green', ['OLIVE GREEN']],
  ['Nougat', 'Nougat', ['NOUGAT']],
  ['Medium Nougat', 'Medium Nougat', ['M. NOUGAT', 'MED. NOUGAT', 'M.NOUGAT']],
  ['Light Nougat', 'Light Nougat', ['L.NOUGAT', 'L. NOUGAT', 'LGH. NOUGAT']],
  ['Dark Orange', 'Dark Orange', ['DK.ORA', 'DK. ORA', 'DK.ORANGE']],
  ['Bright Orange', 'Orange', ['BR.ORANGE', 'BR. ORANGE', 'BR.ORA']],
  ['Reddish Orange', 'Reddish Orange', ['RED. ORANGE', 'RED.ORANGE']],
  ['Flame Yellowish Orange', 'Bright Light Orange', ['FL. YELL-ORA', 'FL.YELL-ORA']],
  ['Bright Yellowish Green', 'Lime', ['BR.YEL-GREEN', 'BR. YEL-GREEN']],
  ['Bright Bluish Green', 'Dark Turquoise', ['BR.BLUEGREEN', 'BR. BLUEGREEN']],
  ['Aqua', 'Light Aqua', ['AQUA']],
  ['Lavender', 'Lavender', ['LAVENDER']],
  ['Medium Lavender', 'Medium Lavender', ['M. LAVENDER', 'MED. LAVENDER']],
  ['Medium Lilac', 'Dark Purple', ['MEDIUM LILAC', 'M. LILAC']],
  ['Bright Reddish Violet', 'Magenta', ['BR.RED-VIOLET', 'BR.RED.VIOLET']],
  ['Bright Purple', 'Dark Pink', ['BR.PURPLE', 'BR. PURPLE']],
  ['Light Purple', 'Bright Pink', ['LGH. PURPLE', 'LGH.PURPLE']],
  ['Medium Blue', 'Medium Blue', ['MEDIUM BLUE', 'M. BLUE']],
  ['Medium Azur', 'Medium Azure', ['MEDIUM AZUR', 'MED. AZUR']],
  ['Dark Azur', 'Dark Azure', ['DARK AZUR', 'DK. AZUR']],
  ['Light Royal Blue', 'Bright Light Blue', ['LT.ROY.BLUE', 'LGH. ROYAL BLUE']],
  ['Cool Yellow', 'Bright Light Yellow', ['COOL YELLOW']],
  ['Vibrant Coral', 'Coral', ['VIBRANT CORAL']],
  ['Warm Pink', 'Warm Pink', ['WARM PINK']],
  ['Spring Yellowish Green', 'Yellowish Green', ['SPR. YEL-GREEN']],
  ['Silver Metallic', 'Flat Silver', ['SILVER MET.', 'SILVER MET']],
  ['Titanium Metallic', 'Pearl Dark Gray', ['TITAN. MET.', 'TITANIUM MET.']],
  ['Warm Gold', 'Pearl Gold', ['WARM GOLD']],
  ['White Glow', 'Glow In Dark White', ['WHITE GLOW']],
  ['Transparent', 'Trans-Clear', ['TR.', 'TR', 'TRANSPARENT']],
  ['Transparent Light Blue', 'Trans-Light Blue', ['TR.L.BLUE', 'TR. L. BLUE']],
  ['Transparent Blue', 'Trans-Dark Blue', ['TR.BLUE', 'TR. BLUE']],
  ['Transparent Brown', 'Trans-Black', ['TR.BROWN', 'TR. BROWN']],
  ['Transparent Red', 'Trans-Red', ['TR.RED', 'TR. RED']],
  ['Transparent Green', 'Trans-Green', ['TR.GREEN', 'TR. GREEN']],
  ['Transparent Yellow', 'Trans-Yellow', ['TR.YEL', 'TR. YELLOW', 'TR.YELLOW']],
  ['Transparent Bright Orange', 'Trans-Orange', ['TR.BR.ORANGE']],
  ['Transparent Fluorescent Reddish Orange', 'Trans-Neon Orange', ['TR.FL.RED-ORA']],
  ['Transparent Fluorescent Green', 'Trans-Neon Green', ['TR.FL.GREEN']],
];

// Approximate sRGB per LEGO color, for the label color swatch.
const RGB = new Map([
  ['White', 0xffffff],
  ['Black', 0x1b1b1b],
  ['Bright Red', 0xc91a09],
  ['Bright Blue', 0x0055bf],
  ['Bright Yellow', 0xf2cd37],
  ['Bright Green', 0x4b9f4a],
  ['Dark Green', 0x237841],
  ['Earth Green', 0x184632],
  ['Earth Blue', 0x0a3463],
  ['Medium Stone Grey', 0xa0a5a9],
  ['Dark Stone Grey', 0x6c6e68],
  ['Brick Yellow', 0xe4cd9e],
  ['Sand Yellow', 0x958a73],
  ['Reddish Brown', 0x582a12],
  ['Dark Brown', 0x352100],
  ['New Dark Red', 0x720e0f],
  ['Sand Green', 0xa0bcac],
  ['Sand Blue', 0x6074a1],
  ['Olive Green', 0x9b9a5a],
  ['Nougat', 0xd09168],
  ['Medium Nougat', 0xaa7d55],
  ['Light Nougat', 0xf6d7b3],
  ['Dark Orange', 0xa95500],
  ['Bright Orange', 0xfe8a18],
  ['Reddish Orange', 0xca4c0b],
  ['Flame Yellowish Orange', 0xf8bb3d],
  ['Bright Yellowish Green', 0xbbe90b],
  ['Bright Bluish Green', 0x008f9b],
  ['Aqua', 0xadc3c0],
  ['Lavender', 0xe1d5ed],
  ['Medium Lavender', 0xac78ba],
  ['Medium Lilac', 0x3f3691],
  ['Bright Reddish Violet', 0x923978],
  ['Bright Purple', 0xc870a0],
  ['Light Purple', 0xe4adc8],
  ['Medium Blue', 0x5a93db],
  ['Medium Azur', 0x36aebf],
  ['Dark Azur', 0x078bc9],
  ['Light Royal Blue', 0x9fc3e9],
  ['Cool Yellow', 0xfff03a],
  ['Vibrant Coral', 0xff698f],
  ['Warm Pink', 0xf7a1b8],
  ['Spring Yellowish Green', 0xdfeea5],
  ['Silver Metallic', 0x898788],
  ['Titanium Metallic', 0x575857],
  ['Warm Gold', 0xaa7f2e],
  ['White Glow', 0xd9e4a7],
  ['Transparent', 0xeeeeee],
  ['Transparent Light Blue', 0xaeefec],
  ['Transparent Blue', 0x0020a0],
  ['Transparent Brown', 0x635f52],
  ['Transparent Red', 0xc91a09],
  ['Transparent Green', 0x237841],
  ['Transparent Yellow', 0xf5cd2f],
  ['Transparent Bright Orange', 0xf08f1c],
  ['Transparent Fluorescent Reddish Orange', 0xff800d],
  ['Transparent Fluorescent Green', 0xf8f184],
]);

// Punctuation, spacing and case don't matter (only ASCII letters and
// digits count, as in the C++).
function key(name) {
  return name.replace(/[^A-Za-z0-9]/g, '').toUpperCase();
}

// key -> [LEGO name, BrickLink name]; the first entry for a key wins.
function table(keysOf) {
  const t = new Map();
  for (const [lego, bl, aliases] of COLORS) {
    for (const k of keysOf(lego, bl, aliases)) {
      if (!t.has(k)) t.set(k, [lego, bl]);
    }
  }
  return t;
}
const BY_LEGO = table((lego, bl, aliases) => [key(lego), ...aliases.map(key)]);
const BY_BL = table((lego, bl) => [key(bl)]);

// {lego, bl, mapped}: a recognized LEGO abbreviation is expanded to LEGO's
// full name; a BrickLink name the sheet provides is kept as-is. `mapped`
// is false if one side was missing and couldn't be looked up.
export function resolve(legoIn, blIn) {
  const lego = trim(legoIn);
  const bl = trim(blIn);
  if (lego) {
    const hit = BY_LEGO.get(key(lego));
    if (hit) return { lego: hit[0], bl: bl || hit[1], mapped: true };
    return { lego, bl, mapped: bl !== '' };
  }
  if (bl) {
    // A LEGO name typed into the BL column still identifies the color.
    for (const t of [BY_BL, BY_LEGO]) {
      const hit = t.get(key(bl));
      if (hit) return { lego: hit[0], bl, mapped: true };
    }
    return { lego: '', bl, mapped: false };
  }
  return { lego: '', bl: '', mapped: true }; // missing_color is reported separately
}

// Trans colors photograph as a faint outline on LEGO's white product shots.
export function isTransparent(lego, bl) {
  return key(lego).startsWith('TR') || key(bl).startsWith('TR');
}

// White-family colors are hard to see on LEGO's white product shots.
export function isLight(lego, bl) {
  return key(lego).includes('WHITE') || key(bl).includes('WHITE');
}

// [r, g, b] in 0..1 for a label's color swatch, or null if unknown.
export function swatchRgb(lego, bl) {
  for (const [t, name] of [[BY_LEGO, lego], [BY_BL, bl], [BY_LEGO, bl]]) {
    const hit = t.get(key(name));
    if (!hit) continue;
    const rgb = RGB.get(hit[0]);
    if (rgb !== undefined) {
      return [((rgb >> 16) & 0xff) / 255, ((rgb >> 8) & 0xff) / 255, (rgb & 0xff) / 255];
    }
  }
  return null;
}

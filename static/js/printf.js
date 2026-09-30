// printf-style number formatting that matches glibc exactly, for the
// report ports: the double's exact decimal value, rounded half-to-even on
// exact ties. (toFixed/toPrecision round exact ties away from zero, e.g.
// 12.5 -> "13" where printf("%.0f") gives "12".)

// x (finite, > 0) = digits / 10^scale, exactly.
function exactDecimal(x) {
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, x);
  const bits = view.getBigUint64(0);
  const biased = Number((bits >> 52n) & 0x7ffn);
  let mant = bits & ((1n << 52n) - 1n);
  let exp;
  if (biased === 0) {
    exp = -1074;
  } else {
    mant |= 1n << 52n;
    exp = biased - 1075;
  }
  if (exp >= 0) return { digits: (mant << BigInt(exp)).toString(), scale: 0 };
  return { digits: (mant * 5n ** BigInt(-exp)).toString(), scale: -exp };
}

// The digit string `d` rounded to its first `keep` digits (keep >= 0),
// half-to-even. Returns the kept digits as a BigInt.
function roundDigits(d, keep) {
  if (keep >= d.length) return BigInt(d) * 10n ** BigInt(keep - d.length);
  let head = keep > 0 ? BigInt(d.slice(0, keep)) : 0n;
  const first = d[keep];
  const tail = /[1-9]/.test(d.slice(keep + 1));
  if (first > '5' || (first === '5' && (tail || (head & 1n) === 1n))) head += 1n;
  return head;
}

function special(x) {
  if (Number.isNaN(x)) return 'nan';
  if (!Number.isFinite(x)) return x < 0 ? '-inf' : 'inf';
  return null;
}

// printf("%.<decimals>f", x).
export function formatF(x, decimals) {
  const s = special(x);
  if (s !== null) return s;
  const sign = x < 0 || Object.is(x, -0) ? '-' : '';
  if (x === 0) return sign + (decimals > 0 ? '0.' + '0'.repeat(decimals) : '0');
  const { digits, scale } = exactDecimal(Math.abs(x));
  // digits / 10^scale rounded to `decimals` places = round(digits / 10^(scale - decimals)).
  let n;
  if (scale <= decimals) n = BigInt(digits) * 10n ** BigInt(decimals - scale);
  else n = roundDigits(digits, digits.length - (scale - decimals));
  let str = n.toString().padStart(decimals + 1, '0');
  if (decimals > 0) str = `${str.slice(0, -decimals)}.${str.slice(-decimals)}`;
  return sign + str;
}

// printf("%.<precision>g", x).
export function formatG(x, precision = 6) {
  const s = special(x);
  if (s !== null) return s;
  const p = precision === 0 ? 1 : precision;
  const sign = x < 0 || Object.is(x, -0) ? '-' : '';
  if (x === 0) return `${sign}0`;
  const { digits, scale } = exactDecimal(Math.abs(x));
  let exp = digits.length - 1 - scale; // decimal exponent of the first digit
  let kept = roundDigits(digits, p).toString();
  if (kept.length > p) {
    kept = kept.slice(0, p);
    exp += 1;
  }
  const strip = (m) => (m.includes('.') ? m.replace(/0+$/, '').replace(/\.$/, '') : m);
  let out;
  if (exp < -4 || exp >= p) {
    const mant = strip(kept.length > 1 ? `${kept[0]}.${kept.slice(1)}` : kept);
    out = `${mant}e${exp < 0 ? '-' : '+'}${String(Math.abs(exp)).padStart(2, '0')}`;
  } else if (exp >= 0) {
    out = strip(`${kept.slice(0, exp + 1)}.${kept.slice(exp + 1)}`);
  } else {
    out = strip(`0.${'0'.repeat(-exp - 1)}${kept}`);
  }
  return sign + out;
}

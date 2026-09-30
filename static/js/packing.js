// "Keep each part on one sheet": which label goes in which slot of a
// multi-label sheet so that no part is split across two sheets, using as
// few sheets as possible.
//
// The labels of a part are `records` that share an element id, already in
// the design's part order (ordering.js orderRecords). A part with n labels
// on sheets of `per` slots:
//  - n <= per: one item of size n, which must not be split;
//  - n > per: floor(n / per) whole sheets of its own, starting at the top of
//    a fresh sheet, and a remainder of n mod per labels (if not 0) that
//    enters the packing as an item like any other. A part is never split
//    more than that (it always uses ceil(n / per) sheets, the minimum).
//
// Packing the items into the fewest sheets is bin packing (capacity per,
// item size = labels). It is solved exactly:
//  1. lower bounds (ceil(sum / per) and Martello-Toth's L2) and first/best-fit
//     decreasing for an upper bound; when they meet, that is the optimum;
//  2. otherwise "bin completion" over the multiset of item sizes (many parts
//     share a size): a sheet holding the largest remaining item is filled
//     with a maximal set of the remaining items (any other sheet can be
//     exchanged for one), memoizing the states already shown to fit or not,
//     cutting with the lower bounds above. The number of sheets is tried from
//     the lower bound up, so the first that fits is the optimum, proven.
//  3. among solutions with that many sheets, the one closest to the design's
//     order: sheets are filled one after the other, each opened by the
//     first part not yet placed (so sheets come out ordered by their first
//     part) and completed with the earliest parts that still leave the rest
//     packable in the sheets left (lexicographically earliest assignment);
//     within a sheet parts keep the design order.
//
// Everything is time-boxed (`timeBoxMs`, default 1000). If the box expires
// during step 2 the best packing found (the first/best-fit one) is returned
// with proven: false; if it expires during step 3 the sheets chosen so far
// are kept and the rest are filled from a packing already known to fit
// (still the same number of sheets, proven unchanged). With no time box
// expiring the result is a pure function of the input.
//
// packRecords() returns the slot sequence as indexes into `records`
// (-1 = an empty slot) so it can cross a Worker boundary cheaply;
// slotsOf() turns that into records and nulls.

export const KEEP_OFF = 'off';
export const KEEP_OPTIMIZE = 'optimize';
export const KEEP_MODES = [KEEP_OFF, KEEP_OPTIMIZE];

// A saved value back to a known one ('off' for anything else).
export const normalizeKeep = (v) => (v === KEEP_OPTIMIZE ? KEEP_OPTIMIZE : KEEP_OFF);

// Parts in order of first appearance: [{id, idx: [record index, ...]}].
function partsOf(records) {
  const byId = new Map();
  const parts = [];
  records.forEach((r, i) => {
    let p = byId.get(r.element_id);
    if (!p) {
      p = { id: r.element_id, idx: [] };
      byId.set(r.element_id, p);
      parts.push(p);
    }
    p.idx.push(i);
  });
  return parts;
}

const ceilDiv = (a, b) => Math.floor((a + b - 1) / b);

// ---- what the current flow does ("off") -------------------------------------

// Sheets, empty slots and split parts when the labels just run on
// continuously in the order given. A part is "split" when it spans more
// sheets than it needs (ceil(n / per)).
export function flowSummary(records, per) {
  const n = records.length;
  const sheets = per > 0 ? ceilDiv(n, per) : 0;
  let splitParts = 0;
  for (const p of partsOf(records)) {
    const first = Math.floor(p.idx[0] / per);
    const last = Math.floor(p.idx[p.idx.length - 1] / per);
    if (last - first + 1 > ceilDiv(p.idx.length, per)) ++splitParts;
  }
  return { sheets, blanks: sheets * per - n, splitParts };
}

// ---- bounds and heuristics ----------------------------------------------------

// Martello-Toth's L2 (which includes ceil(sum / C)) for `counts[s]` items of
// size s, 1..C.
function lowerBound(counts, C, total) {
  const cnt = new Array(C + 2).fill(0); // cnt[s] prefix sums over sizes <= s
  const sum = new Array(C + 2).fill(0);
  for (let s = 1; s <= C; ++s) {
    cnt[s] = cnt[s - 1] + counts[s];
    sum[s] = sum[s - 1] + counts[s] * s;
  }
  let best = ceilDiv(total, C);
  const half = Math.floor(C / 2);
  for (let alpha = 0; alpha <= half; ++alpha) {
    // N1: s > C - alpha; N2: C - alpha >= s > C/2; N3: C/2 >= s >= alpha.
    const n1 = cnt[C] - cnt[C - alpha];
    const n2 = cnt[C - alpha] - cnt[half];
    const sum2 = sum[C - alpha] - sum[half];
    const sum3 = sum[half] - sum[Math.max(alpha - 1, 0)];
    const lb = n1 + n2 + Math.max(0, ceilDiv(sum3 - (n2 * C - sum2), C));
    if (lb > best) best = lb;
  }
  return best;
}

// First-fit and best-fit decreasing; the smaller packing. Returns the bins
// as arrays of item indexes.
function decreasing(sizes, C) {
  const order = sizes.map((_, i) => i).sort((a, b) => sizes[b] - sizes[a] || a - b);
  const run = (best) => {
    const bins = [];
    const left = [];
    for (const i of order) {
      let at = -1;
      for (let b = 0; b < bins.length; ++b) {
        if (left[b] < sizes[i]) continue;
        if (!best) {
          at = b;
          break;
        }
        if (at < 0 || left[b] < left[at]) at = b;
      }
      if (at < 0) {
        bins.push([]);
        left.push(C);
        at = bins.length - 1;
      }
      bins[at].push(i);
      left[at] -= sizes[i];
    }
    return bins;
  };
  const ff = run(false);
  const bf = run(true);
  return bf.length < ff.length ? bf : ff;
}

// ---- exact search -------------------------------------------------------------

class Timeout extends Error {}

// Bin completion over size counts. `c[s]` = items of size s still to place.
class Solver {
  constructor(C, deadline, now) {
    this.C = C;
    this.deadline = deadline;
    this.now = now;
    this.nodes = 0;
    this.memo = new Map(); // key -> {inf: most bins known too few, feas: fewest known enough, pat}
  }

  tick() {
    if ((++this.nodes & 63) === 0 && this.now() > this.deadline) throw new Timeout();
  }

  // Every maximal way to fill one bin with items from `c` that includes an
  // item of size `must`: arrays of [size, count] pairs, fullest first.
  patterns(c, must) {
    const C = this.C;
    const out = [];
    const sizes = [];
    for (let s = C; s >= 1; --s) if (c[s] > 0 || s === must) sizes.push(s);
    const take = new Array(C + 1).fill(0);
    const go = (k, room) => {
      this.tick();
      if (k === sizes.length) {
        // Maximal: no remaining item still fits.
        for (const s of sizes) {
          if (s <= room && c[s] - take[s] > 0) return;
        }
        const pat = [];
        for (const s of sizes) if (take[s] > 0) pat.push([s, take[s]]);
        out.push({ pat, fill: C - room });
        return;
      }
      const s = sizes[k];
      const most = Math.min(c[s], Math.floor(room / s));
      const least = s === must ? 1 : 0;
      for (let t = most; t >= least; --t) {
        take[s] = t;
        go(k + 1, room - t * s);
      }
      take[s] = 0;
    };
    // `must` is counted in take[] like any other size, at least once.
    if (c[must] > 0) go(0, C);
    return out;
  }

  key(c) {
    return c.join(',');
  }

  // Can the items in `c` (sum `total`) be packed in `bins` bins?
  fits(c, total, bins) {
    if (total === 0) return true;
    if (bins <= 0) return false;
    if (total > bins * this.C) return false;
    const k = this.key(c);
    const e = this.memo.get(k);
    if (e) {
      if (e.feas !== undefined && bins >= e.feas) return true;
      if (e.inf !== undefined && bins <= e.inf) return false;
    }
    this.tick();
    if (lowerBound(c, this.C, total) > bins) {
      this.note(k, bins, false);
      return false;
    }
    let s = this.C;
    while (c[s] === 0) --s;
    for (const { pat, fill } of this.patterns(c, s).sort((a, b) => b.fill - a.fill)) {
      for (const [size, n] of pat) c[size] -= n;
      let ok;
      try {
        ok = this.fits(c, total - fill, bins - 1);
      } finally {
        for (const [size, n] of pat) c[size] += n;
      }
      if (ok) {
        this.note(k, bins, true, pat);
        return true;
      }
    }
    this.note(k, bins, false);
    return false;
  }

  note(k, bins, yes, pat) {
    let m = this.memo.get(k);
    if (!m) {
      m = {};
      this.memo.set(k, m);
    }
    if (yes) {
      if (m.feas === undefined || bins < m.feas) {
        m.feas = bins;
        m.pat = pat;
      }
    } else if (m.inf === undefined || bins > m.inf) {
      m.inf = bins;
    }
  }

  // The bins (as [size, count] pattern lists) of a packing already found
  // for `c` — every state on the way was memoized as fitting.
  known(c, bins) {
    const out = [];
    const cur = c.slice();
    let left = bins;
    for (;;) {
      let total = 0;
      for (let s = 1; s <= this.C; ++s) total += cur[s] * s;
      if (total === 0) return out;
      const e = this.memo.get(this.key(cur));
      if (!e || e.feas === undefined || e.feas > left) return null;
      out.push(e.pat);
      for (const [size, n] of e.pat) cur[size] -= n;
      left = e.feas - 1;
    }
  }
}

// ---- the packing ----------------------------------------------------------------

function countsOf(sizes, C) {
  const c = new Array(C + 1).fill(0);
  for (const s of sizes) c[s] += 1;
  return c;
}

// Item-index lists compared as sets of "earliest first": the list holding the
// smaller index where they first differ is better; a longer list beats its
// own prefix (a fuller sheet).
function cmpItems(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; ++i) if (a[i] !== b[i]) return a[i] - b[i];
  return b.length - a.length;
}

// Packs item sizes (1..C each) into the fewest bins of capacity C. Returns
// {bins: [[item index, ...], ...] (each bin's items in index order, bins in
// order of their first item), proven, lowerBound, orderExact}.
export function packSizes(sizes, C, { timeBoxMs = 1000, now = () => Date.now() } = {}) {
  const m = sizes.length;
  const deadline = now() + timeBoxMs;
  if (m === 0) return { bins: [], proven: true, lowerBound: 0, orderExact: true };
  const counts = countsOf(sizes, C);
  const total = sizes.reduce((a, b) => a + b, 0);
  const solver = new Solver(C, deadline, now);

  // 1. Bounds and a heuristic packing.
  let lb = lowerBound(counts, C, total);
  const heuristic = decreasing(sizes, C);
  let need = heuristic.length;
  let proven = lb >= need;
  let witness = null; // [size,count] pattern lists, for an exact packing

  // 2. Exact: how few bins, from the lower bound up.
  if (!proven) {
    try {
      for (let t = lb; t < need; ++t) {
        if (solver.fits(counts, total, t)) {
          need = t;
          witness = solver.known(counts, t);
          break;
        }
        lb = t + 1; // t bins proven not enough
      }
      proven = true;
    } catch (e) {
      if (!(e instanceof Timeout)) throw e;
    }
  }

  // The bins of the packing to fall back on, as item lists.
  const byPatterns = (patterns) => {
    const queues = new Map();
    sizes.forEach((s, i) => {
      if (!queues.has(s)) queues.set(s, []);
      queues.get(s).push(i);
    });
    const at = new Map();
    return patterns.map((pat) => {
      const bin = [];
      for (const [s, n] of pat) {
        const q = queues.get(s);
        const from = at.get(s) || 0;
        for (let k = 0; k < n; ++k) bin.push(q[from + k]);
        at.set(s, from + n);
      }
      return bin.sort((a, b) => a - b);
    });
  };
  let fallback = witness ? byPatterns(witness) : heuristic.map((b) => b.slice().sort((a, b2) => a - b2));
  if (fallback.length !== need) fallback = heuristic.map((b) => b.slice().sort((a, b2) => a - b2));
  const sortBins = (bins) => bins.sort((a, b) => a[0] - b[0]);

  // 3. The exact packing closest to the design order.
  let bins = null;
  let orderExact = false;
  if (proven) {
    const c = counts.slice();
    const placed = new Array(m).fill(false);
    const queues = new Map(); // size -> item indexes still to place, ascending
    sizes.forEach((s, i) => {
      if (!queues.has(s)) queues.set(s, []);
      queues.get(s).push(i);
    });
    const chosen = [];
    let left = need;
    let first = 0;
    let left_total = total;
    try {
      while (left > 0 && left_total > 0) {
        while (placed[first]) ++first;
        const must = sizes[first];
        const cands = [];
        for (const { pat, fill } of solver.patterns(c, must)) {
          // The earliest unplaced items of each size (so the first unplaced
          // item, the earliest of its size, opens the bin).
          const items = [];
          for (const [s, n] of pat) {
            const q = queues.get(s);
            for (let k = 0; k < n; ++k) items.push(q[k]);
          }
          items.sort((a, b) => a - b);
          cands.push({ pat, fill, items });
        }
        cands.sort((a, b) => cmpItems(a.items, b.items));
        let pick = null;
        for (const cand of cands) {
          for (const [s, n] of cand.pat) c[s] -= n;
          let ok;
          try {
            ok = solver.fits(c, left_total - cand.fill, left - 1);
          } finally {
            for (const [s, n] of cand.pat) c[s] += n;
          }
          if (ok) {
            pick = cand;
            break;
          }
        }
        if (!pick) throw new Error('packing: no sheet fits although the rest fits');
        for (const i of pick.items) {
          placed[i] = true;
          queues.get(sizes[i]).splice(queues.get(sizes[i]).indexOf(i), 1);
        }
        for (const [s, n] of pick.pat) c[s] -= n;
        chosen.push(pick.items);
        left_total -= pick.fill;
        --left;
      }
      bins = chosen;
      orderExact = true;
    } catch (e) {
      if (!(e instanceof Timeout)) throw e;
      // Time's up: keep the sheets chosen, fill the rest from a packing known to fit.
      const rest = solver.known(c, left);
      if (rest) {
        const tail = [];
        for (const pat of rest) {
          const bin = [];
          for (const [s, n] of pat) {
            const q = queues.get(s);
            for (let k = 0; k < n; ++k) bin.push(q.shift());
          }
          tail.push(bin.sort((a, b) => a - b));
        }
        bins = chosen.concat(tail);
      }
    }
  }
  if (!bins || bins.length !== need) bins = fallback;
  return { bins: sortBins(bins.map((b) => b.slice().sort((x, y) => x - y))), proven, lowerBound: lb, orderExact };
}

// The slot sequence for `records` (grouped by part, in the design's part
// order) on sheets of `per` slots with no part split that needn't be.
// Returns {layout: [record index | -1, ...] (a multiple of per long),
// sheets, blanks, proven, lowerBound, orderExact}.
export function packRecords(records, per, opts = {}) {
  if (!(per > 1) || records.length === 0) {
    const layout = records.map((_, i) => i);
    const sheets = per > 0 ? ceilDiv(layout.length, per) : 0;
    while (per > 0 && layout.length < sheets * per) layout.push(-1);
    return { layout, sheets, blanks: layout.length - records.length, proven: true, lowerBound: sheets, orderExact: true };
  }
  const parts = partsOf(records);
  // Whole sheets of big parts, and the items that get packed.
  const blocks = []; // {pos, sub, slots: [record index...]} in sheet order candidates
  const items = []; // {part, idx: [record indexes]}
  parts.forEach((p, pi) => {
    const n = p.idx.length;
    let rest = p.idx;
    if (n > per) {
      const whole = Math.floor(n / per);
      for (let s = 0; s < whole; ++s) blocks.push({ pos: pi, sub: 0, slots: p.idx.slice(s * per, (s + 1) * per) });
      rest = p.idx.slice(whole * per);
    }
    if (rest.length) items.push({ part: pi, idx: rest });
  });
  const packed = packSizes(items.map((it) => it.idx.length), per, opts);
  for (const bin of packed.bins) {
    const slots = [];
    for (const i of bin) slots.push(...items[i].idx);
    blocks.push({ pos: items[bin[0]].part, sub: 1, slots });
  }
  blocks.sort((a, b) => a.pos - b.pos || a.sub - b.sub);
  const layout = [];
  for (const b of blocks) {
    layout.push(...b.slots);
    for (let k = b.slots.length; k < per; ++k) layout.push(-1);
  }
  return {
    layout,
    sheets: blocks.length,
    blanks: layout.length - records.length,
    proven: packed.proven,
    lowerBound: packed.lowerBound + blocks.filter((b) => b.sub === 0).length,
    orderExact: packed.orderExact,
  };
}

// The records (null = an empty slot) a layout from packRecords() stands for.
export const slotsOf = (records, layout) => layout.map((i) => (i < 0 ? null : records[i]));

// Element ids in the order their labels first appear in `slots`: the part
// order "same as the labels" means when parts are packed.
export function partOrderOf(slots) {
  const seen = new Set();
  const out = [];
  for (const r of slots) {
    if (r && !seen.has(r.element_id)) {
      seen.add(r.element_id);
      out.push(r.element_id);
    }
  }
  return out;
}

// Parts whose labels are on more than one sheet although the part fits
// fewer (what "keep each part on one sheet" gets rid of): for a slot sequence.
export function splitParts(slots, per) {
  const span = new Map();
  slots.forEach((r, i) => {
    if (!r) return;
    const e = span.get(r.element_id) || { sheets: new Set(), n: 0 };
    e.sheets.add(Math.floor(i / per));
    e.n += 1;
    span.set(r.element_id, e);
  });
  let n = 0;
  for (const e of span.values()) if (e.sheets.size > ceilDiv(e.n, per)) ++n;
  return n;
}

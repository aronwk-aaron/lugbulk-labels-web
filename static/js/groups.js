// Families: people whose orders are packed as one (any number of people). Each group
// has a name and its members' names (matched like in Compare years: case
// and spacing ignored). With `combine` on, applyGroups() turns the members'
// records into one per part under the group's name, quantities added up,
// so the labels, checklist, parts list and lot counts all treat the group
// as one person. Pure: no DOM, no storage.
//
// Stored as {combine, list: [{name, members}]}: with a saved sheet's
// report options (report_options.js) and, for Compare years, in this
// browser. normalizeGroups() checks whatever is read back.

import { formatQty, parseQty } from './pivot.js';
import { asciiLower, trim } from './text.js';

export const MAX_GROUPS = 40;
export const MAX_MEMBERS = 10;
export const MAX_NAME = 60; // characters, for a group's or a member's name

// The key people are matched by: lower case, spacing collapsed.
export function personKey(name) {
  return asciiLower(trim(String(name)).replace(/\s+/g, ' '));
}

// A name as typed: control characters to spaces, spacing collapsed,
// trimmed, at most MAX_NAME characters.
function cleanName(v) {
  if (typeof v !== 'string') return '';
  const s = v.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, ' ').replace(/\s+/g, ' ').trim();
  return [...s].slice(0, MAX_NAME).join('').trim();
}

// "Lee family" when everyone has the same last name, else the names joined:
// "Ann Lee & Cy Doe", "Ann Lee, Bob Roe & Cy Doe".
export function defaultName(members) {
  const words = members.map((m) => m.split(' '));
  const last = words.map((w) => w.at(-1));
  const shared = words.every((w) => w.length > 1) && last.every((l) => personKey(l) === personKey(last[0]));
  if (shared) return cleanName(`${last[0]} family`);
  if (members.length === 2) return cleanName(`${members[0]} & ${members[1]}`);
  return cleanName(`${members.slice(0, -1).join(', ')} & ${members.at(-1)}`);
}

// A valid groups object from anything. Members are cleaned and kept once
// (a person in two groups stays in the first); groups left with fewer
// than two members are dropped; a blank name gets defaultName().
export function normalizeGroups(raw) {
  const src = raw !== null && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
  const out = { combine: typeof src.combine === 'boolean' ? src.combine : true, list: [] };
  const taken = new Set();
  for (const g of Array.isArray(src.list) ? src.list : []) {
    if (out.list.length >= MAX_GROUPS) break;
    if (g === null || typeof g !== 'object') continue;
    const members = [];
    for (const m of Array.isArray(g.members) ? g.members : []) {
      const name = cleanName(m);
      const key = personKey(name);
      if (!name || taken.has(key) || members.length >= MAX_MEMBERS) continue;
      taken.add(key);
      members.push(name);
    }
    if (members.length < 2) {
      for (const m of members) taken.delete(personKey(m));
      continue;
    }
    out.list.push({ name: cleanName(g.name) || defaultName(members), members });
  }
  return out;
}

// person key -> group name, for the groups that apply (none when `combine`
// is off). `keyOf` maps a member's name to the key it is matched by.
export function groupOf(groups, keyOf = personKey) {
  const map = new Map();
  if (!groups || !groups.combine) return map;
  for (const g of groups.list) for (const m of g.members) map.set(keyOf(m), g.name);
  return map;
}

// The records with each group's members combined: one record per group and
// part, at the first member record's place, its qty the members' total.
// Everything else (description, colors, weights) is the part's, the same in
// every member's record.
export function applyGroups(records, groups) {
  const of = groupOf(groups);
  if (!of.size) return records;
  const out = [];
  const combined = new Map(); // JSON [group, element_id] -> index in out
  for (const r of records) {
    const group = of.get(personKey(r.person));
    if (group === undefined) {
      out.push(r);
      continue;
    }
    const k = JSON.stringify([group, r.element_id]);
    const at = combined.get(k);
    if (at === undefined) {
      combined.set(k, out.length);
      out.push({ ...r, person: group });
    } else {
      out[at] = { ...out[at], qty: formatQty(parseQty(out[at].qty) + parseQty(r.qty)) };
    }
  }
  return out;
}

// Groups with members in `people` (names), for showing which apply here:
// [{name, members, present: [names in people]}].
export function presentIn(groups, people) {
  const here = new Set(people.map(personKey));
  return groups.list.map((g) => ({ ...g, present: g.members.filter((m) => here.has(personKey(m))) }));
}

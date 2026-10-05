// The dashboard's "Compare years" section: add several years' order sheets
// (uploads, or saved Google Sheets read through GET /sheets/:id/values),
// then show the tables from years.js. Everything is worked out here in the
// browser; uploads are never sent anywhere.

import { SOURCE_TAB } from './layout.js';
import { SpreadsheetError, readTabs } from './spreadsheet.js';
import { formatCount } from './reports.js';
import * as years from './years.js';

const MAX_UPLOAD_BYTES = 10 * 1024 * 1024;
const MERGES_KEY = 'lugbulk.yearsMerges';

const $ = (id) => document.getElementById(id);

function el(tag, props, children) {
  const e = document.createElement(tag);
  if (props) for (const [k, v] of Object.entries(props)) {
    if (k === 'text') e.textContent = v;
    else e.setAttribute(k, v);
  }
  if (children) for (const c of children) e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  return e;
}

function setStatus(text, error) {
  const s = $('years-status');
  s.textContent = text;
  s.classList.toggle('error', !!error);
  s.classList.toggle('muted', !error);
}

function loadMerges() {
  try {
    const m = JSON.parse(localStorage.getItem(MERGES_KEY) || '{}');
    if (m && typeof m === 'object' && !Array.isArray(m)) {
      return Object.fromEntries(Object.entries(m).filter(([k, v]) => typeof v === 'string' && k !== v));
    }
  } catch (e) {}
  return {};
}
function saveMerges(merges) {
  try { localStorage.setItem(MERGES_KEY, JSON.stringify(merges)); } catch (e) {}
}

// api: the dashboard's fetch wrapper (handles sign-in). saveBlob(blob, name):
// its download helper.
export function initYears({ api, saveBlob }) {
  const sheets = []; // years.readYear results, plus {id}
  let nextId = 1;
  let merges = loadMerges();
  let data = null;
  let view = 'people';
  let person = '';
  let readErrors = ''; // from the last sheets added

  // ---- adding sheets ----

  async function addFile(file) {
    if (file.size === 0) throw new Error(`${file.name}: the file is empty.`);
    if (file.size > MAX_UPLOAD_BYTES) throw new Error(`${file.name}: over 10 MB — too big.`);
    let tabs;
    try {
      tabs = await readTabs(new Uint8Array(await file.arrayBuffer()), SOURCE_TAB);
    } catch (e) {
      if (e instanceof SpreadsheetError) throw new Error(`${file.name}: ${e.message}`);
      throw new Error(`${file.name}: couldn't read the file.`);
    }
    add(years.readYear(tabs, file.name));
  }

  async function addSaved(sheet) {
    const res = await api(`/sheets/${encodeURIComponent(sheet.row_id)}/values`);
    if (!res.ok) {
      const text = await res.text().catch(() => '');
      throw new Error(`${sheet.display_name}: ${text.trim() || `request failed (${res.status})`}`);
    }
    const body = await res.json().catch(() => null);
    if (!body || !Array.isArray(body.rows)) throw new Error(`${sheet.display_name}: unexpected answer from the server.`);
    add(years.readYear([body.rows], sheet.display_name));
  }

  function add(year) {
    if (!year.records.length) throw new Error(`${year.name}: no orders found in it.`);
    sheets.push({ ...year, id: nextId++ });
  }

  async function addAll(work) {
    setStatus('Reading…');
    const errors = [];
    for (const w of work) {
      try { await w(); } catch (e) { errors.push((e && e.message) || 'Something went wrong.'); }
    }
    readErrors = errors.join(' ');
    refresh();
  }

  $('years-file').addEventListener('change', (e) => {
    const files = [...e.target.files];
    e.target.value = '';
    addAll(files.map((f) => () => addFile(f)));
  });
  const drop = $('years-drop');
  drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('dragging'); });
  drop.addEventListener('dragleave', () => drop.classList.remove('dragging'));
  drop.addEventListener('drop', (e) => {
    e.preventDefault();
    drop.classList.remove('dragging');
    addAll([...e.dataTransfer.files].map((f) => () => addFile(f)));
  });

  let saved = [];
  const savedSelect = $('years-saved');
  if (savedSelect) {
    savedSelect.addEventListener('change', () => {
      const sheet = saved.find((s) => String(s.row_id) === savedSelect.value);
      savedSelect.value = '';
      if (sheet) addAll([() => addSaved(sheet)]);
    });
  }

  // ---- the list of sheets ----

  function renderList() {
    const wrap = $('years-list');
    wrap.hidden = !sheets.length;
    wrap.textContent = '';
    if (!sheets.length) return;
    const dupes = new Set(years.duplicateYears(sheets));
    const body = el('tbody');
    for (const s of sheets) {
      const input = el('input', { type: 'text', inputmode: 'numeric', maxlength: '4', placeholder: 'Year',
                                  'aria-label': `Year of ${s.name}`, value: s.year ?? '' });
      if (s.year === null || dupes.has(s.year)) input.classList.add('missing');
      input.addEventListener('change', () => {
        s.year = years.parseYear(input.value);
        refresh();
      });
      const remove = el('button', { type: 'button', text: 'Remove' });
      remove.addEventListener('click', () => {
        sheets.splice(sheets.indexOf(s), 1);
        refresh();
      });
      const people = new Set(s.records.map((r) => r.person)).size;
      const parts = new Set(s.records.map((r) => r.element_id)).size;
      body.appendChild(el('tr', {}, [
        el('td', { class: 'wrap', text: s.name }),
        el('td', {}, [input]),
        el('td', { class: 'num', text: String(people) }),
        el('td', { class: 'num', text: String(parts) }),
        el('td', { text: s.priceColumn ? `“${s.priceColumn}”, ${s.prices.size} parts` : 'none found' }),
        el('td', {}, [remove]),
      ]));
    }
    wrap.appendChild(el('table', { class: 'data' }, [
      el('thead', {}, [el('tr', {}, ['Sheet', 'Year', 'People', 'Parts', 'Price column', ''].map((h, i) =>
        el('th', { class: i === 2 || i === 3 ? 'num' : '', text: h })))]),
      body,
    ]));
  }

  // ---- names ----

  function renderNames() {
    const names = data.people.map((p) => p.name);
    const merged = Object.keys(merges).length;
    $('years-names-summary').textContent =
      `Names: ${names.length} people` + (merged ? `, ${merged} merged` : '');

    const suggest = $('years-suggest');
    suggest.textContent = '';
    for (const [a, b] of years.nameSuggestions(data.people)) {
      const btn = el('button', { type: 'button', text: `Merge “${a}” into “${b}”` });
      btn.addEventListener('click', () => merge(a, b));
      suggest.appendChild(el('div', { class: 'row' }, ['Same person? ', btn]));
    }

    for (const id of ['years-merge-from', 'years-merge-into']) {
      const select = $(id);
      const was = select.value;
      select.textContent = '';
      for (const n of names) select.appendChild(el('option', { value: n, text: n }));
      if (names.includes(was)) select.value = was;
    }

    const list = $('years-merges');
    list.textContent = '';
    for (const p of data.people) {
      // Only real merges: spellings that differ in more than capitals and spacing.
      const byKey = new Map(p.spellings.map((s) => [years.personKey(s), s]));
      if (byKey.size < 2) continue;
      const undo = el('button', { type: 'button', text: 'Undo' });
      undo.addEventListener('click', () => {
        for (const s of p.spellings) delete merges[years.personKey(s)];
        saveMerges(merges);
        refresh();
      });
      list.appendChild(el('li', { class: 'row' }, [`${p.name}: ${[...byKey.values()].join(' / ')} `, undo]));
    }
  }

  function merge(from, into) {
    const a = years.mergedKey(years.personKey(from), merges);
    const b = years.mergedKey(years.personKey(into), merges);
    if (a === b) return;
    merges[a] = b;
    saveMerges(merges);
    refresh();
  }
  $('years-merge-btn').addEventListener('click', () => merge($('years-merge-from').value, $('years-merge-into').value));

  // ---- the tables ----

  const money = years.formatMoney;
  const th = (text, num) => el('th', { class: num ? 'num' : '', text });
  const td = (text, cls) => el('td', { class: cls || '', text: text ?? '' });
  const partCells = (r) => [td(r.element_id), td(r.description, 'wrap'), td(r.color)];

  function matches(r, filter) {
    if (!filter) return true;
    return [r.element_id, r.description, r.color, r.person].some((v) => v && v.toLowerCase().includes(filter));
  }

  function table(head, rows, foot) {
    const t = el('table', { class: 'data' }, [el('thead', {}, [el('tr', {}, head)]), el('tbody', {}, rows)]);
    if (foot) t.appendChild(el('tfoot', {}, [el('tr', {}, foot)]));
    return t;
  }

  const VIEWS = {
    people: {
      note: () => 'Lots (different parts), pieces and amount spent per person and year. Spent counts parts with a price only.',
      csv: () => [years.peopleCsv(data), 'people by year.csv'],
      render(filter) {
        const ys = data.years;
        const rows = years.peopleSummary(data).filter((t) => matches(t, filter)).map((t) => {
          const cells = [el('td', {}, [personLink(t.person)])];
          for (const y of ys) {
            const v = t.years[y];
            cells.push(td(v ? `${v.lots} / ${formatCount(v.pieces)}` : '', 'num'), td(v ? money(v.spent) : '', 'num'));
          }
          cells.push(td(String(t.lots), 'num'), td(formatCount(t.pieces), 'num'), td(money(t.spent), 'num'));
          return el('tr', {}, cells);
        });
        return table([th('Person'), ...ys.flatMap((y) => [th(`${y} lots / pieces`, true), th(`${y} spent`, true)]),
          th('Lots', true), th('Pieces', true), th('Spent', true)], rows);
      },
    },
    person: {
      note: () => (person ? `Every part ${person} got, by year. Click a name in “People” to jump here.` : ''),
      csv: () => [years.inventoryCsv({ ...data, entries: data.entries.filter((e) => e.person === person) }),
                  `${person} parts by year.csv`],
      render(filter) {
        const ys = data.years;
        const inv = years.personInventory(data, person).filter((r) => matches(r, filter));
        const rows = inv.map((r) => el('tr', {}, [...partCells(r),
          ...ys.map((y) => td(r.qty[y] ? formatCount(r.qty[y]) : '', 'num')),
          td(formatCount(r.total), 'num'), td(money(r.spent), 'num')]));
        const sum = (f) => inv.reduce((s, r) => s + f(r), 0);
        const foot = [td(`${inv.length} part${inv.length === 1 ? '' : 's'}`), td(''), td(''),
          ...ys.map((y) => td(formatCount(sum((r) => r.qty[y] || 0)), 'num')),
          td(formatCount(sum((r) => r.total)), 'num'), td(money(sum((r) => r.spent)), 'num')];
        return table([th('Element ID'), th('Description'), th('Color'), ...ys.map((y) => th(String(y), true)),
          th('Total', true), th('Spent', true)], rows, foot);
      },
    },
    prices: {
      note: () => 'The price paid per piece each year, and how many were bought at it (everyone together).',
      csv: () => [years.pricesByYearCsv(data), 'prices by year.csv'],
      render(filter) {
        const rows = years.pricesByYear(data).filter((r) => matches(r, filter)).map((r) => el('tr', {}, [
          ...partCells(r), td(String(r.year), 'num'), td(r.price === null ? '—' : money(r.price), 'num'),
          td(formatCount(r.qty), 'num'), td(String(r.people), 'num')]));
        return table([th('Element ID'), th('Description'), th('Color'), th('Year', true), th('Price', true),
          th('Qty', true), th('People', true)], rows);
      },
    },
    average: {
      note: () => 'One row per part. The average is of the yearly prices: each year counts once, however many were bought.',
      csv: () => [years.averagePricesCsv(data), 'average prices.csv'],
      render(filter) {
        const ys = data.years;
        const rows = years.averagePrices(data).filter((r) => matches(r, filter)).map((r) => el('tr', {}, [
          ...partCells(r), ...ys.map((y) => td(money(r.prices[y]), 'num')),
          td(r.average === null ? '—' : money(r.average), 'num'), td(money(r.low), 'num'), td(money(r.high), 'num'),
          td(formatCount(r.qty), 'num')]));
        return table([th('Element ID'), th('Description'), th('Color'), ...ys.map((y) => th(String(y), true)),
          th('Average', true), th('Low', true), th('High', true), th('Qty', true)], rows);
      },
    },
  };

  function personLink(name) {
    const a = el('a', { href: '#', text: name });
    a.addEventListener('click', (e) => {
      e.preventDefault();
      person = name;
      showView('person');
    });
    return a;
  }

  function showView(v) {
    view = v;
    for (const t of document.querySelectorAll('#years-view [role=tab]')) {
      t.setAttribute('aria-selected', String(t.dataset.view === v));
    }
    renderView();
  }
  for (const t of document.querySelectorAll('#years-view [role=tab]')) {
    t.addEventListener('click', () => showView(t.dataset.view));
  }

  function renderView() {
    const select = $('years-person');
    select.hidden = view !== 'person';
    $('years-csv-all').hidden = view !== 'person';
    if (view === 'person') {
      select.textContent = '';
      for (const p of data.people) select.appendChild(el('option', { value: p.name, text: p.name }));
      if (!data.people.some((p) => p.name === person)) person = data.people[0]?.name ?? '';
      select.value = person;
    }
    const v = VIEWS[view];
    $('years-view-note').textContent = v.note();
    const wrap = $('years-table');
    wrap.textContent = '';
    wrap.appendChild(v.render($('years-filter').value.trim().toLowerCase()));
  }
  $('years-person').addEventListener('change', (e) => { person = e.target.value; renderView(); });
  $('years-filter').addEventListener('input', () => { if (data) renderView(); });
  $('years-csv').addEventListener('click', () => {
    const [text, name] = VIEWS[view].csv();
    saveBlob(new Blob([text], { type: 'text/csv' }), name);
  });
  $('years-csv-all').addEventListener('click', () => {
    saveBlob(new Blob([years.inventoryCsv(data)], { type: 'text/csv' }), 'everyone parts by year.csv');
  });

  // ---- everything, after any change ----

  function refresh() {
    renderList();
    const missing = sheets.filter((s) => s.year === null);
    const dupes = years.duplicateYears(sheets);
    const ready = sheets.length > 0 && !missing.length && !dupes.length;
    $('years-view').hidden = !ready;
    const problems = [readErrors];
    if (missing.length) problems.push(`Fill in the year for ${missing.map((s) => s.name).join(', ')}.`);
    if (dupes.length) problems.push(`More than one sheet is for ${dupes.join(', ')} — fix a year or remove a sheet.`);
    const text = problems.filter(Boolean).join(' ');
    setStatus(text, !!text);
    if (!ready) return;
    data = years.combine(sheets, merges);
    renderNames();
    showView(view);
  }

  return {
    // The signed-in user's saved sheets ({row_id, display_name}), for the
    // "Add a saved Google Sheet" list.
    setSavedSheets(list) {
      saved = list;
      if (!savedSelect) return;
      savedSelect.length = 1;
      for (const s of list) savedSelect.appendChild(el('option', { value: String(s.row_id), text: s.display_name }));
    },
  };
}

// The editor for families (groups.js): pick the people in a family (any
// number, at least two), name it if you like, add or take out people
// later, or remove it, plus the "combine" switch. Used by the open sheet's
// Families panel and by Compare years; where the families are kept is the
// caller's business.
//
//   const ed = groupEditor(container, {
//     people: () => names,           // who can be picked
//     get: () => groups,             // the current families
//     set: (groups) => ...,          // store a change (already normalized)
//     combineLabel: 'Pack each family as one person',
//   });
//   ed.render()                      // after the people or families changed

import { combo } from './combo.js';
import { MAX_GROUPS, MAX_MEMBERS, defaultName, normalizeGroups, personKey } from './groups.js';

function el(tag, props, children) {
  const e = document.createElement(tag);
  if (props) for (const [k, v] of Object.entries(props)) {
    if (k === 'text') e.textContent = v;
    else e.setAttribute(k, v);
  }
  if (children) for (const c of children) e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  return e;
}

export function groupEditor(container, { people, get, set, combineLabel, onFull = () => {} }) {
  container.classList.add('group-editor');
  let pending = []; // the new family being picked
  let message = '';

  function update(change) {
    const next = structuredClone(get());
    change(next);
    const wanted = next.list.filter((g) => g.members.length >= 2).length;
    const saved = normalizeGroups(next);
    set(saved);
    if (saved.list.length < Math.min(wanted, MAX_GROUPS) || wanted > MAX_GROUPS) onFull();
    render();
  }

  // People not in any family yet (nor in the one being picked).
  const free = () => {
    const taken = new Set([...get().list.flatMap((g) => g.members), ...pending].map(personKey));
    return people().filter((n) => !taken.has(personKey(n)));
  };

  const chip = (name, title, onRemove, absent = false) => {
    const x = el('button', { type: 'button', class: 'chip-x', title, 'aria-label': title, text: '×' });
    x.addEventListener('click', onRemove);
    return el('span', { class: 'chip' + (absent ? ' absent' : ''), title: absent ? 'Not on this sheet' : '' }, [name, x]);
  };

  function render() {
    const groups = get();
    container.textContent = '';
    const here = new Set(people().map(personKey));

    const box = el('input', { type: 'checkbox' });
    box.checked = groups.combine;
    box.addEventListener('change', () => update((g) => { g.combine = box.checked; }));
    container.appendChild(el('label', { class: 'inline-field group-combine' }, [box, combineLabel]));

    const list = el('ul', { class: 'group-list' });
    groups.list.forEach((g, i) => {
      const name = el('input', { type: 'text', value: g.name, 'aria-label': 'Family name', maxlength: '60' });
      name.addEventListener('change', () => update((n) => { n.list[i].name = name.value; }));
      const members = el('div', { class: 'group-members' });
      g.members.forEach((m, j) => {
        members.appendChild(chip(m, `Take ${m} out of the family`,
          () => update((n) => { n.list[i].members.splice(j, 1); }), !here.has(personKey(m))));
      });
      if (g.members.length < MAX_MEMBERS) {
        const add = el('input', { type: 'text', placeholder: 'Add someone…', 'aria-label': `Add someone to ${g.name}` });
        members.appendChild(el('span', { class: 'combo' }, [add]));
        combo(add, { items: free, clearOnPick: true,
                     onPick: (who) => update((n) => { n.list[i].members.push(who); }) });
      }
      const drop = el('button', { type: 'button', class: 'danger', text: 'Remove family' });
      drop.addEventListener('click', () => update((n) => { n.list.splice(i, 1); }));
      list.appendChild(el('li', { class: 'group' }, [el('div', { class: 'group-head' }, [name, drop]), members]));
    });
    container.appendChild(list);

    // A new family: pick its people one by one (two or more), then make it.
    const fresh = el('div', { class: 'group group-new' });
    const head = el('div', { class: 'group-head group-new-title' }, [el('span', { class: 'grow', text: 'New family' })]);
    fresh.appendChild(head);
    const picked = el('div', { class: 'group-members' });
    pending.forEach((m, j) => picked.appendChild(chip(m, `Take ${m} out`, () => { pending.splice(j, 1); render(); })));
    const add = el('input', { type: 'text', placeholder: pending.length ? 'Add someone else…' : 'Add a person…',
                              'aria-label': 'Add a person to the new family' });
    picked.appendChild(el('span', { class: 'combo' }, [add]));
    combo(add, { items: free, clearOnPick: true, onPick: (who) => {
      if (pending.length < MAX_MEMBERS) pending.push(who);
      message = '';
      render();
      container.querySelector('.group-new .combo input')?.focus();
    } });
    fresh.appendChild(picked);
    const make = el('button', { type: 'button', class: 'primary',
                                text: 'Make family',
                                title: pending.length >= 2 ? `Make “${defaultName(pending)}”` : 'Pick two or more people first' });
    // Pressing the button mustn't take the focus from the box first: a name
    // still being typed there would be lost. It's taken as picked if it
    // matches someone exactly.
    make.addEventListener('mousedown', (e) => e.preventDefault());
    make.addEventListener('click', () => {
      const typed = personKey(add.value);
      const match = typed ? free().find((n) => personKey(n) === typed) : undefined;
      if (match !== undefined && pending.length < MAX_MEMBERS) pending.push(match);
      if (pending.length < 2) {
        message = 'Pick at least two people for a family.';
        render();
        return;
      }
      const members = pending;
      pending = [];
      message = '';
      update((n) => { n.list.push({ name: '', members }); });
    });
    // Above the box, not under or beside it: the open list could cover it.
    head.appendChild(make);
    if (message) fresh.appendChild(el('p', { class: 'status error', text: message }));
    container.appendChild(fresh);
  }

  render();
  return { render };
}

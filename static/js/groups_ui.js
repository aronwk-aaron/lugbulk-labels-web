// The editor for groups (couples, families — see groups.js): make a group
// from two people, add or remove members, rename it, or drop it, plus the
// "combine" switch. Used by the open sheet's Groups panel and by Compare
// years; where the groups are kept is the caller's business.
//
//   const ed = groupEditor(container, {
//     people: () => names,           // who can be picked
//     get: () => groups,             // the current groups
//     set: (groups) => ...,          // store a change (already normalized)
//     combineLabel: 'Pack each group as one person',
//   });
//   ed.render()                      // after the people or groups changed

import { combo } from './combo.js';
import { MAX_GROUPS, normalizeGroups, personKey } from './groups.js';

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

  function update(change) {
    const next = structuredClone(get());
    change(next);
    const wanted = next.list.filter((g) => g.members.length >= 2).length;
    const saved = normalizeGroups(next);
    set(saved);
    if (saved.list.length < Math.min(wanted, MAX_GROUPS) || wanted > MAX_GROUPS) onFull();
    render();
  }

  // People not in any group yet.
  const free = () => {
    const grouped = new Set(get().list.flatMap((g) => g.members.map(personKey)));
    return people().filter((n) => !grouped.has(personKey(n)));
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
      const name = el('input', { type: 'text', value: g.name, 'aria-label': 'Group name', maxlength: '60' });
      name.addEventListener('change', () => update((n) => { n.list[i].name = name.value; }));
      const members = el('div', { class: 'group-members' });
      g.members.forEach((m, j) => {
        const x = el('button', { type: 'button', class: 'chip-x', title: `Take ${m} out of the group`,
                                 'aria-label': `Take ${m} out of the group`, text: '×' });
        x.addEventListener('click', () => update((n) => { n.list[i].members.splice(j, 1); }));
        const chip = el('span', { class: 'chip' + (here.has(personKey(m)) ? '' : ' absent'),
                                  title: here.has(personKey(m)) ? '' : 'Not on this sheet' }, [m, x]);
        members.appendChild(chip);
      });
      const add = el('input', { type: 'text', placeholder: 'Add someone…', 'aria-label': `Add someone to ${g.name}` });
      const addWrap = el('span', { class: 'combo' }, [add]);
      members.appendChild(addWrap);
      combo(add, { items: free, clearOnPick: true,
                   onPick: (who) => update((n) => { n.list[i].members.push(who); }) });
      const drop = el('button', { type: 'button', class: 'danger', text: 'Remove group' });
      drop.addEventListener('click', () => update((n) => { n.list.splice(i, 1); }));
      list.appendChild(el('li', { class: 'group' }, [
        el('div', { class: 'group-head' }, [name, drop]), members]));
    });
    container.appendChild(list);

    // A new group: two people, then "Group them".
    const first = el('input', { type: 'text', placeholder: 'Someone…', 'aria-label': 'First person' });
    const second = el('input', { type: 'text', placeholder: 'and someone else…', 'aria-label': 'Second person' });
    const make = el('button', { type: 'button', text: 'Group them' });
    const c1 = combo(first, { items: free });
    const c2 = combo(second, { items: () => free().filter((n) => n !== c1.value) });
    make.addEventListener('click', () => {
      if (!c1.value || !c2.value || c1.value === c2.value) return;
      update((n) => { n.list.push({ name: '', members: [c1.value, c2.value] }); });
    });
    container.appendChild(el('div', { class: 'group-new' }, [
      'New group: ', el('span', { class: 'combo' }, [first]), el('span', { class: 'combo' }, [second]), make]));
  }

  render();
  return { render };
}

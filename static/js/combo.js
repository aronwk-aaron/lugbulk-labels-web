// A searchable dropdown ("combobox") for picking one name from a long list:
// type to filter, arrow keys and Enter (or a click) to pick. Turns an
// <input type="text"> into one; its list is added right after it, so the
// input's parent should be position: relative (the .combo class).
//
//   const c = combo(input, { items: () => names, onPick: (name) => ... });
// A name typed out in full counts as picked when the box loses focus.
//   c.value          the picked name ('' if none)
//   c.set(name)      show a name as picked, without calling onPick
//   c.refresh()      re-read items() (e.g. after the names changed)

let nextId = 1;

export function combo(input, { items, onPick = () => {}, clearOnPick = false }) {
  const id = `combo-${nextId++}`;
  const list = document.createElement('ul');
  list.id = id;
  list.className = 'combo-list';
  list.setAttribute('role', 'listbox');
  list.hidden = true;
  input.after(list);
  input.setAttribute('role', 'combobox');
  input.setAttribute('autocomplete', 'off');
  input.setAttribute('spellcheck', 'false');
  input.setAttribute('aria-autocomplete', 'list');
  input.setAttribute('aria-expanded', 'false');
  input.setAttribute('aria-controls', id);

  let value = '';
  let shown = [];
  let active = -1;

  function render(query) {
    const words = query.toLowerCase().split(/\s+/).filter(Boolean);
    shown = items().filter((name) => words.every((w) => name.toLowerCase().includes(w)));
    list.textContent = '';
    shown.forEach((name, n) => {
      const li = document.createElement('li');
      li.id = `${id}-${n}`;
      li.setAttribute('role', 'option');
      li.setAttribute('aria-selected', String(name === value));
      li.textContent = name;
      // mousedown, not click: keep focus in the input so blur doesn't close first.
      li.addEventListener('mousedown', (e) => {
        e.preventDefault();
        pick(name);
      });
      li.addEventListener('mousemove', () => setActive(n));
      list.appendChild(li);
    });
    if (!shown.length) {
      const li = document.createElement('li');
      li.className = 'none';
      li.textContent = 'No one matches.';
      list.appendChild(li);
    }
  }

  function setActive(n) {
    const prev = list.querySelector('.active');
    if (prev) prev.classList.remove('active');
    active = n;
    const li = n >= 0 ? document.getElementById(`${id}-${n}`) : null;
    if (li) {
      li.classList.add('active');
      li.scrollIntoView({ block: 'nearest' });
      input.setAttribute('aria-activedescendant', li.id);
    } else {
      input.removeAttribute('aria-activedescendant');
    }
  }

  function open(query) {
    render(query);
    list.hidden = false;
    input.setAttribute('aria-expanded', 'true');
    const at = shown.indexOf(value);
    setActive(query ? (shown.length ? 0 : -1) : at);
  }

  function close() {
    list.hidden = true;
    input.setAttribute('aria-expanded', 'false');
    input.removeAttribute('aria-activedescendant');
    active = -1;
  }

  function pick(name) {
    value = clearOnPick ? '' : name;
    input.value = value;
    close();
    onPick(name);
  }

  // Focusing starts a fresh search: the box empties (the current name
  // shows as its placeholder) and the whole list opens.
  const hint = input.placeholder;
  function startSearch() {
    input.placeholder = value || hint;
    input.value = '';
    open('');
  }
  input.addEventListener('focus', startSearch);
  input.addEventListener('mousedown', () => {
    if (document.activeElement === input && list.hidden) startSearch();
  });
  input.addEventListener('input', () => open(input.value.trim()));
  // Leaving the box with a name typed out in full (any capitals) picks it,
  // as if it had been chosen from the list.
  input.addEventListener('blur', () => {
    const typed = input.value.trim().replace(/\s+/g, ' ').toLowerCase();
    const match = typed ? items().find((n) => n.trim().replace(/\s+/g, ' ').toLowerCase() === typed) : undefined;
    close();
    input.placeholder = hint;
    if (match !== undefined && match !== value) {
      pick(match);
      return;
    }
    input.value = value;
  });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      if (list.hidden) {
        open('');
        return;
      }
      if (!shown.length) return;
      const step = e.key === 'ArrowDown' ? 1 : -1;
      setActive((active + step + shown.length) % shown.length);
    } else if (e.key === 'Enter') {
      if (!list.hidden && active >= 0 && shown[active]) {
        e.preventDefault();
        pick(shown[active]);
      }
    } else if (e.key === 'Escape') {
      if (!list.hidden) {
        e.preventDefault();
        close();
        input.value = value;
      }
    }
  });

  return {
    get value() {
      return value;
    },
    set(name) {
      value = name;
      if (document.activeElement !== input) input.value = name;
    },
    refresh() {
      if (!items().includes(value)) this.set('');
      if (!list.hidden) render(input.value.trim());
    },
  };
}

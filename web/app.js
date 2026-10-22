/* Lumen console UI.
 *
 * ONE RULE, and it is the one that matters: this file never mutates the show
 * directly. Every change is a COMMAND STRING sent to /cmd, because §16.2 makes
 * the command layer the only mutation path. So anything the UI can do is also
 * typeable, scriptable, bindable and undoable — and nothing the UI does can
 * exist outside the command vocabulary.
 *
 * Rendering is a plain redraw from the latest snapshot. A console at a few Hz
 * has no need for a virtual DOM, and a redraw that reads only from state
 * cannot drift out of sync with the engine.
 */

const S = {
  state: null,
  windows: ['Fixtures', 'Executors', 'Programmer', 'Groups', 'Presets', 'Output',
            'Cues', 'Laser', 'History'],
  open: ['Fixtures', 'Executors', 'Programmer', 'Groups', 'Presets', 'Output'],
  history: [],
  historyAt: -1,
  connected: false,
  both: false,          // §9.6: show every surface page at once
  editMode: false,      // §9.4: edit mode vs run mode, per surface
};

/* ------------------------------------------------------------------ net */
async function send(command) {
  try {
    const res = await fetch('/cmd', { method: 'POST', body: command });
    const j = await res.json();
    showReply(j);
    await refresh();
    return j;
  } catch (e) {
    showReply({ ok: false, parsed: true, message: 'not connected', rows: [] });
    return null;
  }
}

async function refresh() {
  try {
    const res = await fetch('/state');
    S.state = await res.json();
    S.connected = true;
  } catch (e) {
    S.connected = false;
  }
  render();
}

/* --------------------------------------------------------------- helpers */
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};

const pct = v => (v === null || v === undefined) ? null : Math.round(v * 100);

/* A level as a number over a bar: exact and scannable at once. */
function levelCell(v) {
  const td = el('td', 'num');
  const p = pct(v);
  const span = el('span', 'lvl', p === null ? '·' : String(p));
  if (p === null || p === 0) span.classList.add('zero');
  if (p !== null && p > 0) {
    const bar = el('i');
    bar.style.width = Math.max(3, p) + '%';
    span.appendChild(bar);
  }
  td.appendChild(span);
  return td;
}

function showReply(j) {
  const r = document.getElementById('reply');
  if (!j) return;
  r.className = j.ok ? 'good' : 'bad';
  r.textContent = j.message || (j.ok ? 'ok' : 'failed');
  if (j.rows && j.rows.length) toast(j.rows.join('\n'), !j.ok);
  else if (!j.ok) toast(j.message || 'failed', true);
}

function toast(text, bad) {
  const box = document.getElementById('toast');
  const t = el('div', 'toast' + (bad ? ' bad' : ''));
  t.textContent = text;
  box.appendChild(t);
  setTimeout(() => t.remove(), 5000);
}

/* ------------------------------------------------------------- windows */
function win(title, symbol, context, build, span, rows) {
  const w = el('div', 'win');
  w.style.gridColumn = 'span ' + (span || 4);
  if (rows) w.style.gridRow = 'span ' + rows;

  const h = el('header');
  h.appendChild(el('span', 'sym', symbol));
  h.appendChild(el('span', null, title));
  if (context) h.appendChild(el('span', 'ctx', context));
  h.appendChild(el('span', 'gear', '⚙'));
  w.appendChild(h);

  const body = el('div', 'body');
  build(body);
  w.appendChild(body);
  return w;
}

/* §11.10: the fixture sheet shows current output per fixture and channel. */
function buildFixtures(body) {
  const st = S.state;
  const table = el('table');
  const head = el('tr');
  ['', 'Fixture', 'Type', 'Addr', 'Dim', 'R', 'G', 'B', 'Pan', 'Tilt'].forEach(h => {
    const th = el('th', h === 'Fixture' || h === 'Type' ? '' : 'num', h);
    head.appendChild(th);
  });
  const thead = el('thead');
  thead.appendChild(head);
  table.appendChild(thead);

  const tb = el('tbody');
  const pick = (f, name) => {
    const a = f.attrs.find(x => x.attr === name);
    return a ? a.value : null;
  };

  for (const f of st.fixtures) {
    const tr = el('tr', 'clickable' + (f.selected ? ' sel' : ''));
    tr.onclick = () => send('Fixture ' + f.id);

    tr.appendChild(el('td', 'num muted', String(f.id)));
    tr.appendChild(el('td', null, f.name));
    tr.appendChild(el('td', 'muted', f.type));
    tr.appendChild(el('td', 'num muted', f.universe + '.' + f.address));
    for (const attr of ['Dimmer', 'Red', 'Green', 'Blue', 'Pan', 'Tilt'])
      tr.appendChild(levelCell(pick(f, attr)));
    tb.appendChild(tr);
  }
  table.appendChild(tb);
  body.appendChild(table);
  if (!st.fixtures.length) body.appendChild(el('div', 'empty', 'Nothing patched.'));
}

/* §9: the executor surface lives in surface.js, which renders both reference
 * styles from the same state. */
function buildExecutors(body) {
  if (!S.state.executors.length) {
    body.appendChild(el('div', 'empty', 'No executors. Try: Assign 1 1'));
    return;
  }
  buildExecutorSurface(body);
}

function buildProgrammer(body) {
  const st = S.state;
  const info = el('div');
  info.style.cssText = 'font:11px var(--mono);color:var(--dim);margin-bottom:8px';
  info.textContent = st.programmer.count + ' value(s) · ' +
    st.programmer.selection.length + ' selected' +
    (st.programmer.hasActive ? ' · active' : '');
  body.appendChild(info);

  const row = el('div', 'chips');
  const add = (label, cmd, cls) => {
    const c = el('button', cls);
    c.textContent = label;
    c.onclick = () => send(cmd);
    c.style.marginRight = '4px';
    row.appendChild(c);
  };
  add('Clear', 'Clear');
  add('Clear All', 'ClearAll');
  add('Highlight', 'Highlight toggle', st.programmer.highlight ? 'on' : '');
  add('Store next cue', 'Store Cue');
  add('Undo', 'Undo');
  add('Redo', 'Redo');
  body.appendChild(row);

  // Faders read back from the FIRST selected fixture, so they show what is
  // actually set rather than sitting at zero over live values — a fader whose
  // position disagrees with the output is how an operator gets surprised.
  const first = st.fixtures.find(f => f.selected);
  const current = name => {
    if (!first) return null;
    const a = first.attrs.find(x => x.attr === name);
    return a && a.value !== null ? Math.round(a.value * 100) : null;
  };

  const levels = el('div');
  levels.style.marginTop = '10px';
  for (const [label, attr] of [['Intensity', 'Dimmer'], ['Red', 'Red'],
                               ['Green', 'Green'], ['Blue', 'Blue']]) {
    const now = current(attr);
    const r = el('div');
    r.style.cssText = 'display:flex;align-items:center;gap:8px;margin:5px 0';
    const name = el('span', null, label);
    name.style.cssText = 'width:62px;font-size:11px;color:var(--dim)';
    const sl = el('input');
    sl.type = 'range'; sl.min = 0; sl.max = 100; sl.value = now === null ? 0 : now;
    sl.style.flex = '1';
    sl.disabled = !first;
    const out = el('b');
    out.style.cssText = 'font:11px var(--mono);width:30px;text-align:right';
    out.textContent = now === null ? '·' : String(now);
    if (now === null) out.style.color = 'var(--dimmer)';
    sl.oninput = () => { out.textContent = sl.value; out.style.color = ''; };
    sl.onchange = () =>
      send('At ' + (attr === 'Dimmer' ? '' : attr + ' ') + sl.value);
    r.append(name, sl, out);
    levels.appendChild(r);
  }
  body.appendChild(levels);
  if (!first)
    body.appendChild(el('div', 'empty', 'Select a fixture to use the faders.'));
}

function buildGroups(body) {
  const chips = el('div', 'chips');
  for (const g of S.state.groups) {
    const c = el('div', 'chip');
    c.onclick = () => send('Group ' + g.id);
    c.appendChild(document.createTextNode(g.name));
    c.appendChild(el('span', 'c-sub', g.members));
    chips.appendChild(c);
  }
  body.appendChild(chips);
  if (!S.state.groups.length) body.appendChild(el('div', 'empty', 'No groups.'));
}

function buildPresets(body) {
  for (const pool of S.state.presetPools) {
    const label = el('div', 'uni-label', pool.name + ' · ' + pool.group);
    body.appendChild(label);
    const chips = el('div', 'chips');
    for (const p of pool.presets) {
      const c = el('div', 'chip');
      c.textContent = p.name;
      c.onclick = () => send('Preset ' + pool.id + '.' + p.id);
      chips.appendChild(c);
    }
    body.appendChild(chips);
  }
  if (!S.state.presetPools.length) body.appendChild(el('div', 'empty', 'No presets.'));
}

/* What would actually leave the desk. */
function buildOutput(body) {
  const st = S.state;
  for (const u of st.universes) {
    body.appendChild(el('div', 'uni-label',
      'Universe ' + u.number + ' · ' + u.used + ' ch · ' + u.frames + ' frames'));
    const grid = el('div', 'dmx');
    u.data.forEach((v, i) => {
      const c = el('div', 'cell' + (v > 0 ? ' hot' : ''));
      if (v > 0) {
        const t = v / 255;
        c.style.background = 'rgb(' + Math.round(60 + 195 * t) + ',' +
          Math.round(100 + 120 * t) + ',60)';
      }
      c.appendChild(el('span', null, String(i + 1)));
      c.appendChild(el('b', null, String(v)));
      grid.appendChild(c);
    });
    body.appendChild(grid);
  }
  if (!st.universes.length)
    body.appendChild(el('div', 'empty', 'No output yet — the engine sends on tick.'));
}

function buildCues(body) {
  const st = S.state;
  for (const seq of st.sequences) {
    // Goto addresses an EXECUTOR, not a sequence — a sequence has no playback
    // position of its own. A sequence no executor is driving is listed but
    // cannot be jumped, and says so rather than failing on click.
    const driver = st.executors.find(x => x.sequence === seq.id);
    body.appendChild(el('div', 'uni-label',
      'Sequence ' + seq.id + ' · ' + (seq.name || 'unnamed') +
      (driver ? ' · exec ' + driver.id : ' · not assigned')));
    const table = el('table');
    const tb = el('tbody');
    for (const c of seq.cues) {
      const tr = el('tr', driver ? 'clickable' : '');
      if (driver) tr.onclick = () => send('Goto ' + c.number + ' ' + driver.id);
      tr.appendChild(el('td', 'num muted', c.number.toFixed(1)));
      tr.appendChild(el('td', null, c.name || ('Cue ' + c.number)));
      tr.appendChild(el('td', 'num muted', c.values + ' val'));
      tr.appendChild(el('td', 'num muted', c.fadeIn.toFixed(1) + 's'));
      tb.appendChild(tr);
    }
    table.appendChild(tb);
    body.appendChild(table);
  }
  if (!st.sequences.length) body.appendChild(el('div', 'empty', 'No cues stored.'));
}

/* §14.6: laser state visible, not buried. */
function buildLaser(body) {
  const st = S.state;
  const stop = el('button', 'danger' + (st.emergencyStop ? ' on' : ''));
  stop.textContent = st.emergencyStop ? 'E-STOP LATCHED — reset' : 'EMERGENCY STOP';
  stop.style.cssText = 'width:100%;margin-bottom:8px;font-weight:600;letter-spacing:1px';
  stop.onclick = () => toast(
    st.emergencyStop
      ? 'Reset is deliberate: it also disarms every zone.'
      : 'Emergency stop is on its own path — it blanks and disarms at once.');
  body.appendChild(stop);

  const list = el('div', 'safety');
  for (const z of st.laser) {
    const row = el('div', 'zone' + (z.armed ? ' armed' : '') +
      (st.emergencyStop ? ' estop' : ''));
    row.appendChild(el('span', 'dot'));
    row.appendChild(el('span', null, z.name));
    row.appendChild(el('span', 'z-reason', z.reason));
    list.appendChild(row);
  }
  body.appendChild(list);
  if (!st.laser.length) body.appendChild(el('div', 'empty', 'No laser zones.'));

  const note = el('div', 'empty',
    'Arming is hold-to-fire and cannot be done from a cue (§14.6).');
  body.appendChild(note);
}

/* §8.6: the history view, with undone entries marked. */
function buildHistory(body) {
  const st = S.state;
  const table = el('table');
  const tb = el('tbody');
  for (const h of st.history.slice().reverse()) {
    const tr = el('tr');
    tr.style.opacity = h.undone ? '0.4' : '1';
    tr.appendChild(el('td', 'num muted', String(h.id)));
    tr.appendChild(el('td', null, h.description));
    tr.appendChild(el('td', 'muted', h.scope));
    tb.appendChild(tr);
  }
  table.appendChild(tb);
  body.appendChild(table);
  if (!st.history.length) body.appendChild(el('div', 'empty', 'Nothing to undo yet.'));
}

const BUILDERS = {
  Fixtures: { sym: '▦', build: buildFixtures, span: 5, rows: 2 },
  Executors: { sym: '▶', build: buildExecutors, span: 12, rows: 2 },
  Programmer: { sym: '✎', build: buildProgrammer, span: 3 },
  Groups: { sym: '⬡', build: buildGroups, span: 3 },
  Presets: { sym: '◧', build: buildPresets, span: 3 },
  Output: { sym: '⇄', build: buildOutput, span: 6, rows: 1 },
  Cues: { sym: '≡', build: buildCues, span: 3 },
  Laser: { sym: '⚠', build: buildLaser, span: 3 },
  History: { sym: '↶', build: buildHistory, span: 3 },
};

/* ------------------------------------------------------------- rendering */
function render() {
  const link = document.getElementById('link');
  link.textContent = S.connected ? 'connected' : 'not connected';
  link.className = S.connected ? 'link-ok' : 'link-bad';
  if (!S.state) return;
  const st = S.state;

  document.getElementById('stat-fixtures').textContent = st.fixtures.length + ' fixtures';
  document.getElementById('stat-dmx').textContent =
    (st.universes[0] ? st.universes[0].frames : 0) + ' frames';
  document.getElementById('stat-prog').textContent = st.programmer.count + ' in prog';

  const gm = document.getElementById('gm');
  if (document.activeElement !== gm) gm.value = Math.round(st.grandMaster * 100);
  document.getElementById('gmv').textContent = Math.round(st.grandMaster * 100);
  document.getElementById('blackout').classList.toggle('on', st.blackout);

  // Window buttons: press to open, press again to close (§2.1).
  const bar = document.getElementById('windowbar');
  bar.innerHTML = '';
  for (const name of S.windows) {
    const b = el('button', S.open.includes(name) ? 'on' : '', name);
    b.onclick = () => {
      const at = S.open.indexOf(name);
      if (at >= 0) S.open.splice(at, 1);
      else S.open.push(name);
      render();
    };
    bar.appendChild(b);
  }

  const grid = document.getElementById('grid');
  // A redraw mid-drag would yank the fader out from under the hand. The
  // header and status still update; only the window grid holds still, and
  // only while something in it is being touched.
  if (grid.contains(document.activeElement) &&
      document.activeElement.tagName === 'INPUT') return;
  grid.innerHTML = '';
  for (const name of S.open) {
    const spec = BUILDERS[name];
    if (!spec) continue;
    let ctx = '';
    if (name === 'Fixtures') ctx = st.programmer.selection.length + ' selected';
    if (name === 'Executors') {
      const p = (st.surfaces || []).find(x => x.id === st.surface);
      ctx = S.both ? (st.surfaces || []).length + ' pages'
                   : (p ? p.name + ' · ' + p.style : 'no page');
    }
    if (name === 'Output') ctx = st.universes.length + ' universes';
    if (name === 'History') ctx = st.canUndo ? 'undo available' : 'nothing to undo';
    grid.appendChild(win(name, spec.sym, ctx, spec.build, spec.span, spec.rows));
  }
}

/* ------------------------------------------------------------ interaction */
function wire() {
  const cmd = document.getElementById('cmd');
  cmd.addEventListener('keydown', async ev => {
    if (ev.key === 'Enter') {
      const line = cmd.value.trim();
      if (!line) return;
      S.history.push(line);
      S.historyAt = S.history.length;
      cmd.value = '';
      await send(line);
    } else if (ev.key === 'ArrowUp') {
      ev.preventDefault();
      if (S.historyAt > 0) cmd.value = S.history[--S.historyAt] || '';
    } else if (ev.key === 'ArrowDown') {
      ev.preventDefault();
      if (S.historyAt < S.history.length - 1) cmd.value = S.history[++S.historyAt] || '';
      else { S.historyAt = S.history.length; cmd.value = ''; }
    }
  });

  const gm = document.getElementById('gm');
  gm.addEventListener('change', () => send('GrandMaster ' + gm.value));
  gm.addEventListener('input', () => {
    document.getElementById('gmv').textContent = gm.value;
  });

  document.getElementById('blackout').onclick = () =>
    send('Blackout ' + (S.state && S.state.blackout ? 'off' : 'on'));

  // The command line keeps focus, because a console is typed at.
  document.addEventListener('keydown', ev => {
    if (ev.target.tagName === 'INPUT') return;
    if (ev.key.length === 1 || ev.key === 'Backspace') {
      cmd.focus();
    }
  });
  cmd.focus();
}

wire();
refresh();
setInterval(refresh, 500);

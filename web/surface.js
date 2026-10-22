/* The executor surface, in both reference styles.
 *
 * §9 resolves the MA3/ChamSys question on paper — "flexibility in the canvas,
 * immediacy in the tile" — and §9.6 keeps BOTH. Keeping both only means
 * something if the style belongs to a PAGE rather than to a preference: a
 * show owns a busking wing and a programmed canvas at the same time, and
 * moves between them.
 *
 * So the engine owns the pages, this renders whichever one is current, and
 * switching or converting is a command like everything else (§16.2).
 *
 * What differs between the two is layout DISCIPLINE, not data and not
 * features:
 *
 *   wing      a fixed matrix. Every tile identical, numbered, never moves.
 *             You learn where things are by position and they are there
 *             forever. Rigid, and fast because of it.
 *
 *   canvas    free placement on a snapping grid. Tiles take the size they
 *             deserve and render more detail as they get bigger (§9.2).
 *             Flexible, and slower to build.
 *
 * Both forbid overlap (§9.1): on a busking surface, overlapping targets mean
 * mis-presses. The engine enforces that; a page that fails it came from a
 * damaged file, and is shown rather than quietly rendered wrong.
 */

/* --------------------------------------------------------------- shared */

/* §9.2: legend and colour are a VIEW of the assigned object, never typed.
 * The page stores a UID; this resolves it to whatever that object is now. */
function tileExec(tile) {
  return S.state.executors.find(e => e.id === tile.executor) || null;
}

function tileHue(x) {
  if (x.chase) return 'chase';
  const n = (x.sequenceName || x.label || '').toLowerCase();
  if (n.includes('blind') || n.includes('white')) return 'blinder';
  if (n.includes('spot') || n.includes('mover') || n.includes('pos')) return 'position';
  if (n.includes('pixel') || n.includes('video')) return 'pixel';
  if (n.includes('colour') || n.includes('color')) return 'colour';
  return 'default';
}

function fmtRate(x) {
  return x.chase ? x.rate.toFixed(1).replace(/\.0$/, '') + ' Hz' : '';
}

/* The fader strip on a tile. A command, like every other mutation. */
function tileFader(x, tall) {
  const bar = el('div', 'x-fader' + (tall ? ' tall' : ''));
  const fill = el('i');
  fill.style.height = Math.round(x.master * 100) + '%';
  bar.appendChild(fill);
  bar.onclick = ev => {
    ev.stopPropagation();
    const r = bar.getBoundingClientRect();
    const v = Math.round((1 - (ev.clientY - r.top) / r.height) * 100);
    send('ExecMaster ' + x.id + ' ' + Math.max(0, Math.min(100, v)));
  };
  bar.title = 'Master ' + Math.round(x.master * 100) + '% — click to set';
  return bar;
}

function wireTile(node, x) {
  node.onclick = () => send('Go ' + x.id);
  node.oncontextmenu = ev => { ev.preventDefault(); send('Off ' + x.id); };
  node.title = (x.sequenceName || 'unassigned') + ' — click Go, right-click Off';
}

/* ----------------------------------------------------------- the wing */
/*
 * Empty slots are DRAWN, not hidden: on a real wing the empty one is still a
 * button under a thumb, and position being permanent is the entire bargain.
 */
function buildWing(body, page) {
  const slots = page.cols * page.rows;

  const bar = el('div', 'wing-pages');
  const hint = el('span', 'wing-hint',
    'fixed ' + page.cols + ' × ' + page.rows + ' — a slot never moves');
  hint.style.marginLeft = '0';
  bar.appendChild(hint);
  body.appendChild(bar);

  const grid = el('div', 'wing');
  grid.style.gridTemplateColumns = 'repeat(' + page.cols + ', minmax(0, 1fr))';
  for (let i = 0; i < slots; ++i) {
    const tile = page.tiles.find(t => t.slot === i);
    const x = tile ? tileExec(tile) : null;
    const cell = el('div', 'wing-cell' + (x ? '' : ' empty') +
                            (x && x.active ? ' live' : ''));
    cell.appendChild(el('span', 'w-num', String(i + 1)));

    if (!x) {
      grid.appendChild(cell);
      continue;
    }

    cell.dataset.hue = tileHue(x);
    wireTile(cell, x);
    cell.appendChild(el('div', 'w-name', tile.legend || x.label));
    cell.appendChild(el('div', 'w-sub',
      x.active ? ('CUE ' + x.cue + '/' + x.cues)
               : (x.cues + ' CUE' + (x.cues === 1 ? '' : 'S'))));
    if (x.chase) cell.appendChild(el('div', 'w-rate', fmtRate(x)));
    cell.appendChild(tileFader(x, false));
    grid.appendChild(cell);
  }
  body.appendChild(grid);

  const row = el('div', 'wing-go');
  for (const [label, cmd] of [['GO', 'Go'], ['PAUSE', 'Off'], ['BACK', 'Back'],
                              ['RELEASE', 'Release']]) {
    const b = el('button', '', label);
    b.onclick = () => send(cmd);
    row.appendChild(b);
  }
  body.appendChild(row);
}

/* --------------------------------------------------------- the canvas */
/*
 * A tile renders at a detail level that follows the rectangle it was given
 * (§9.2's density table). The cell size is fixed in pixels rather than
 * fractional, because a tile placed at 3×2 has to be the same size on every
 * screen or the layout stops travelling with the show.
 */
function buildCanvas(body, page) {
  const bar = el('div', 'wing-pages');
  // §9.4: edit mode shows the grid, run mode hides it. Visible at a glance,
  // never a hidden state.
  const edit = el('button', S.editMode ? 'on' : '', S.editMode ? 'EDIT' : 'RUN');
  edit.onclick = () => { S.editMode = !S.editMode; render(); };
  bar.appendChild(edit);
  bar.appendChild(el('span', 'wing-hint',
    'free placement, snapping grid — size sets detail'));
  body.appendChild(bar);

  const canvas = el('div', 'canvas' + (S.editMode ? ' editing' : ''));
  canvas.style.gridTemplateColumns = 'repeat(' + page.cols + ', 104px)';

  for (const tile of page.tiles) {
    const x = tileExec(tile);
    if (!x) continue;
    const t = el('div', 'c-tile' + (x.active ? ' live' : ''));
    t.dataset.hue = tileHue(x);
    t.style.gridColumn = (tile.x + 1) + ' / span ' + tile.w;
    t.style.gridRow = (tile.y + 1) + ' / span ' + tile.h;
    wireTile(t, x);

    // §9.2's density table, implemented rather than described.
    const area = tile.w * tile.h;
    const head = el('div', 'c-head');
    head.appendChild(el('span', 'c-sym', x.chase ? '⟳' : '▶'));
    if (tile.w >= 2) head.appendChild(el('span', 'c-name', tile.legend || x.label));
    t.appendChild(head);

    if (area >= 4) {
      t.appendChild(el('div', 'c-state',
        x.active ? ('Cue ' + x.cue + ' of ' + x.cues) : (x.cues + ' cues · idle')));
    }
    if (area >= 6) {
      const prog = el('div', 'c-prog');
      const fill = el('i');
      fill.style.width = Math.round((x.active ? x.fade : 0) * 100) + '%';
      prog.appendChild(fill);
      t.appendChild(prog);
      t.appendChild(el('div', 'c-next',
        x.chase ? fmtRate(x)
                : ('next ' + (x.nextCue === null ? '—' : x.nextCue) +
                   ' · ' + (x.nextFade || 0).toFixed(1) + 's')));
    }
    t.appendChild(tileFader(x, tile.h > 1));
    canvas.appendChild(t);
  }
  body.appendChild(canvas);
}

/* ------------------------------------------------------------- pages */
function drawPage(body, page) {
  if (page.style === 'canvas') buildCanvas(body, page);
  else buildWing(body, page);
  for (const problem of page.problems)
    body.appendChild(el('div', 'clash', problem));
}

const STYLE_NOTE = {
  wing: 'fixed matrix · position is permanent',
  canvas: 'free placement · size sets detail',
};

function buildExecutorSurface(body) {
  const st = S.state;
  const pages = st.surfaces || [];
  if (!pages.length) {
    body.appendChild(el('div', 'empty', 'No surface pages. Try: Surface'));
    return;
  }

  // One button per page. Keeping both styles means the show OWNS pages of
  // each (§9.6), so this selects a page rather than setting a preference.
  const pick = el('div', 'style-pick');
  for (const p of pages) {
    const b = el('button', (!S.both && p.id === st.surface) ? 'on' : '',
                 p.name + ' · ' + p.style);
    b.title = STYLE_NOTE[p.style] || '';
    b.onclick = () => { S.both = false; send('Surface ' + p.id); };
    pick.appendChild(b);
  }
  const both = el('button', S.both ? 'on' : '', 'Side by side');
  both.title = 'every page at once, each in its own style';
  both.onclick = () => { S.both = !S.both; render(); };
  pick.appendChild(both);

  // Converting is a command, and the lossy direction says what it costs
  // before it costs it.
  const current = pages.find(p => p.id === st.surface);
  if (current && !S.both) {
    const other = current.style === 'wing' ? 'canvas' : 'wing';
    const conv = el('button', '', 'Make it a ' + other);
    conv.title = other === 'wing'
      ? 'lossy: tiles lose their size, and some may have nowhere to go'
      : 'lossless: every slot is already a 1×1 cell';
    conv.onclick = () => send('SurfaceStyle ' + other + ' ' + current.id);
    conv.style.marginLeft = 'auto';
    pick.appendChild(conv);
  }
  body.appendChild(pick);

  if (S.both) {
    const split = el('div', 'split');
    for (const p of pages) {
      const half = el('div', 'half');
      half.appendChild(el('div', 'half-label', p.name + ' — ' + p.style));
      drawPage(half, p);
      split.appendChild(half);
    }
    body.appendChild(split);
    return;
  }

  drawPage(body, current || pages[0]);
}

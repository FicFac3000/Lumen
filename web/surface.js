/* The executor surface, in both reference styles.
 *
 * §9 resolves the MA3/ChamSys question on paper — "flexibility in the canvas,
 * immediacy in the tile" — but that is a decision made in prose. This file
 * renders both styles from the SAME executor state so the decision can be
 * looked at instead of argued about.
 *
 * What differs between them is layout DISCIPLINE, not data and not features:
 *
 *   ChamSys   a fixed matrix. Every tile identical, numbered, never moves.
 *             Pages of ten wings. You learn where things are by position and
 *             they are there forever. Rigid, and fast because of it.
 *
 *   MA3       free placement on a snapping grid. Tiles take the size they
 *             deserve, and render more detail as they get bigger (§9.2).
 *             Pages are whole layouts. Flexible, and slower to build.
 *
 * Both share one rule from §9.1: NO OVERLAP. On a busking surface overlapping
 * targets mean mis-presses, so the MA3 canvas enforces the one piece of
 * ChamSys rigidity worth keeping.
 */

/* --------------------------------------------------------------- shared */

/* §9.2: legend and colour are a VIEW of the assigned object, never typed.
 * Colour comes from what the sequence drives, so the surface is readable
 * before anyone has configured anything. */
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

/* The fader under a tile. Commands only, like everything else. */
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

/* ----------------------------------------------------- ChamSys: the wing */
/*
 * Ten columns, fixed. Every tile the same size whether it holds a one-cue
 * blinder or a forty-cue stack. Empty slots are DRAWN, not hidden, because on
 * a real wing the empty one is still a button under your thumb and muscle
 * memory needs the gaps to be where they will always be.
 */
const WING_COLS = 10;
const WING_ROWS = 2;

function buildWing(body) {
  const st = S.state;
  const page = S.wingPage || 1;
  const perPage = WING_COLS * WING_ROWS;
  const first = (page - 1) * perPage + 1;

  const pages = el('div', 'wing-pages');
  for (let p = 1; p <= 4; ++p) {
    const b = el('button', p === page ? 'on' : '', 'PAGE ' + p);
    b.onclick = () => { S.wingPage = p; render(); };
    pages.appendChild(b);
  }
  const hint = el('span', 'wing-hint', 'fixed 10 × 2 — a slot never moves');
  pages.appendChild(hint);
  body.appendChild(pages);

  const grid = el('div', 'wing');
  for (let i = 0; i < perPage; ++i) {
    const number = first + i;
    const x = st.executors.find(e => e.id === number);
    const cell = el('div', 'wing-cell' + (x ? '' : ' empty') +
                            (x && x.active ? ' live' : ''));
    cell.appendChild(el('span', 'w-num', String(number)));

    if (!x) {
      // An unassigned slot still reads as a slot. ChamSys's whole bargain is
      // that position is permanent, which only works if nothing reflows.
      grid.appendChild(cell);
      continue;
    }

    cell.dataset.hue = tileHue(x);
    wireTile(cell, x);
    cell.appendChild(el('div', 'w-name', x.label));
    cell.appendChild(el('div', 'w-sub',
      x.active ? ('CUE ' + x.cue + '/' + x.cues) : (x.cues + ' CUE' + (x.cues === 1 ? '' : 'S'))));
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

/* --------------------------------------------------- MA3: the free canvas */
/*
 * Tiles take a grid rectangle and render at a detail level that depends on
 * the rectangle they were given (§9.2's density table). The layout below is
 * a default derived from the executors; in the real thing it is placed by
 * hand and travels with the show.
 *
 * The default deliberately makes the tiles DIFFERENT sizes, because that is
 * the whole claim of this style: a blinder is one big button, a cue stack
 * wants room for its state, and a low-state fader needs almost nothing.
 */
const CANVAS_COLS = 12;

function defaultPlacement(executors) {
  // Size follows what the tile has to say: more cues and live state earn
  // more room. This is the rule a user would otherwise apply by hand.
  const out = [];
  let col = 0, row = 0, rowH = 0;
  for (const x of executors) {
    let w = 2, h = 1;
    if (x.cues >= 3) { w = 3; h = 2; }         // a stack: needs its state
    else if (x.cues === 1 && /blind/i.test(x.label)) { w = 3; h = 2; }
    else if (x.cues === 1) { w = 2; h = 1; }
    if (col + w > CANVAS_COLS) { col = 0; row += rowH; rowH = 0; }
    out.push({ x, col, row, w, h });
    col += w;
    rowH = Math.max(rowH, h);
  }
  return out;
}

/* §9.1: overlap is forbidden. The check is cheap and the failure it prevents
 * — two targets under one thumb — is not recoverable mid-show. */
function overlaps(placed) {
  for (let i = 0; i < placed.length; ++i)
    for (let k = i + 1; k < placed.length; ++k) {
      const a = placed[i], b = placed[k];
      if (a.col < b.col + b.w && b.col < a.col + a.w &&
          a.row < b.row + b.h && b.row < a.row + a.h)
        return [a, b];
    }
  return null;
}

function buildCanvas(body) {
  const st = S.state;

  const bar = el('div', 'wing-pages');
  for (const name of ['Show', 'Prep', 'Soundcheck']) {
    const b = el('button', name === (S.layout || 'Show') ? 'on' : '', name.toUpperCase());
    b.onclick = () => { S.layout = name; render(); };
    bar.appendChild(b);
  }
  const edit = el('button', S.editMode ? 'on' : '', S.editMode ? 'EDIT' : 'RUN');
  edit.onclick = () => { S.editMode = !S.editMode; render(); };
  bar.appendChild(edit);
  bar.appendChild(el('span', 'wing-hint',
    'free placement, snapping grid — size sets detail'));
  body.appendChild(bar);

  const placed = defaultPlacement(st.executors);
  const clash = overlaps(placed);

  // The cell size lives in CSS, not here: a layout placed at 3x2 has to be
  // the same size on every screen or it stops travelling with the show.
  const canvas = el('div', 'canvas' + (S.editMode ? ' editing' : ''));

  for (const p of placed) {
    const x = p.x;
    const t = el('div', 'c-tile' + (x.active ? ' live' : ''));
    t.dataset.hue = tileHue(x);
    t.style.gridColumn = (p.col + 1) + ' / span ' + p.w;
    t.style.gridRow = (p.row + 1) + ' / span ' + p.h;
    wireTile(t, x);

    // §9.2's density table, implemented rather than described.
    const area = p.w * p.h;
    const head = el('div', 'c-head');
    head.appendChild(el('span', 'c-sym', x.chase ? '⟳' : '▶'));
    if (p.w >= 2) head.appendChild(el('span', 'c-name', x.label));
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
    t.appendChild(tileFader(x, p.h > 1));
    canvas.appendChild(t);
  }
  body.appendChild(canvas);

  if (clash) {
    body.appendChild(el('div', 'clash',
      'Overlap refused: ' + clash[0].x.label + ' and ' + clash[1].x.label +
      ' (§9.1 — overlapping targets mean mis-presses)'));
  }
}

/* ------------------------------------------------------------- the switch */
function buildExecutorSurface(body) {
  const pick = el('div', 'style-pick');
  for (const [key, label, note] of [
    ['chamsys', 'ChamSys wing', 'fixed matrix · position is permanent'],
    ['ma3', 'MA3 canvas', 'free placement · size sets detail'],
    ['both', 'Side by side', 'same executors, both disciplines'],
  ]) {
    const b = el('button', S.surface === key ? 'on' : '', label);
    b.title = note;
    b.onclick = () => { S.surface = key; render(); };
    pick.appendChild(b);
  }
  body.appendChild(pick);

  if (S.surface === 'both') {
    const split = el('div', 'split');
    const left = el('div', 'half');
    left.appendChild(el('div', 'half-label', 'ChamSys — fixed wing'));
    buildWing(left);
    const right = el('div', 'half');
    right.appendChild(el('div', 'half-label', 'MA3 — free canvas'));
    buildCanvas(right);
    split.append(left, right);
    body.appendChild(split);
    return;
  }
  if (S.surface === 'ma3') buildCanvas(body);
  else buildWing(body);
}

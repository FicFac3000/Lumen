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

/* §9.2: a tile's legend, colour and action are a VIEW of the assigned object,
 * resolved by the ENGINE. Nothing here derives them — a second renderer that
 * re-derived the legend would eventually derive it differently, and the whole
 * point of storing a UID is that there is one answer. */

/* The fader strip. Only tiles that actually have a level get one: a group
 * tile with a fader would imply selection were a level. */
function tileFader(tile, tall) {
  if (tile.level < 0 || !tile.levelCommand) return null;
  const bar = el('div', 'x-fader' + (tall ? ' tall' : ''));
  const fill = el('i');
  fill.style.height = Math.round(tile.level * 100) + '%';
  bar.appendChild(fill);
  bar.onclick = ev => {
    ev.stopPropagation();
    const r = bar.getBoundingClientRect();
    const v = Math.round((1 - (ev.clientY - r.top) / r.height) * 100);
    send(tile.levelCommand.replace('%d', String(Math.max(0, Math.min(100, v)))));
  };
  bar.title = Math.round(tile.level * 100) + '% — click to set';
  return bar;
}

function wireTile(node, tile) {
  if (tile.missing) {
    node.title = 'the object this tile pointed at is gone';
    return;
  }
  if (tile.unsupported) {
    node.title = 'nothing drives this kind of tile yet';
    return;
  }
  if (tile.command) {
    node.onclick = () => send(tile.command);
    node.title = tile.command + (tile.altCommand ? ' · right-click: ' + tile.altCommand : '');
  }
  if (tile.altCommand)
    node.oncontextmenu = ev => { ev.preventDefault(); send(tile.altCommand); };
}

/* A colour preset shows the colour it actually produces (§9.2). A palette of
 * swatches that all render identically is one you have to read instead of
 * see, which defeats having a palette. */
function paintTile(node, tile) {
  node.dataset.hue = tile.hue || 'default';
  if (tile.colour) node.style.setProperty('--hue', tile.colour);
  if (tile.missing) node.classList.add('broken');
  if (tile.unsupported) node.classList.add('inert');
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
    const cell = el('div', 'wing-cell' + (tile ? '' : ' empty') +
                            (tile && tile.live ? ' live' : ''));
    cell.appendChild(el('span', 'w-num', String(i + 1)));

    if (!tile) {
      grid.appendChild(cell);
      continue;
    }

    paintTile(cell, tile);
    wireTile(cell, tile);
    cell.appendChild(el('div', 'w-name', tile.legend));
    if (tile.detail) cell.appendChild(el('div', 'w-sub', tile.detail.toUpperCase()));
    const fader = tileFader(tile, false);
    if (fader) cell.appendChild(fader);
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
/* The cell size the CSS grid uses. Drag arithmetic has to agree with it, so
 * it is named once here rather than guessed in three places. */
const CELL_W = 104, CELL_H = 46, CELL_GAP = 4;

/* §9.1: the engine refuses an overlapping placement, but refusing on DROP is
 * too late — the operator has already committed the gesture. So the drag
 * shows, live, whether the position it is over would be accepted, using the
 * same rule the engine applies. */
function wouldFit(page, tile, want) {
  if (want.w < 1 || want.h < 1) return false;
  if (want.x < 0 || want.y < 0 || want.x + want.w > page.cols) return false;
  for (const o of page.tiles) {
    if (o === tile) continue;
    if (want.x < o.x + o.w && o.x < want.x + want.w &&
        want.y < o.y + o.h && o.y < want.y + want.h) return false;
  }
  return true;
}

/* Dragging a tile, or one of its edges. The DOM is rebuilt from the snapshot
 * twice a second, so the drag works on a floating ghost and commits once —
 * trying to move the real element would fight the redraw. */
function beginDrag(ev, canvas, page, tile, mode, onTap) {
  ev.preventDefault();
  ev.stopPropagation();
  const shift = ev.shiftKey;
  const startX = ev.clientX, startY = ev.clientY;
  const from = { x: tile.x, y: tile.y, w: tile.w, h: tile.h };
  let want = { ...from };

  const ghost = el('div', 'ghost');
  const place = r => {
    ghost.style.left = (r.x * (CELL_W + CELL_GAP)) + 'px';
    ghost.style.top = (r.y * (CELL_H + CELL_GAP)) + 'px';
    ghost.style.width = (r.w * CELL_W + (r.w - 1) * CELL_GAP) + 'px';
    ghost.style.height = (r.h * CELL_H + (r.h - 1) * CELL_GAP) + 'px';
  };
  place(from);
  canvas.appendChild(ghost);
  S.dragging = true;

  const onMove = m => {
    // Snap by whole cells: the grid IS the unit, so a half-cell preview
    // would promise a position the model cannot hold.
    const dx = Math.round((m.clientX - startX) / (CELL_W + CELL_GAP));
    const dy = Math.round((m.clientY - startY) / (CELL_H + CELL_GAP));
    want = mode === 'move'
      ? { x: from.x + dx, y: from.y + dy, w: from.w, h: from.h }
      : { x: from.x, y: from.y, w: from.w + dx, h: from.h + dy };
    want.w = Math.max(1, want.w);
    want.h = Math.max(1, want.h);
    want.x = Math.max(0, want.x);
    want.y = Math.max(0, want.y);
    place(want);
    ghost.classList.toggle('bad', !wouldFit(page, tile, want));
  };

  const onUp = () => {
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    ghost.remove();
    S.dragging = false;
    const moved = want.x !== from.x || want.y !== from.y ||
                  want.w !== from.w || want.h !== from.h;
    // A press that went nowhere is a tap. Deciding that here rather than in
    // a separate click handler avoids depending on whether the browser still
    // synthesises a click after a prevented pointerdown — it does not,
    // reliably, and selection silently stopped working when it did not.
    if (!moved) {
      if (onTap) onTap(shift);
      else render();
      return;
    }
    if (!wouldFit(page, tile, want)) {
      toast('Not there — it would overlap another tile (§9.1)', true);
      render();
      return;
    }
    // The engine is still the one that decides; this just avoids asking it
    // a question the answer to which is visibly no.
    send('TileMove ' + (tile.slot + 1) + ' ' + (want.x + 1) + ' ' + (want.y + 1) +
         ' ' + want.w + ' ' + want.h);
  };

  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', onUp);
}

function buildCanvas(body, page) {
  const bar = el('div', 'wing-pages');
  // §9.4: edit mode shows the grid and the handles, run mode hides both.
  // Visible at a glance, never a hidden state — and enforced by the engine,
  // so this button is a view of the page rather than a local flag.
  const edit = el('button', page.editMode ? 'on' : '',
                  page.editMode ? 'EDIT' : 'RUN');
  edit.title = page.editMode
    ? 'tiles can be moved and resized'
    : 'tiles can only be operated';
  edit.onclick = () => send('EditMode ' + (page.editMode ? 'off' : 'on') + ' ' + page.id);
  bar.appendChild(edit);

  if (page.editMode) {
    // §9.1's alignment helpers. They close most of the speed gap with a
    // fixed grid, which is the whole reason a free canvas is bearable.
    const sel = page.tiles.filter(t => t.selected).length;
    for (const [label, how, hint] of [
      ['Left', 'left', 'align left edges'],
      ['Top', 'top', 'align top edges'],
      ['Match W', 'matchwidth', 'take the width of the first selected'],
      ['Spread', 'spreadh', 'even the gaps'],
      ['Fill row', 'fillrow', 'share the row equally'],
    ]) {
      const b = el('button', '', label);
      b.title = hint + (sel < 2 ? ' — select two or more tiles first' : '');
      b.disabled = sel < 2;
      b.onclick = () => send('Align ' + how);
      bar.appendChild(b);
    }
    const clear = el('button', '', sel ? sel + ' selected' : 'none selected');
    clear.title = 'clear the tile selection';
    clear.onclick = () => send('TileSelect');
    bar.appendChild(clear);

    // Arranging is where mistakes happen, so undo belongs here rather than
    // only in the programmer window three panes away (§8.6).
    const undo = el('button', '', '↶ Undo');
    undo.title = S.state.canUndo ? 'undo the last edit' : 'nothing to undo';
    undo.disabled = !S.state.canUndo;
    undo.onclick = () => send('Undo');
    bar.appendChild(undo);
  }

  bar.appendChild(el('span', 'wing-hint', page.editMode
    ? 'drag to move · drag the corner to resize · click to select'
    : 'free placement, snapping grid — size sets detail'));
  body.appendChild(bar);

  const canvas = el('div', 'canvas' + (page.editMode ? ' editing' : ''));
  canvas.style.gridTemplateColumns = 'repeat(' + page.cols + ', ' + CELL_W + 'px)';

  for (const tile of page.tiles) {
    const t = el('div', 'c-tile' + (tile.live ? ' live' : '') +
                        (tile.selected ? ' picked' : ''));
    paintTile(t, tile);
    t.style.gridColumn = (tile.x + 1) + ' / span ' + tile.w;
    t.style.gridRow = (tile.y + 1) + ' / span ' + tile.h;

    if (page.editMode) {
      // In edit mode a tile is a thing to arrange, not a button to press:
      // pressing Go while rearranging the surface is exactly the mis-press
      // §9.4 separates the modes to prevent.
      const tap = shift => {
        // Selection ORDER is information — match-width reads the first — so
        // shift-adding appends rather than rebuilding from screen order.
        const already = page.tiles.filter(o => o.selected).map(o => o.slot + 1);
        const me = tile.slot + 1;
        if (!shift) { send('TileSelect ' + me); return; }
        const next = already.includes(me)
          ? already.filter(n => n !== me)
          : already.concat([me]);
        send(next.length ? 'TileSelect ' + next.join(' ') : 'TileSelect');
      };
      t.onpointerdown = ev => {
        if (ev.button !== 0) return;
        beginDrag(ev, canvas, page, tile, 'move', tap);
      };
      const grip = el('div', 'grip');
      grip.title = 'drag to resize';
      grip.onpointerdown = ev => beginDrag(ev, canvas, page, tile, 'resize');
      t.appendChild(grip);
    } else {
      wireTile(t, tile);
    }

    // §9.2's density table, implemented rather than described.
    const area = tile.w * tile.h;
    const head = el('div', 'c-head');
    if (tile.symbol) head.appendChild(el('span', 'c-sym', tile.symbol));
    if (tile.w >= 2) head.appendChild(el('span', 'c-name', tile.legend));
    t.appendChild(head);

    if (area >= 4 && tile.detail) t.appendChild(el('div', 'c-state', tile.detail));
    if (area >= 6) {
      // Only an executor has a cue to be partway through; for everything
      // else the extra room goes to the legend rather than to a bar that
      // would always read zero.
      const x = S.state.executors.find(e => tile.command === 'Go ' + e.id);
      if (x) {
        const prog = el('div', 'c-prog');
        const fill = el('i');
        fill.style.width = Math.round((x.active ? x.fade : 0) * 100) + '%';
        prog.appendChild(fill);
        t.appendChild(prog);
        t.appendChild(el('div', 'c-next', x.chase
          ? x.rate.toFixed(1).replace(/\.0$/, '') + ' Hz'
          : 'next ' + (x.nextCue === null ? '—' : x.nextCue) + ' · ' +
            (x.nextFade || 0).toFixed(1) + 's'));
      }
    }
    const fader = tileFader(tile, tile.h > 1);
    if (fader) t.appendChild(fader);
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

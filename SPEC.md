# Lumen — Feature Specification

Working spec. Captured from the user's feature dump; grows as we go.
Status markers: `[want]` requested, `[open]` needs a decision, `[flag]` legal or
technical blocker, `[done]` implemented in the engine.

---

## 1. Fixture library and fixture editor

### 1.1 Library content

- `[want]` Bundle the full GDTF fixture library.
- `[want]` Bundle the full ChamSys fixture library.
- `[flag]` **ChamSys library cannot be redistributed.** ChamSys personality
  files (`.hed`) are ChamSys's own data, shipped under their licence. Bundling
  them inside a different product is not something we can do legally.
  Workable alternatives, in order of preference:
  1. **Import on the user's machine.** If the user has MagicQ installed, point
     the importer at their existing `heads/` directory and convert to GDTF on
     first run. The data stays theirs; we ship a parser, not a library.
  2. **Convert on demand.** Drag-and-drop `.hed` import, one fixture at a time.
  3. **Build our own library** over time, published as GDTF.
  A `.hed` parser is cheap to write — the format is a plain-text channel list.
  The value the user actually wants (every head already profiled) is preserved
  by route 1 without a redistribution problem.
- `[open]` GDTF Share content has its own terms of use. Decide between bundling
  a snapshot vs. fetching from GDTF Share with the user's own account.
- `[want]` Automatic library updates / sync.

### 1.2 One model, two views

GDTF stores a fixture as a **geometry tree** with geometry references. That is
the same structure MA3's tree view exposes for multi-instance fixtures. So:

- **Internal model:** GDTF (geometry tree, DMX modes, channel functions,
  attribute set, wheels, physical descriptions, revisions).
- **Table view (ChamSys style):** flattened DMX-mode channel list. Fast to
  scan, fast to edit, one row per DMX channel. This is the default editing
  surface for simple fixtures.
- **Tree view (MA3 style):** geometry hierarchy with instances. The editing
  surface for multi-instance fixtures — Color Strike M, Pixel Curve 12,
  Acme Tornado, LED bars, pixel tiles, anything with sub-fixtures.
- Both views are live on the same data. An edit in the table appears in the
  tree and vice versa. No import/export step between them.

### 1.3 Multi-instance handling

- `[want]` Geometry references so one "cell" definition instantiates N times
  with automatic DMX offset.
- `[want]` Sub-fixture addressing in patch and selection (e.g. fixture 12.3).
- `[want]` Master/cell relationship: main dimmer and shutter at the parent,
  colour per cell.
- `[open]` Selection syntax for cells — MA3 uses `12.1 Thru 12.12`.

#### 1.3.1 Decision: a channel offset is relative to its own instance

`[done]` The authoring model numbers a channel from the start of the geometry
that declares it, not from the start of the mode. On a plain geometry the two
are the same, because its instance offset is zero; on an instanced one, a
pixel's red is offset 0 and each GeometryReference's own offset places the
whole cell.

The alternative — offsets relative to the mode, as the GDTF *file* stores them
— makes the first cell's numbering special and every other cell a correction
to it. Twelve pixels then need twelve declarations, which is precisely the
editing problem §1.2's tree view exists to remove.

Consequences, all of them wanted:

- Growing a cell definition **re-spaces its instances automatically**. Adding
  white to a pixel bar moves cells 2–12 down by one each; leaving them
  overlapping is never what the user meant.
- Channels on *other* geometries are left where they are. Moving a fixture's
  master channels because a cell grew would be a surprise, so the compile
  reports the collision instead and the user decides.
- Packing works per address space: within each cell, and across the mode only
  when the mode has no cells at all. With cells present, whether the master
  block sits before or after them is a layout decision, not a mechanical one.

#### 1.3.2 Imported profiles are normalised

`[done]` GDTF files in the wild declare a four-cell bar as twelve channels, one
per instance. On import, where every reference to a shared geometry declares
the same channels at the same offsets within its own instance, those
declarations collapse into one against the shared geometry. The compiled result
is identical — same channels, same addresses — only the editing surface
changes.

A group whose instances differ is left exactly as it is, and the importer says
which instance stopped it. An LED bar whose last cell lacks a channel is a real
fixture, and averaging it into a uniform cell would hand the user a wrong
patch.

### 1.4 GDTF builder

#### 1.4.1 Decision: GDTF is the fixture file format, and an import keeps its bytes

`[done]` Fixture files stay GDTF, in and out. There is no private Lumen fixture
format, and an imported `.gdtf` package is **kept verbatim** rather than
replaced by anything Lumen regenerates.

The reason is coverage. Lumen's model holds geometries, modes, channels and
channel sets. A real package also carries 3D models, wheels and slots,
emitters, filters, colour spaces, physical descriptions, DMX profiles and
revision history. Regenerating a package from the model would silently discard
all of it — so the original travels with the show and an export returns it
unchanged, byte for byte.

- A profile is only written from the model when it was **authored in Lumen**,
  or **edited** here. Then the export says what regenerating cost, naming each
  category it could not write, rather than shipping a thinner profile quietly.
- The package is **embedded in the show file**, unlike media (§17.7). A profile
  is tens of kilobytes, and a show that opens with its fixtures missing because
  a library folder moved is not a show.
- MVR export embeds these kept packages, so a plot that leaves Lumen carries the
  designer's own profiles rather than Lumen's reading of them.
- The parsed model and the package are both stored, because they are allowed to
  diverge: that divergence is exactly what "edited" means.

- `[want]` Built-in GDTF fixture builder — create a fixture from scratch,
  not just edit imported ones.
- Scope: geometry tree authoring, DMX mode authoring, channel functions and
  sets, attribute assignment from the GDTF attribute list, wheel/slot
  definitions, physical data, revision history, validation against the schema,
  export as a valid `.gdtf` package.
- `[open]` 3D model handling — GDTF packages carry 3DS/glTF geometry. Decide
  whether the builder authors models or only references supplied ones.

---

## 2. Window system and UI

### 2.1 Simulated hard buttons (ChamSys style)

- `[want]` On-screen hard-button bank, always visible, mirroring the physical
  MagicQ console layout.
- `[want]` One button per main window.
- `[want]` Press behaviour: **restore the last configuration of that window and
  bring it to front** — not "open a fresh default window". Each window type
  remembers its own last state (size, position, view mode, filters, scroll,
  selected tab).
- `[open]` Second press behaviour — cycle instances, or close/hide?
- `[open]` Modifier presses (shift/ctrl) for alternate window variants.

### 2.2 Window arrangement (MA3 style)

- `[want]` Snap window size and position to a placement grid.
- `[want]` Adjustable grid granularity, user-controlled at runtime.
- `[open]` Per-screen grid settings vs. one global grid.
- `[open]` Saved view/layout presets recallable from the hard-button bank.
- `[open]` Multi-monitor: independent grids, windows draggable between screens.

### 2.3 Implied requirements

- Window state persistence lives in the show file, so layouts travel with
  the show.
- Needs a window manager layer of our own — not OS windows — so the grid,
  snapping and the hard-button restore logic all behave consistently.

---

### 2.4 Window header convention

Every window and sub-page carries the same header cluster, so per-window
configuration is always in the same place rather than scattered between
right-click menus and a global preferences screen:

```
[ symbol ] Title · context                    [ ⚙ ] [ ⤢ ] [ × ]
```

- **⚙ Settings** — that window's *own* behaviour settings. What appears is
  window-type specific: a preset pool shows sort and filter-display mode
  (§7.4, §7.5); a sub-page shows replace/overlay and auto-return (§9.1); an
  executor surface shows edit/run lock (§9.4).
- **⤢ Snap/size** — snap to grid region, maximise, send to other screen.
- **× Close**.

- `[want]` Whatever the gear exposes is exactly what persists as that window's
  remembered state (§2.1), so "the last configuration of that window" has a
  single, visible definition rather than being implicit.
- `[want]` The gear is reachable in run mode. Locking a surface (§9.4) prevents
  moving tiles, not configuring how the window behaves.

### 2.5 Panel sets

A **panel set** is a named configuration of the screen chrome: which bands
exist, where they sit, how large they are. MagicQ's differing button/encoder
treatments, made explicit and switchable.

Shipped sets:

| Set | Chrome |
|-----|--------|
| **Full** | every band on screen — window buttons, pool buttons, soft buttons, encoder bar |
| **Wing** | on-screen button bands removed; the space is **reclaimed by windows**, not left empty |
| **Programming** | pool buttons + encoder bar prominent, executor surface minimal |
| **Show** | executor surface maximal, programming chrome hidden |
| **Touch** | larger targets throughout, radial-first popups (§10.4) |

- `[want]` User-definable sets beyond these, stored in the show.
- `[want]` Switchable at runtime, bindable to a hard button, expressible as a
  command string (`Panel Set "Wing"`).
- `[want]` **Auto-switch on hardware detect** — connecting a wing selects the
  `Wing` set, disconnecting restores the previous one, with a manual override
  that sticks. The user asked for selectable; auto with override is selectable
  plus one less thing to remember.
- `[want]` Per-screen: a two-screen rig can run `Programming` on one and `Show`
  on the other.

### 2.6 Bands

| Band | Default edge | Holds |
|------|--------------|-------|
| **Window bar** | top | one hard button per window type |
| **Pool bar** | top / left | one hard button per preset pool |
| **Soft buttons** | right | context-sensitive, change with the focused window |
| **Encoder bar** | **bottom** | encoder strip, MA3-style |
| **Command line** | bottom, above encoders | input + feedback |
| **Masters** | right / bottom | grand master, rate, blackout |

Each band: show/hide, edge, size, and whether it repeats per screen.

**Pool bar** — `[want]` a button per pool, including the five main groups
(Intensity, Position, Color, Beam, Control) and every user pool. Pools are
user-extensible, so `[open]` the bar needs a strategy past ~12: paginate,
scroll, or overflow into a `More` radial (§10.1).

**Encoder bar** `[done-decision]` — bottom edge, MA3-style. Shows the focused
attribute group's sub-attributes mapped across the encoders, each rendered with
§7.1's three-layer identity:

```
 ◐ Dimmer        ✛ Pan          ✛ Tilt         ↻ Prism 1 Rot
   72%             -14°           38°            CW · 14 rpm
```

- Value plus the active ChannelSet's range label (§7.2) where one applies —
  so an encoder on a gobo wheel reads the slot name, not a raw number.
- `[want]` Bank paging when a group has more sub-attributes than encoders, with
  the bank indicator always visible.
- Per §6.1.1, Intensity holds the first bank permanently and is never paged
  away.

### 2.6.1 Encoder behaviour

`[done-decision]` MA-style, with **three resolutions** switched by a setting:

| Resolution | Value change per full turn |
|------------|---------------------------|
| **Normal** | full attribute range |
| **Fine** | 1/10 of range |
| **Ultra fine** | 1/100 of range |

- `[want]` Resolution is global, switched from a hard button, and shown on each
  encoder so the current mode is never in doubt.
- `[flag]` On an **8-bit** attribute, ultra fine has nothing to address — 1/100
  of 256 steps is below resolution. The encoder should indicate that fine or
  ultra has no further effect rather than appearing to work. 16-bit attributes
  (§1's `fine` channel) are where the lower resolutions earn their place.

**Four buttons around each encoder:**

```
      ┌─────────────┐
 ◀ ⤒  │   ◐ Dimmer  │  ⤓ ▶     ⤒ ⤓ = previous / next channel range
      │     72%     │           ◀ ▶ = nudge down / up
 ◀ −  └─────────────┘  + ▶
```

| Position | Action |
|----------|--------|
| **Top left** | jump to the next **lower** channel range |
| **Top right** | jump to the next **upper** channel range |
| **Bottom left** | nudge value **down** by one unit |
| **Bottom right** | nudge value **up** by one unit |

**Range jumping** steps between ChannelSets (§7.2) — the next gobo slot, the
step from `stop` to `CW slow` on a prism rotate. `[want]` Top buttons grey out
when no further range exists, as the user specified.

- `[open]` Where inside the target range does it land? Centre is right for
  `indexed` kinds (a gobo slot), the near edge for `continuous` ones (the slow
  end of `CW`). Proposal: land per kind (§6.2), configurable.

**Nudging** `[done-decision]` steps by **one unit of the current display mode,
scaled by the encoder resolution**:

| Display mode | Normal | Fine | Ultra fine |
|--------------|--------|------|------------|
| Percent | 1 % | 0.1 % | 0.01 % |
| DMX 0–255 | 1 | 0.1 | 0.01 |
| Degrees | 1° | 0.1° | 0.01° |
| Hertz | 1 Hz | 0.1 Hz | 0.01 Hz |

The point of tying it to the display mode is that the nudge always matches what
is on screen — press up once, the number you are reading goes up by one.

- `[flag]` Sub-resolution nudges clamp to the attribute's real resolution, with
  the same indication as above. A 0.01 DMX nudge on an 8-bit channel does
  nothing and should say so.
- `[want]` Display mode defaults from the attribute's physical unit (degrees
  for an angle, Hz for a frequency, percent otherwise), with a per-attribute
  override and a range-label mode that shows the ChannelSet name instead of a
  number.
- **Time-valued attributes** follow §4.16's seconds / bars-and-beats modal
  switch rather than having their own display mode, so an encoder on a speed or
  a fade time reads in whatever unit the rest of the app is showing.
- `[want]` All four buttons are bindable (§12), so a hardware wing can map them
  to physical keys around its own encoders.

### 2.7 Opening windows and cursor focus

Extends §2.1's hard-button behaviour.

A window button press:
1. Opens the window if it is not already open.
2. Raises it and gives it focus if it is.
3. **Restores the cursor to where it was** when that window was last focused.
4. Falls back to the first cell (top-left) if the window has never been opened.

- `[want]` Cursor position is part of the window state persisted via the §2.4
  gear, so it travels with the show.
- `[want]` The cursor is a visible, high-contrast cell outline — legible in a
  dark venue at a glance, not a subtle highlight.
- `[open]` Does the cursor restore if the underlying content changed (the row
  it was on was deleted)? Leaning: clamp to the nearest valid cell.

**Directed focus.** `[want]` Beyond restoring a remembered position, a window
can be opened *at a specified target* — a given cue, fixture, attribute cell or
pool entry. This is what §11.10's Jump to source needs, and it is a general
primitive: any object reference (§8.1) can be the destination.

**`[flag]` Jumping needs a way back, or it is a one-way trip.** A navigation
**back stack**, separate from undo (§8.6) — undo reverts changes, this reverts
*location*:

- Jumping pushes the current window, cursor position and selection.
- A `Back` action returns; `Forward` re-follows.
- On arrival, a breadcrumb shows where the jump came from, so the return path
  is visible rather than remembered.
- Bound to a hard button and expressible as a command (`Goto Source`, `Back`),
  consistent with §5.5.

### 2.8 Keyboard and cursor navigation

`[done-decision]` The three tile bindings from §10.3 get keyboard equivalents,
so the whole surface is operable without touch:

| Input | Acts as |
|-------|---------|
| Arrow keys | move cursor focus |
| **Return** | tap — primary binding |
| **Return held** | long press — secondary binding |
| **Return twice** | double tap — tertiary binding |
| Escape | cancel / close popup |
| Page Up/Down | page a pool or bank |

- Hold duration uses the same setting as §10.2, so keyboard and touch feel
  identical.
- `[flag]` **The §10.3 conflict applies here too.** On a tile whose primary
  fires immediately (Go, Flash), a double-Return would fire it twice before the
  gesture resolves. Those tiles get **Shift+Return** as the tertiary instead,
  assigned by the same validation rule — not a separate mechanism.
- `[want]` Arrow navigation crosses between bands and windows, not only within
  one, so the cursor can reach everything. `[open]` Edge behaviour: wrap, stop,
  or jump to the adjacent window? Leaning jump, with wrap as a setting.
- `[want]` The command line keeps keyboard focus while the cursor moves, so
  typing a command never requires clicking back into it.

---

## 3. Engine (already built)

- `[done]` Attribute model: `(FixtureId, Attr, float 0..1)` as the single
  abstraction across DMX, video pixels and laser zones.
- `[done]` Patch with footprint collision checking, 8/16-bit channels.
- `[done]` Programmer with selection, manual values, store-to-cue.
- `[done]` Cues, sequences, executors with fades and crossfade snapshots.
- `[done]` Phaser FX engine with waveform set, phase spread across a group.
- `[done]` Merge pipeline: HTP on intensity, LTP elsewhere, programmer on top,
  grand master and blackout.
- `[done]` sACN (E1.31) multicast and Art-Net output, 44 Hz output thread.
- `[pending]` MA-style command line parser — paused for this spec.

**Rework needed before building further** (each flagged in place):

| Change | Driver |
|--------|--------|
| `Attr` enum → `{group, sub, kind}` | §6.3 |
| `mergeRuleFor()` reads the `intensity` kind, not `Attr::Dimmer` | §6.3 |
| Tracking resolution pass; `Executor::snapshot()` resolves tracked state | §11.1 |
| `ValueLayer` value type: `float` → `{kind, value, presetUid, presetAttr, active}` | §11.8 |
| Clear state machine; `storeCue()` filters by `active`, preserves references | §11.8 |
| Retained last-output value per attribute, for LTP hold-last on release | §11.9 |
| Traced merge: on-demand provenance for a fixture subset | §11.10 |
| Source mute set, consulted during merge | §11.10 |
| Query engine shared by sheet filters, dynamic groups, preset facets | §11.10 |
| `tick()` sorts by priority, reads per-executor merge rule, applies swap groups | §11.4 |
| `crossfade()` gains fade curves | §11.2 |
| sACN / Art-Net **input** with source priority | §12.7 |
| Input event queue drained at top of `tick()` | §12.7 |
| Mutex + `universes_` copy → triple-buffer handoff | §15.2 |
| Extraction merge layer | §13.3 |

Order matters: the taxonomy change (§6.3) touches every file, and the
concurrency change (§15.2) must land before the render or laser threads exist.

---

## 4. FX engine — ChamSys recipes on an MA3 phaser core

The two engines solve different halves of the same problem, so we keep both
and stack them rather than picking one.

- **ChamSys FX** is a *recipe*: one named thing ("Circle") that covers several
  attributes at once, applied to a selection, busked live with a handful of
  encoders — Speed, Size, Spread, Offset. Fast, shallow, immediate.
- **MA3 phasers** are a *primitive*: per-attribute, step-based, stored inside
  the cue value itself, trackable, with per-step Width / Transition / Accel /
  Decel and Grouping for how phase spreads over the selection. Deep, granular,
  less immediate.

### 4.1 Three-tier model

```
Recipe  ──instantiates──>  Slots  ──compile to──>  Phasers  ──write──>  Attributes
(named FX)                 (rebindable targets)    (step-based)         (any domain)
```

**Tier 1 — Phaser (primitive).** Per-attribute. A phaser evaluates a phase
`0..1` to a value, via one of two generators (§4.5). This is what lives in a
cue and what the render pipeline evaluates.

**Tier 2 — Slot.** A recipe member: `{target, phaser template, speed multiplier,
size multiplier, phase offset, group assignment}`. The `target` is a variable,
not baked in — this is the core request.

**Tier 3 — Recipe.** A named bundle of slots plus master Speed / Size / Spread /
Offset. Applying it to a selection instantiates its slots as phasers on those
fixtures. The masters re-scale every slot at once, which is the ChamSys busking
feel; drilling in exposes slot, then step.

### 4.2 Rebindable targets — the unlock

A slot's target can be bound three ways:

1. **Concrete attribute** — `Pan`, `ColorR`, `Zoom`.
2. **Role** — `AxisX`, `AxisY`, `Intensity`, `ColorComponent1`, `Size`. Roles
   resolve per-fixture at apply time against whatever the selected fixture
   actually has.
3. **Unbound** — prompts on apply.

So one "Circle" recipe (slot A → `AxisX` cosine, slot B → `AxisY` sine, 90°
apart) becomes:
- a pan/tilt circle on a moving head,
- an XY circle on a laser zone,
- a position orbit on a mapped video surface,

with no separate FX library per domain. Rebind the same two slots to `ColorR`
and `ColorB` and it is a colour chase. **This is why the single
`(FixtureId, Attr, float)` abstraction was worth building.**

- `[open]` Role vocabulary — needs a fixed list, mapped per fixture type in the
  GDTF profile.
- `[done-decision]` **Fallback when a fixture has no attribute for a role:
  skip the slot, not the fixture.** A Circle recipe applied to a mixed
  selection still runs its intensity slot on a plain dimmer; only the
  pan/tilt slots drop out for that fixture. Per-fixture, per-slot
  resolution — a slot is skipped only for the fixtures that can't serve it.

### 4.3 Live vs stored — resolving the real conflict

ChamSys FX are busked on a playback. MA3 phasers are baked into cue values.
Lumen stores **both**:

- On `Store`, the compiled phasers are written into the cue, so playback never
  depends on the recipe library being present or unchanged.
- The recipe reference and its master parameters are stored *alongside*, so the
  cue can be reopened in the recipe editor and re-busked later.

Portable like MA3, editable like ChamSys. A recipe edited in the library does
not retroactively change stored cues unless the user explicitly re-links.

### 4.4 Encoder depth

Three drill-down levels, same encoders throughout:

| Level  | Encoders control                                  |
|--------|---------------------------------------------------|
| Recipe | master Speed, Size, Spread, Offset                |
| Slot   | that slot's target, multipliers, phase, grouping  |
| Step   | value, width, transition, accel, decel            |

- `[want]` Recipe library, user-extensible, stored in the show and exportable.
- `[want]` Save any programmer FX state back out as a new named recipe.

### 4.5 Three generators, one interface

`[done-decision]` A phaser's generator is **either** analytic or step-based.
Both answer the same question — value at phase — so everything downstream is
unchanged.

**Correction to an earlier assumption:** waveforms do *not* bake into step
lists. A stepped approximation of a sine is coarser and no cheaper than
evaluating the sine, so keeping both as first-class generators is better:

| Generator | Holds | Good for |
|-----------|-------|----------|
| **Waveform** | `{wave, size, centre, duty}` — the existing enum | smooth, cheap, continuous motion |
| **Steps** | ordered `{value, width, attack, decay, transition}` | exact control, chases, hard cuts |
| **Curve** | bezier control points | arbitrary hand-drawn shapes |

Step parameters, per step:

- **value** — the target
- **width** — how long the step dwells, as a fraction of the cycle
- **transition** — how long the move to the next step takes
- **attack / decay** — rise and fall shaping, each with its own time and curve

`[done-decision]` **Attack and decay are separate, not a symmetric ease.** Fast
attack with slow decay is the difference between a flat strobe and one with a
tail, and symmetric easing cannot express it. Each has its own time and its own
editable curve.

A step list with width 1.0 and transition 0 is a hard chase; wide transitions
with shaped attack and decay produce arbitrary smooth motion.

- `[want]` Convert between generators — sample a waveform into N steps for
  hand-editing, or read a curve's envelope back as steps.

### 4.6 Absolute and relative

`[done-decision]` **Per slot**, not per recipe:

| Mode | Behaviour |
|------|-----------|
| **Relative** (default) | offsets around the underlying value — pan swings ±20° from wherever the fixture is already pointing |
| **Absolute** | drives explicit values, ignoring the base — pan goes to 0° and 90° regardless |

Relative is what makes a recipe portable: the same Circle works on fixtures
aimed in different directions. Absolute is what a colour chase through specific
colours needs. The engine's existing `useBase` / `center` fields are this
distinction; it just needs exposing per slot rather than per phaser.

- This is also what §11.10's **Relative** column reads — the FX contribution as
  a delta.

### 4.7 Stacking

`[done-decision]` Two phasers may target the same attribute. The current engine
replaces on a match (`Programmer::addPhaser`); it needs a mode:

| Mode | Effect | Use |
|------|--------|-----|
| **Replace** | last wins | current behaviour, still the default for a same-slot edit |
| **Add** | sum the contributions | a slow drift plus a fast flicker |
| **Multiply** | one scales the other | an envelope modulating another FX's size |

- Slots are ordered, so stacking order is defined rather than incidental.
- `[flag]` Summed contributions must clamp, and clamping asymmetrically
  distorts the waveform. `[want]` Show when a stack is clipping, rather than
  letting it quietly flatten.

**FX layers.** `[done-decision]` Stacking is organised into **layers**, not just
an ordered list — the same model as §13.4's composition stack, applied to FX:

- Any number of layers may target the same attribute.
- Each layer has a blend mode (`replace` · `add` · `multiply` · `max` · `min`),
  an opacity, and its own enable/solo.
- Layers are independently editable, so a slow drift and a fast flicker stay
  separate objects rather than merging into one unreadable phaser.
- `[want]` Per-layer mute and solo while editing — the FX equivalent of
  §11.10's source inspection, and the only practical way to work on one layer
  of a four-layer effect.

### 4.8 Timebase, speed masters and sync

`[flag]` **The timebase choice is not cosmetic.** The engine currently uses
`now()` for programmer phasers and `now - cueStart` for cue phasers — two
different behaviours, neither chosen deliberately. Both are wanted, so it
becomes a per-phaser setting:

| Clock | Behaviour | Use |
|-------|-----------|-----|
| **Global** | one continuous app-wide FX clock | a breathing effect that must not jump when a cue changes |
| **Playback** | resets with the cue | a chase that should start on the downbeat |
| **Triggered** | starts on an explicit trigger | synchronised hits |

- `[want]` **Speed masters** — named rate groups that any phaser subscribes to,
  so a dozen effects stay locked together and are adjusted with one control.
  Bindable (§12), and a master can take its rate from MIDI clock or a tap
  (§11.5).
- `[want]` An explicit **Sync / Restart** action to realign running phasers.
- `[flag]` **Adding a fixture to a running FX must not restart it.** Phase comes
  from the fixture's position in the spread (§20.2), not from when it joined,
  so a newly selected fixture drops straight into the correct phase. The
  current `idx/total` formula already behaves this way; the spread-table change
  must preserve it.

### 4.9 Fade and stop

- `[want]` **FX fade in and out** — size ramps from zero rather than snapping.
  Per recipe and per slot, and a cue's fade time scales FX size so an effect
  arrives with the cue instead of appearing instantly.
- `[flag]` **Stop behaviour matters most on position.** A pan FX stopped
  mid-swing leaves a rig visibly misaligned. Modes:

| Stop mode | Behaviour |
|-----------|-----------|
| **Stop in place** | freeze at the current value |
| **Return to base** | fade back to the underlying value |
| **Run to zero** | continue to the next zero crossing, then stop |

  `[want]` Default per attribute kind (§6.2): `Run to zero` for `axis`
  attributes, `Return to base` for colour and intensity. A sensible default
  here prevents the single most common visible FX artefact.

### 4.10 Colour FX

`[flag]` An FX applied independently to `Red`, `Green` and `Blue` produces
muddy, desaturated transitions — the colours cross through grey rather than
around the wheel. Correct behaviour needs hue space.

- §6.1 already defines `Hue` and `Saturation` as `virtual`-kind attributes, so
  a phaser on `Hue` is the answer and no new mechanism is needed.
- `[want]` Colour recipes target `Hue` / `Saturation` by default, with
  per-channel targeting available for the cases that genuinely want it.

### 4.11 Per-fixture variation

`[want]` Seeded random variation per fixture on phase, speed and size, so an
effect reads as organic rather than mechanical — the difference between a
mechanical flicker and a believable one.

- Seeded, so it is reproducible and storable. The engine's `hashNoise()` is
  already deterministic and can serve this.
- Amount per slot, independent of the `random` spread source (§20.2) — that
  randomises *ordering*, this randomises *each fixture's rendering*.

### 4.12 Randomness — shuffle, not dice

`[done-decision]` **True unrepeated random.** Independent random draws are the
wrong primitive: they repeat values, sometimes twice in a row, which reads as a
broken effect rather than a random one. Random modes use a **permutation**
instead:

- Shuffle the set of steps, targets or directions; exhaust it; reshuffle.
- Every value appears exactly once per cycle — even coverage, no immediate
  repeats.
- `[flag]` Guard the seam between cycles: reject a reshuffle whose first element
  equals the previous cycle's last, or the one visible repeat the permutation
  was meant to prevent happens exactly at the join.
- Seeded, so a show replays identically. The seed is stored with the phaser.

`[want]` Both available, labelled honestly — `Random` (independent draws, can
repeat) and `Shuffle` (permutation, cannot). Shuffle is the default, since it is
what people mean by random.

Applies to: the `random` waveform, the `random` spread source (§20.2), random
direction (§4.13), and per-fixture variation (§4.11).

### 4.13 Direction and bounce

Direction: `forward` · `reverse` · `bounce` · `random` (per §4.12, so a random
direction never repeats immediately).

**Bounce pivot** `[done-decision]` — three modes, because a spread across a
group gives the travelling wave *width*, and where it reverses changes the look
entirely:

| Mode | Reversal point | Result |
|------|---------------|--------|
| **At the end** | the leading edge reaches the last member | the wave never leaves the group; densest, no gaps |
| **Centre at the end** | the wave's midpoint reaches the last member | half the wave overshoots; a pause at each extreme |
| **Over the end** | the whole wave travels past the last member | full overshoot; a clear gap before it returns |

`[done-decision]` **Confirmed: bounce pivots on spread position**, not on the
value range. The three modes govern where the travelling wave reverses relative
to the ends of the spread.

Consequences worth stating, since this is a spread-space operation:

- Bounce is a transform on the **spread source** (§20.2), alongside groups, blocks
  and wings (§20.3) — not a property of the waveform. So it composes: a wave can
  bounce across a grid axis or around a stage angle, not only along selection
  order.
- `[flag]` **Engine placement:** bounce belongs in the spread-table resolution
  from §20.9, not in `Phaser::wavePoint()`. Putting it in the waveform would
  make it incompatible with grid and stage spread sources.
- `[want]` The overshoot region needs defined behaviour for members outside the
  spread during the overshoot: hold at the extreme value, or drop to the base.
  Hold reads as a pause, drop reads as a gap. Per mode, defaulting to hold for
  `Centre at the end` and drop for `Over the end`.
- `[want]` Bounce is visible in §4.14's 3D view as a folded surface, which is
  the quickest way to tell the three modes apart while setting one up.
- `[want]` Overshoot amount configurable on `Over the end`, so the gap length is
  adjustable rather than fixed at one wave width.
- `[want]` Bounce applies to any spread source (§20.2), so a wave can bounce
  across a grid axis or around a stage angle, not only along the selection.

### 4.14 The FX editor

Three views on the same phaser, switchable. `[want]` Clean and readable above
all — this is the window where a dense effect either becomes comprehensible or
does not.

**Form view** — value against phase, one cycle wide.

- The editing surface for the **curve** generator: bezier control points with
  draggable handles, add/remove points, snap to grid, numeric entry per point.
- Step boundaries, attack and decay shown in place, so §4.5's parameters are
  visible as shape rather than as numbers.
- `[want]` Overlay every selected fixture's curve, offset by its phase, so the
  spread is visible at a glance. The single most useful thing this view can do.
- `[want]` A live playhead, and the current output value marked on each curve.

**Stack view** — the layers from §4.7, each with its own form, blend mode,
opacity, mute and solo.

**3D view** `[done-decision]` — a surface plot:

```
X = selection (fixtures in selection order, §20.4)
Y = value
Z = time
```

This makes phase spread, grouping, blocks and wings (§20.3) legible in a way no
2D view can: a sine with 360° spread reads as a diagonal wave across the
surface, blocks as terraces, wings as a mirror fold.

- `[want]` Orbit, zoom, and a configurable time window of N cycles.
- `[want]` Scrub the Z axis to step through time, with the rig following live.
- Reuses the §13 GPU pipeline rather than a separate renderer.
- `[open]` Spread sources other than selection order on the X axis — grid
  position or stage angle (§20.2) would be more meaningful for spatial effects.
  Probably worth it; the axis just reads the resolved spread table.

### 4.15 Gap

`[done-decision]` A **gap** between loops: the phaser completes its cycle, waits,
then repeats. Length definable in seconds or musical time (§4.8).

- Fixed length, or a **sequence** of lengths cycled through — `1 bar, 1 bar,
  2 bars` — so the repeat is not metronomic.
- `[want]` Random gap length from a range, using §4.12's shuffle so lengths do
  not repeat immediately.

#### Parameters

Seven independent settings, because each answers a genuinely different question:

| Setting | Options |
|---------|---------|
| **Hold** | `base` (fall to the underlying value) · `hold last` · `first` (snap to the cycle's start and wait) · `fade to base` (with its own time and curve) |
| **Placement** | `after` the cycle (default) · `before` · `split` (half each end) |
| **Length** | fixed · sequence · random from a range · **fraction of cycle** (scales with speed instead of staying absolute) |
| **Frequency** | every loop · **every Nth loop** — three cycles then a gap |
| **Phase on resume** | `reset` to 0 · `continue` from where it stopped · **`reshuffle`** (new spread permutation each loop, §4.12) |
| **Scope** | per fixture · group |
| **Transition** | snap · eased in and out, with its own curve |

**`[flag]` Scope is the one real fork**, because a spread means fixtures finish
at different times:

| Scope | Behaviour | Use |
|-------|-----------|-----|
| **Per fixture** (default) | the gap is part of each fixture's own cycle, so it travels with the phase | a pulse rolling across the rig with pauses riding along |
| **Group** | the gap starts once the *last* fixture completes; the whole group waits together | a collective breath — everything stops, then everything restarts |

Per fixture is the cheap one: cycle length becomes `active + gap` and the
existing phase maths handles it. Group scope needs an effective cycle of
`active + spread + gap` plus a group-completion check.

#### Named behaviours

`[done-decision]` Seven orthogonal parameters is powerful and unusable, so gap
follows the same pattern as recipes over slots (§4.1): **named behaviours that
set the parameters, with the parameters available underneath.**

| Behaviour | Does | Reads as |
|-----------|------|----------|
| **Pause** | base · after · every loop · reset | the plain one — effect runs, stops, runs |
| **Freeze** | hold last · after · every loop · reset | motion halts mid-gesture, then resumes |
| **Breathe** | fade to base, eased · split · every loop | organic swell and settle |
| **Burst** | base · after · every **N**th loop · reset | rhythmic bursts of activity |
| **Stutter** | hold last · short random lengths · continue | nervous, broken motion |
| **Drift** | base · continue phase | never realigns; slowly goes out of step with itself |
| **Reshuffle** | base · after · reshuffle on resume | a new random pattern every loop |

- `[want]` User-definable behaviours saved alongside the shipped ones, stored in
  the show and exportable with the recipe library (§4.4).
- `[want]` Editing a parameter under a named behaviour marks it as modified
  rather than silently detaching, with a one-press revert.

### 4.16 Musical time

`[done-decision]` Any time value in the FX engine is expressible in **seconds or
bars and beats**, with a modal switch between the two displays.

**Musical units:** bars · beats · subdivisions (`1/4`, `1/8`, `1/16`, `1/32`),
with dotted and triplet variants.

- Requires a **time signature** (4/4 default) to define a bar, and a **tempo
  source**: internal BPM, tap, MIDI clock, or timecode (§12.3, §18.5). §4.8's
  speed masters already accept MIDI clock and tap, so this extends that rather
  than adding a second mechanism.
- `[want]` Tempo source and signature visible wherever musical time is shown —
  a beat value means nothing without knowing what is driving it.

**`[flag]` The unit is stored with the value, not just displayed.** This is the
part that is easy to get wrong:

- A speed entered as `2 beats` *stays* musical and follows tempo changes.
- A speed entered as `1.5 s` *stays* absolute and ignores tempo.
- The modal switch changes the **display and the entry default**. It does not
  convert existing values, and so does not silently bind or unbind them from
  tempo.
- `[want]` An explicit **Convert** action for when that is actually wanted, per
  value or across a selection.

Without that rule, toggling the display mode would relink every fade in the show
to tempo — or unlink it — invisibly. The display mode is a lens; the unit is
data.

**Scope.** `[want]` The same modal switch applies app-wide, not only to FX:
cue fade, delay and part timing (§11.2), chase rate (§11.5), release times
(§11.3) and gap lengths (§4.15). A show programmed to music wants all of it in
bars; a theatre show wants all of it in seconds.

### 4.17 Live FX playback and control

`[done-decision]` FX run as **playbacks**, not only as values stored in cues: a
running recipe instance with live parameter state, focusable and controllable
while it runs.

- Multiple concurrent FX playbacks, each independently controlled.
- Each is a **playback layer with a priority**, merging like any executor
  (§11.4) — the same model as §13.3's extraction layer, so the merge pipeline
  gains no new concepts.
- Assignable to a tile (§9.3 already lists an FX recipe tile) or an executor,
  with its fader driving size or rate by choice.

#### The control set

Nine live controls, all of which address parameters already specified:

| Control | Governs | Spec |
|---------|---------|------|
| **Size** | amplitude | §4.1 |
| **Width** | pulse width / duty — how much of the cycle is "on" | §4.5 |
| **Spread / Phase** | spread amount and offset | §20.3 |
| **Form** | generator and waveform | §4.5 |
| **Gap** | gap length and named behaviour | §4.15 |
| **Speed** | the phaser's own cycle rate | §4.8 |
| **Rate** | a multiplier on top of speed, so double-time needs no stored change | §4.8 |
| **Direction** | forward · reverse · bounce · random | §4.13 |
| **Selection** | spread source — linear, grid, stage | §20.2 |

`[want]` When an FX playback is focused, the encoder bar (§2.6) pages to these
nine, so the physical controls address the running effect without a mode
change.

#### Parameter presets

`[done-decision]` A **parameter preset** captures the state of those nine
controls. Distinct from a recipe, and the distinction matters:

| Object | Holds |
|--------|-------|
| **Recipe** (§4.1) | *structure* — which slots, which attributes, which roles |
| **Parameter preset** | *state* — a snapshot of the live control values |

- `[want]` **Recipe-agnostic where possible.** Size, speed, rate, spread, gap
  and direction apply to any running FX, so one `fast and wide` preset works
  across recipes. Form applies only where the target recipe's generator matches;
  the preset stores what it can and says what it skipped.
- Lives in its own pool with §7's sorting, filtering and UIDs (§8.1).
- `[want]` Capture from the current live state in one gesture — busk it until it
  looks right, then store it.

#### Recall

- **Radial** (§5) — sectors are parameter presets, branches lead to individual
  parameter adjustment. §5.1's flick-to-execute is the muscle-memory path, which
  is exactly what busking FX wants.
- **Panel** (§10.1) — for scanning a large preset pool, per the radial-for-
  actions / panel-for-selections rule.
- **Bindings** (§12) — a MIDI controller's encoders drive the nine controls,
  its buttons recall presets, with feedback (§12.5).

#### Fade and morph on recall

- `[want]` **Recall with a fade time**, so parameters move rather than snap —
  size swelling and speed ramping into the new state. Per preset, with an
  override at recall.
- `[want]` **Morph between two parameter presets** on a fader or an XY pad,
  continuously interpolating the numeric controls. Discrete controls (form,
  direction, selection mode) switch at the midpoint rather than interpolating.
  This is the single most expressive busking tool in the FX engine and nothing
  else in the spec provides it.
- `[flag]` Morphing **speed** needs care: crossfading a cycle rate while a
  phaser is mid-cycle causes a visible jump unless phase is preserved across
  the change. Interpolate phase continuously and derive the rate from it, not
  the other way round.

#### Engine impact

| Change | Note |
|--------|------|
| FX playback object: running recipe instance with live parameter state | §4.17 |
| FX playbacks join the merge pipeline as prioritised layers | §11.4 |
| Parameter preset pool | §7, §8.1 |
| Parameter fade and morph interpolator | phase-preserving for speed |

---

## 5. Radial action menu

Replaces MA3's swipe gesture with a game-style radial selection tree.

### 5.1 Behaviour

- Long-press, right-click or two-finger tap on any object opens a radial menu
  centred on the touch point.
- Objects that carry menus: fixtures, groups, cues, executors, recipes,
  windows, palettes, patch rows.
- Release executes. Release in the centre cancels.
- A fast flick in a direction executes without waiting for the ring to draw, so
  the layout becomes muscle memory.

### 5.2 Tree navigation

A sector is either a **leaf** (executes a command) or a **branch** (opens the
next level). Branches are marked with a chevron on the outer edge; leaves are
not. Arbitrary nesting depth.

Reference interaction, as described by the user: touching a fixture opens a ring
of 6; the bottom sector is a branch, so touch-and-hold on it opens its own set
of actions, each of which may itself branch on touch-and-hold.

Two navigation modes, both live, because they suit different speeds:

- **Drill (held).** Keep the finger down and hold on a branch — the next level
  replaces the current ring, centred on the same point. The parent collapses to
  a thin breadcrumb arc with the chosen sector highlighted. Releasing on a leaf
  executes; releasing on a branch opens it and keeps the menu up.
- **Step (tapped).** Tap a branch to open its children and lift the finger. The
  menu stays open for a second tap. Suits desk work and mouse use.

Backing out: tap the centre, drag back through the breadcrumb arc, or swipe
inward. Centre tap at the root closes.

- `[open]` Hold duration before a branch auto-opens — 180 ms default in the
  prototype, adjustable 60–600 ms for feel-testing against accidental opens.
- `[done-decision]` Sub-rings **replace in place** with a breadcrumb arc inside
  the hub, rather than growing as concentric rings. Keeps the radius readable
  at depth; context is carried by the breadcrumb ticks instead.

**Prototype:** `proto/radial-menu.html` — working interaction test. Covers
hold-to-drill, step mode, breadcrumb arcs, chevron branch marking, flick-to-
execute, live ring geometry, and the JSON tree editor.

### 5.3 Visual design

Clean and minimal, built so customisation never degrades it:

- Sectors are flat arcs with generous gaps; no gradients, no bevels, no drop
  shadows. One accent colour for the active sector, everything else neutral.
- Labels sit horizontally inside the arc, never rotated with it — rotated text
  is the main thing that makes radial menus feel cheap.
- Icon optional, label optional, but at least one required. Icon-only rings
  stay tight; label rings expand the radius to fit.
- The ring is sized by sector count, not fixed: 4 sectors is a small ring, 8 is
  a wide one. Readability over consistency.
- Everything under the menu dims slightly rather than being covered by a scrim,
  so the object being acted on stays visible — the menu is about *that* object.
- Sector count: 4–8 per ring, 6 as the comfortable default. More than 8 wraps
  into a `More…` branch automatically rather than slicing thinner.

### 5.4 Customisation

- Per-object-type menu trees, each independently editable.
- Node = `{label, icon, command, children}`; `command` is a console string.
- Drag to reorder sectors; drag a node onto another to nest it.
- Angular position is editable, so a user can pin Store to the same vector in
  every menu and build directional memory across the whole app.
- `[open]` Ship a locked default set alongside user menus so a broken custom
  tree is always one tap from recoverable.

### 5.5 Storage and recall

- A menu node is `{label, icon, command, children}` where `command` is a
  **command-line string** — the same syntax the console parser executes. Menus
  are therefore macros with a radial skin, and anything typeable is bindable.
- Menus are named, stored in the show, exportable, and assignable per object
  type and per window.
- `[want]` Recallable — bind a named menu to a hard button or a gesture.
- `[open]` Default trees per object type, designed so common actions sit on
  stable vectors (e.g. down-right is always Store) to build directional memory.

---

## 6. Attribute taxonomy — Lumen's own

**Lumen's attribute model is native, not GDTF's.** GDTF, ChamSys `.hed`, laser
zones and pixel-mapper cells are all *import mappings onto this taxonomy*. All
sorting, filtering, preset organisation, FX role binding and UI grouping read
this, never the source profile's own vocabulary. A fixture imported from GDTF
and a laser zone defined natively sort side by side because both resolve to the
same sub-attribute.

### 6.1 Structure

Two levels: **main group** → **sub-attribute**. A sub-attribute may carry a
*kind* flag that drives behaviour (merge rule, anchor detection, encoder feel).

**Position**
- `Pan`, `Tilt`
- `PanRotate`, `TiltRotate` — continuous/infinite rotation: direction + speed,
  not absolute position. Anchored (see §7.3).
- `PanTiltSpeed`, `PanTiltMode`
- `PosX`, `PosY`, `PosZ` — moving trusses, laser scan position, mapped surface
  placement
- `RotX`, `RotY`, `RotZ`

**Color**
- Additive: `Red`, `Green`, `Blue`, `Amber`, `White`, `Lime`, `Cyan`, `UV`,
  `WarmWhite`, `CoolWhite`, `Indigo`, `Mint`
- Subtractive: `CyanSub`, `Magenta`, `Yellow`
- Correction: `CTO`, `CTB`, `CTC`, `Tint`
- Model: `Hue`, `Saturation`, `ColorTemp`
- Discrete: `ColorWheel1..n`, `ColorWheelSpin1..n`, `ColorMacro`

**Beam**
- Shutter: `Shutter`, `StrobeRate`, `StrobeMode`
- Optics: `Zoom`, `Focus`, `Iris`, `Frost`, `Barndoor`
- Gobo: `Gobo1..n`, `Gobo1Pos..n` (indexed), `Gobo1Spin..n` (anchored)
- Prism: `Prism1..n`, `Prism1Pos..n`, `Prism1Spin..n` (anchored)
- Animation: `AnimWheel1..n`, `AnimWheelPos`, `AnimWheelSpin` (anchored)
- Shapers / framing: `Shaper1A/1B..4A/4B`, `ShaperRot`, `ShaperMacro`
- Effects: `BeamEffect`, `BeamFx1..n`

**Intensity** `[done-decision]` — its own group, not folded into Beam.
- `Dimmer`, `DimmerFine`, `DimmerCurve`, per-cell dimmers on multi-instance
  fixtures.
- Carries the `intensity` kind, which is what the merge pipeline, grand master
  and blackout read.

**Control** `[done-decision]` — its own group, kept away from the others.
- `LampOn`, `LampOff`, `Reset`, `ResetPanTilt`, `FanMode`, `FanSpeed`,
  `DisplayIntensity`, `Calibrate`, `PowerMode`

### 6.1.1 Access — where each group lives

The five groups are not used at the same rate, so they do not get equal
footing. Access is deliberately asymmetric:

| Group | Access |
|-------|--------|
| **Intensity** | Permanent. Dedicated fader plus the first encoder bank, always live, never behind a group selector. It is touched more than everything else combined. |
| **Position** | Encoder bank, in the main group rotation. |
| **Color** | Encoder bank, in the main group rotation. |
| **Beam** | Encoder bank, in the main group rotation. |
| **Control** | **Not** in the encoder rotation. Reached explicitly: a `Control` window, or the `Fixture ▸ Control` radial branch. |

Why Control is separated: `Reset` and `LampOff` are destructive mid-show. If
Control sat in the encoder group rotation, one wrong group press plus one
encoder nudge could strike a lamp during a cue. So:

- `[want]` Control attributes require an explicit press-and-confirm, not an
  encoder sweep. Momentary buttons with a hold-to-fire, not continuous values.
- `[done-decision]` **Opt-in Control presets.** Excluded from preset pools by
  default; the user can explicitly create one (`lamp on all`, `fan quiet`,
  `display off`). A Control preset is marked as such in the pool, and the
  marking is not cosmetic — see §7.6.
- `[done-decision]` **Control values do store into cues**, like any other
  attribute. A pre-show cue that lamps on and an end-of-night cue that lamps
  off are both wanted.
- `[want]` **Fire on change, not on restore.** Control attributes emit only
  when their value *transitions*. Re-entering a cue that already holds
  `LampOn` does not re-pulse it, and jumping backwards through a stack does not
  re-fire a `Reset`. This is what makes cue storage safe without taking the
  feature away — the guard is in the output stage, not in what may be stored.
- `[want]` Control attributes are excluded from crossfades: they step at the
  cue's trigger point rather than fading, since a half-faded `Reset` is
  meaningless.

### 6.2 Kind flags

A sub-attribute's kind changes behaviour across the whole app, so it is
declared once here rather than special-cased per feature:

| Kind | Meaning | Drives |
|------|---------|--------|
| `intensity` | HTP-merged, master-scaled | merge pipeline, grand master |
| `anchored` | Range has a semantic zero (stop/centre) | preset auto-stepping, FX centring |
| `indexed` | Discrete slots, not continuous | encoder snapping, preset-per-slot |
| `continuous` | Plain analogue range | default |
| `virtual` | Derived, not a real DMX channel (Hue, Sat) | patch, programmer |
| `axis` | Spatial — bindable to FX roles `AxisX`/`AxisY` | FX role resolution (§4.2) |

### 6.3 Import mapping

- `[want]` GDTF importer maps GDTF attribute names onto this taxonomy via a
  mapping table shipped with the app, editable when a profile uses something
  unusual.
- `[want]` ChamSys `.hed` importer maps its channel types onto the same
  taxonomy.
- `[want]` Unmapped source attributes land in a visible `Unmapped` bucket
  rather than being dropped, with a one-click assign.
- **Engine impact:** `Core.hpp`'s current flat `Attr` enum must be restructured
  into `{group, sub, kind}`, and `mergeRuleFor()` must read the `intensity`
  kind flag instead of testing for `Attr::Dimmer`.

---

## 7. Palettes and presets

### 7.1 Three-layer attribute identity

Every attribute displays as three independent parts, so one glyph never has to
carry all the meaning:

| Layer | Holds | Prism example |
|-------|-------|---------------|
| **Symbol** | What the attribute *is*. Fixed per attribute. | prism glyph |
| **Label** | Its name, from GDTF `Pretty`. User-overridable. | `Prism 1` |
| **Indicator** | Live qualifier or state. Varies at runtime. | `3-facet` / `↻ 14 rpm` |

A prism therefore reads as a chain of attributes, each with its own symbol,
label and indicator — which is what the user described:

```
◇  Prism 1        3-facet          (GDTF: Prism1)
◈  Prism 1 Pos    142°             (GDTF: Prism1Pos)
↻  Prism 1 Rot    CW · 14 rpm      (GDTF: Prism1PosRotate)
```

The indicator is a small composite, not a string: `{glyph?, value, unit?}`.
Direction is a glyph (`↻ ↺ ■`), speed is a value plus unit. For a bidirectional
attribute the direction glyph comes from which side of the semantic anchor the
value sits on, not from a separate stored field.

- Symbols come from a Lumen icon set keyed by GDTF attribute name, since GDTF
  defines no iconography. `[open]` ~350 GDTF attributes exist; ship symbols for
  the common set and fall back to the Feature Group glyph plus the label.
- `[want]` User-overridable label and symbol per attribute, per show.

### 7.2 Range labels

GDTF already names DMX ranges: a `ChannelFunction` carries
`PhysicalFrom`/`PhysicalTo`, and its `ChannelSet` children each carry a name
and a DMX sub-range. Those names *are* the range labels.

- Imported profiles get range labels for free.
- The fixture editor (§1.2) edits them in the table view: one row per
  ChannelSet, with name, DMX from/to, physical from/to.
- `[want]` Where a profile ships unnamed or badly named sets, labels are
  editable and the override is stored in the show, not written back into the
  library fixture.
- `[open]` Label inheritance — should an override on `Prism1PosRotate` for one
  fixture type propagate to every fixture of that type, or per-fixture?

### 7.3 Auto-generated stepped presets

Take a range, divide it into N equal steps, emit one preset per step.
Customisable step count. The naive version breaks on exactly the attribute the
user raised, so the generator needs two refinements:

**Problem 1 — semantic anchors.** A rotation attribute is not a uniform range.
`Prism1PosRotate` typically runs `CCW fast → CCW slow → stop → CW slow → CW
fast`. Dividing that into 8 equal steps produces presets that straddle the stop
point and are useless. So the generator detects **anchors** (a ChannelSet whose
physical value is 0, or named stop/open/off) and steps *symmetrically outward
from the anchor*, N per side, keeping the anchor as its own preset.

**Problem 2 — DMX space vs physical space.** Equal DMX steps are not equal
physical steps when the profile defines a nonlinear mapping. Both are wanted:
equal DMX for "spread these evenly across the channel", equal physical for
"give me 5, 10, 15, 20 rpm".

Generator modes:

| Mode | Steps are equal in | Use |
|------|--------------------|-----|
| `dmx` | DMX value | Even spread across a channel |
| `physical` | Physical value (°, rpm, Hz) | Real-world values |
| `per-set` | Within each ChannelSet, N each | Gobo wheels, prism facets |
| `symmetric` | Outward from a detected anchor | Rotation, pan/tilt, CTO |

Default: `per-set` with `symmetric` applied automatically where an anchor is
found. `[open]` Confirm this default is what the user expects.

**Naming templates.** Generated names come from a token string so a batch never
needs hand-renaming:

```
"{attr} {set} {n}/{N}"        →  Prism Rot CW 3/5
"{attr} {dir} {phys}{unit}"   →  Prism Rot ↻ 14rpm
"{set}"                       →  3-facet
```

Tokens: `{attr} {set} {n} {N} {phys} {unit} {dir} {dmx} {fixtype}`.

- `[want]` Preview the generated set before committing — a list of names and
  values, editable, then Create.
- `[want]` Regenerate in place: change N from 5 to 8 and the set rebuilds,
  keeping cue references where names match.
- `[open]` What happens to cues referencing preset 7 when a regeneration
  produces only 5? Leaning: keep the orphan, mark it, never silently delete.

### 7.4 Sorting — two modes

Sorting offers an explicit choice, because the two behaviours have very
different consequences:

**Mode A — view sort (default, non-destructive).** Changes display order only.
IDs untouched, references untouched. Per-pool-view, remembered with the window
state (§2.1), so two open preset windows can sort the same pool differently.

**Mode B — renumber (destructive, guarded).** Rewrites the IDs to match the new
order. Flow, in this order:

1. Warn before executing, naming exactly what will change: how many presets are
   renumbered, and how many references across the show will be rewritten.
2. Offer to make a backup of the show file. Default to **yes**.
3. Execute: renumber IDs, then rewrite every reference so nothing breaks.
4. Report what was remapped, with undo available.

References that must be rewritten in step 3:

| Holder | Reference form |
|--------|----------------|
| Cues | preset ID in stored values |
| Other presets | nested/referenced presets |
| Groups | preset links |
| Executors | assigned preset |
| FX recipes | preset-valued slots |
| Macros | **ID inside a command string** |
| Radial menus | **ID inside a command string** (§5.5) |
| Hard buttons | bound command string |

- `[resolved by §8]` With UID-based references this table mostly disappears —
  structured references need no rewriting, because they do not store IDs.
  See §8.5. The command-string risk remains only for **positional**
  references (`@4.12`), which are meant to follow the slot.
- `[want]` Dry-run view: show the full remap table and every command string
  that will be rewritten, before committing.
- `[want]` Manual drag reorder within a pool, in either mode.
- `[want]` Auto-sort keys: name, ID, creation time, main group, sub-attribute,
  kind, fixture type, physical value, last used.

### 7.5 Filtering

Filtering is faceted off **Lumen's own taxonomy** (§6), so it works identically
for GDTF-imported fixtures, `.hed`-imported fixtures, laser zones and mapped
pixels:

| Facet | Source |
|-------|--------|
| Main group | `Position` / `Color` / `Beam` (+ `Intensity`, pending §6.1) |
| Sub-attribute | `Pan`, `Prism1Spin`, `Shaper2A`, … |
| Kind | `anchored`, `indexed`, `virtual`, `axis`, `intensity` |
| Fixture types | which types the preset holds values for |
| Generated | auto-stepped vs hand-stored |
| User tags | free, user-assigned |

The two examples given resolve to:
- "only presets with rotations" → kind `anchored`, or sub-attribute matching
  `*Rotate` / `*Spin`
- "only presets with the frame/shaper attribute" → sub-attribute `Shaper*`

- `[want]` Facets combine: OR within a facet, AND across facets, with a live
  count beside each option.
- `[want]` Filters expressible as command strings, so a radial sector or hard
  button recalls one: `Preset Filter Sub "Prism1Spin"` / `Preset Filter Clear`
- `[want]` Named saved filters, stored in the show, bindable.

**Filter display — three options, user's choice:**

1. **Hide** non-matching presets. Pool compacts.
2. **Grey out** in place. Pool geometry preserved, so spatial memory survives —
   which matters when busking and reaching by position.
3. **Grey out + unselectable.** Greyed presets stay visible for orientation but
   cannot be triggered while the filter is active, so a mis-aimed press during
   a show cannot fire the wrong preset.

Option 3 is the safe busking default. The setting is per-pool-view and stored
with the window state.

### 7.6 Control presets

`[done-decision]` Opt-in, per §6.1.1. A Control preset holds Control-group
values (`LampOn`, `Reset`, `FanSpeed`, `DisplayIntensity`) and exists because
"lamp on all" and "fan quiet" are genuinely wanted as one press. But a preset
that can strike or reset a rig needs guarding that an ordinary colour preset
does not:

- Marked distinctly in the pool — its own symbol and a border treatment, so it
  never reads as a normal preset at a glance.
- `[want]` Lives in a dedicated Control pool by default, not mixed into the
  Beam or Position pools, so a mis-aimed press during a show lands on nothing.
- `[want]` **Hold-to-fire**, not tap-to-fire, matching §6.1.1's rule for
  Control attributes generally. A tap highlights and shows what it will do; the
  hold commits.
- `[want]` Per-preset `confirm` flag, default **on** for presets containing
  `Reset`, `LampOff` or `PowerMode`, default off for benign ones
  (`DisplayIntensity`, `FanSpeed`).
- `[done-decision]` Storable in cues (§6.1.1), with the fire-on-change rule
  doing the guarding.
- `[want]` Filterable by main group `Control`, so they can be hidden entirely
  during a show and surfaced during prep.

---

## 8. Object references and naming

Applies to every show object — presets, cues, sequences, groups, executors, FX
recipes, menus — not just presets.

### 8.1 Three identities, one reference

Neither the ID nor the name should be what a reference stores. Both are things
the user changes. So every object carries three identities:

| Identity | Mutable | Visible | Purpose |
|----------|---------|---------|---------|
| **UID** | Never | No | What every reference actually stores |
| **ID** (`4.2`) | Yes | Yes | Addressing, pool position, fast typing |
| **Name** (`"Prism CW 3"`) | Yes | Yes | Human recognition |

A UID is assigned at creation, never changes, and is never reused even after
deletion. Everything that points at an object — a cue value, a macro, a radial
sector, a hard button, an FX slot — stores the UID.

**Consequence: rename and renumber both become safe.** Changing an ID does not
break references. Changing a name does not break references. The user's
question resolves to yes, and better than yes — the link survives *both*
changes, not just one.

### 8.2 Referencing by name or by ID

Both work as input; they are two ways of addressing the same object:

```
Preset 4.2                  → by ID
Preset "Prism CW 3"         → by name
Preset 4.2 At 50            → either resolves to the same UID
```

Names must be unambiguous for this to work:

`[done-decision]` **Unique per pool**, not globally. Enforced on entry and on
generation; a collision auto-suffixes (`Prism CW 3`, `Prism CW 3 (2)`).

Consequence: a name-reference needs a pool to resolve against, so the pool is
part of the address.

```
Preset 4."Prism CW 3"       → pool 4, by name
Preset Beam."Prism CW 3"    → pool by name, preset by name
Preset "Prism CW 3"         → resolves only if unambiguous across pools;
                              otherwise the parser reports which pools match
```

- `[want]` Bare names stay typeable. The parser resolves a bare name when
  exactly one pool holds it, and on collision reports the candidates rather
  than guessing. Same-name presets in different pools are the expected case
  (`"Red"` in both a Color pool and a per-fixture-type pool), so this will
  happen often and must read as a prompt, not an error.
- `[want]` The current pool context narrows a bare name first — typing inside
  a pool window resolves against that pool before searching others.

### 8.3 Bound vs positional references

One real distinction the UID model forces into the open. Two reference
intents exist and the user wants both at different times:

**Bound reference (default).** Resolved to a UID when written. Means *this
specific object*, wherever it moves and whatever it is called later. Survives
rename, renumber, re-sort.

**Positional reference.** Stores the literal ID or name, resolved at execution.
Means *whatever currently sits at this address*. Needed for template shows,
busking layouts and "whatever is in slot 1" macros.

```
Preset 4.2        → bound; follows that preset
Preset @4.2       → positional; follows pool slot 4.2
Preset @"Red"     → positional; follows whatever is named Red
```

- `[open]` Confirm the `@` sigil, or use a toggle in the macro editor instead of
  syntax.

### 8.4 Stored command strings

A macro, radial sector or hard button holds a command string, and those contain
references. They are stored **compiled**: references resolved to UIDs at
authoring time, rendered back to the user as the object's current ID or name
when displayed.

So a radial sector authored as `Preset 4.2` displays as `Preset "Prism CW 3"`
after a rename, and still fires the same preset. Positional references
(`@4.2`) are stored as literal text by definition and resolve late.

- `[want]` Display preference: show references by ID, by name, or `ID "Name"`.
- `[want]` A reference whose target was deleted renders as
  `Preset <deleted 4.2>` rather than failing silently, and the object is listed
  in a Broken References view.

### 8.5 What this simplifies

The renumber flow in §7.4 gets much smaller. Structured references need no
rewriting at all, because they hold UIDs. Only two things need attention:

1. **Positional references** (`@4.2`) — these are *meant* to follow the slot, so
   they are left alone, but the dry-run lists which ones will now resolve
   differently.
2. **Display refresh** — nothing to rewrite, just re-render.

The parser-level command-string rewrite pass flagged in §7.4 is no longer
needed for bound references. That flag stands only for positional ones.

Likewise §7.3's "regenerate in place, keeping cue references where names match"
becomes stronger: a regeneration reuses the UID for each step that still
exists, so matching by name is a fallback rather than the mechanism.

---

### 8.6 Undo, redo and history

`[done-decision]` Bindings:

| Input | Action |
|-------|--------|
| **Undo** | undo the last action, any scope |
| **Shift + Undo** | redo |
| **Ctrl / Cmd + Undo** | undo the last **programmer** action specifically |
| **Ctrl/Cmd + Shift + Undo** | redo the last programmer action |

**Transactional entries.** One history entry may contain many operations,
applied and reverted atomically. Required by §7.4's renumber flow and §16.5's
agent batches, both of which must undo as a unit.

**`[flag]` Scoped undo creates an out-of-order problem.** With a history of
`[programmer A, patch B, programmer C]`, `Ctrl+Undo` twice reverts C then A,
leaving B in place. Harmless if A and B are independent, corrupting if B built
on A.

The fix: every entry records the set of object UIDs it touched. A scoped undo is
permitted only when no later entry touched an overlapping set. Where one did,
Lumen refuses and says which action blocks it, offering to undo back through it
instead. In practice programmer actions rarely collide with patch or playback
edits, so the common case is unobstructed.

`[done]` Built as specced, with two details the implementation settled:

- An out-of-order undo **removes** its entry rather than leaving it behind the
  cursor. The cursor cannot express "everything applied except this one", and
  a redo of it would land on different state anyway. An in-order undo leaves
  the entry redoable as usual.
- Redo is guarded the same way in the other direction: redoing an older entry
  out of order is refused when an undone entry between here and there touched
  the same objects.
- The programmer's touched set is the **fixtures** it wrote to, which is what
  lets a patch edit elsewhere in the rig pass the collision check while an
  unpatch of the same fixture correctly blocks it.

**`[flag]` Playback actions are not undoable.** `Go`, `Off`, `Release`, fader
moves and flashes are *performance*, not edits. If Undo could rewind a
playback, it would change live output — so the undo system covers edits only.
That makes Undo inherently safe to press during a show, which is worth more
than the ability to un-Go a cue.

Undoable: patch, programmer, cues, presets, groups, layouts, recipes, menus,
bindings, surface layouts.

**Two separate records**, because the user's ask spans both:

| | Undo history | Action log |
|--|--------------|-----------|
| Purpose | reverting | audit and review |
| Lifetime | volatile, session only | persisted in the show |
| Holds | before/after state deltas | description, timestamp, scope, source |
| Depth | capped, configurable | full |

- `[want]` **History view** showing timestamps, a plain-language description,
  scope, and — under §18.7's session networking — which user made the change.
- `[want]` Jump-to-point: undo everything back to a chosen entry, as one
  confirmed operation.
- `[want]` The action log records agent actions with the prompt that caused
  them (§16.5), so the two systems share one timeline.
- `[want]` Entries store deltas rather than whole-model snapshots, with a
  configurable depth cap — a bulk patch of 2000 fixtures must not blow memory.
- `[done]` Undo history does not survive save/load; the action log does. A
  reopened show knows what was done to it but cannot be rewound past its saved
  state, which is the behaviour an operator can reason about.
- `[done]` An action is **marked** undone in the log rather than deleted: an
  action that happened and was reverted is two facts, not zero.
- `[done]` A command that changes nothing records nothing — an entry that
  undoes nothing makes Undo look broken — and an aborted transaction reverts
  what it already did, so a command that fails halfway leaves no trace.
- `[done]` A bulk operation is ONE entry holding per-object deltas, so
  patching eight fixtures undoes in one press and a 2000-fixture patch costs
  2000 small records rather than 2000 copies of the patch. Depth and byte caps
  evict oldest-first, but never below one entry.

---

## 9. Executor surface — ChamSys exec on an MA3 layout engine

The ask: MA3's layout flexibility with the ChamSys Execute window's
immediacy. These pull in opposite directions — ChamSys is easy *because* it is
rigid, MA3 is flexible *because* it is configurable — so the synthesis has to
be specific about which half wins where.

**Split: flexibility in the canvas, immediacy in the tile.**

### 9.1 Canvas — MA3 side

- Free placement on a **snapping grid**, reusing §2.2's placement grid and its
  runtime-adjustable granularity. Not ChamSys's fixed cell matrix.
- A tile occupies a grid rectangle, resized by dragging edges.
- `[want]` **Overlap forbidden.** A tile cannot be placed on top of another —
  on a busking surface, overlapping targets mean mis-presses. This is the one
  place ChamSys's rigidity is kept.
- `[want]` Alignment helpers: fill row, distribute evenly, match size, align
  edges. These close most of the speed gap with a fixed grid.
- `[want]` Pages of layouts, recallable from the hard-button bank (§2.1),
  stored with window state so a layout travels with the show.
- `[want]` Multi-monitor: a surface can span screens or be per-screen.
- `[explore]` **Nested pages** — a tile that opens another layout page.
  ChamSys pages are flat; nesting is more powerful but much easier to get lost
  in mid-show. Worth prototyping, with these guards designed in from the start
  rather than retrofitted:
  - Depth limit, default 2. Deeper is where people get lost.
  - Persistent breadcrumb on the surface itself, not in a menu.
  - A **Home** hard button that always returns to the root page from any depth,
    one press, no confirmation.
  - Sub-pages visually distinct from the root — a border tint or header band —
    so "am I on a sub-page?" is answerable at a glance under show conditions.
  - A **settings icon in the window header** (§2.4) opens that sub-page's own
    behaviour settings, so each page is configured where it lives rather than
    in a global preferences screen.

  `[done-decision]` Per-sub-page settings, reached from that header icon:

  | Setting | Options |
  |---------|---------|
  | Presentation | **replace** the surface · **overlay** it |
  | Reserved strip | which root region stays visible under an overlay (masters, Go row, none) |
  | Auto-return | off · after N seconds idle |
  | Return target | root · parent page |
  | Depth allowed | inherit · limit here |
  | Visual marker | border tint / header band colour |

  Replace vs overlay therefore becomes a per-page choice rather than one global
  answer — a quick-pick sub-page overlays and keeps the Go row reachable, a
  sustained-work sub-page replaces and uses the full surface.

### 9.2 Tile — ChamSys side

A tile is **self-describing and assigned in one gesture**. This is the part
that makes the ChamSys window pleasant and it is preserved exactly.

**Assignment, two routes:**
1. Tap an empty tile → radial menu (§5) of tile types → pick the object.
2. Drag an object from any pool onto the canvas.

**Legend and colour are live, not typed.** Because a tile stores the assigned
object's **UID** (§8.1), its legend and colour are a *view* of that object:

- Legend auto-populates from the object's name. Rename the preset and every
  tile showing it updates. Neither reference console does this.
- Colour auto-derives from the object's main group (§6), or from a colour
  preset's actual colour.
- Symbol comes from §7.1's three-layer identity.
- All three remain overridable per tile.

**Density scales with size.** One tile type renders at several levels of detail
depending on the grid rectangle it was given — this is what lets the flexible
canvas keep ChamSys's legibility:

| Size | Shows |
|------|-------|
| 1×1 | symbol + colour only |
| 2×1 | symbol + legend |
| 2×2 | + live state (current cue, level, rate) |
| 3×2+ | + progress bar, next cue, timing |

### 9.3 Tile types

Binds to the existing `Executor` model in the engine (fader, go/back/off).

- **Executor** — fader + Go / Pause / Flash / Off
- **Cue stack** — current cue, next cue, progress, timing
- **Group**, **Preset**, **Macro**, **FX recipe**
- **Attribute encoder strip**, **Intensity fader**
- **Rate / speed master**, **Grand master**, **Blackout**
- **Timecode** display and transport
- **Laser zone** — arm, safety status, zone select
- **Video surface / layer** — opacity, blend, media select
- **Status** — DMX output rate, network, DAC state
- **Label**, **divider**, **image** — pure layout, no action
- `[open]` **Page link** tile, pending the nesting question above.

#### 9.3.1 Resolution happens once, in the engine

`[done-decision]` §9.2 makes a tile's legend, colour and symbol a *view* of
the assigned object. That resolution lives in the **engine**
(`resolveTile`), not in each interface that draws a surface. A second
renderer that re-derived the legend would eventually derive it differently,
and the whole point of storing a UID is that there is one answer.

What a tile resolves to is everything an interface needs and nothing more:
legend, detail line, symbol, colour, a coarse hue, **the command a press
sends**, the command a right-press sends, whether it is live, and a fader
position with the command that sets it. A tile *is* its command, so no
renderer needs a switch over kinds to act on a press.

Three things that fall out, each of which was a decision:

- **A colour preset carries the colour it actually produces**, resolved
  through the preset itself rather than guessed from its name. A palette of
  swatches that all render identically is one you have to read instead of
  see, which defeats having a palette.
- **Only tiles with a level get a fader.** A group tile with one would imply
  selection were a level. The grand master gets a fader and *no* press
  command: one that could be changed by a tap rather than a deliberate drag
  is a blackout waiting to happen.
- **A tile whose object is gone is drawn BROKEN, not omitted.** Dropping it
  would take its position with it, and on a wing position is the entire
  bargain (§9.6). Likewise a kind nothing drives yet renders inert rather
  than looking pressable.

Laser zone tiles resolve too, under the rules §14.6.1 settles. **Macro**
tiles remain unresolved for the simpler reason that no macro object exists
yet.

### 9.4 Edit mode vs run mode

- Explicit toggle, with run mode the default. In run mode tiles cannot be
  moved, resized, reassigned or cleared — only operated.
- `[want]` The mode is visible at a glance, not a hidden state: edit mode
  shows the grid and tile handles, run mode hides both.
- `[want]` Mode is per-surface, so a prep layout can stay editable while the
  show surface is locked.

### 9.5 What each console contributes

| Behaviour | From | Kept because |
|-----------|------|--------------|
| Free placement, adjustable grid | MA3 | the flexibility asked for |
| View/page presets, multi-monitor | MA3 | layouts travel with the show |
| One-gesture assign | ChamSys | the ease asked for |
| Auto legend and colour | ChamSys | no typing to make a usable surface |
| Large, self-describing targets | ChamSys | busking legibility |
| No overlapping tiles | ChamSys | mis-press safety |
| Live legend via UID | neither | §8 makes it free |

### 9.6 Both styles, rendered

`[done-decision]` §9's split was decided in prose. It is now also **built both
ways**, rendering from the same executor state, so the decision can be looked
at rather than argued about — `web/surface.js`, switchable in the Executors
window and viewable side by side.

What rendering both actually showed:

- **The wing's claim is position permanence**, and it only holds if nothing
  ever reflows. Empty slots therefore have to be *drawn*, not hidden: the empty
  one is still a button under a thumb. This is why the side-by-side view makes
  the wing **scroll** rather than fit — reflowing it to half a screen would
  quietly destroy the single property it exists to demonstrate.
- **The canvas's claim is that size carries meaning**, which only holds if a
  cell is a fixed size in pixels. Fractional columns would make a tile placed
  at 3×2 a different size on every screen, and a layout that changes shape per
  screen does not travel with the show (§9.1). The grid is therefore sized in
  pixels and the canvas scrolls.
- The two are **not a feature difference**. Every tile in both styles does the
  same things — Go, Off, master — through the same commands. What differs is
  only how much the operator is allowed to arrange, which is exactly the trade
  §9 names.

`[done-decision]` **Both stay.** The style is therefore a property of a
*page*, not a preference: a show owns a busking wing and a programmed canvas
at once and moves between them, rather than having one answer imposed on every
surface it owns. A global switch would have been cheaper and would have made
the question come back every time someone wanted the other one for ten minutes.

So a `Surface` is a named page with a style, a matrix size, and tiles; tiles
store the assigned object's **UID** (§8.1), which is what makes the legend a
view rather than a copy. Pages persist in the show file, because a show that
forgets which of its surfaces was a wing has not kept both — it has kept one
and guessed.

### 9.7 Converting a page, and what it costs

`[done-decision]` Keeping both forces an operation neither console needs: a
page has to be convertible. It is **not symmetric**, and the asymmetry is the
whole of the design:

| Direction | Cost |
|---|---|
| wing → canvas | **lossless.** Every slot is already a 1×1 cell at the right position, so the canvas simply stops ignoring the rectangle. Nothing moves, nothing shrinks. |
| canvas → wing | **lossy, two different ways.** A 3×2 tile cannot stay 3×2 in a fixed matrix, and two tiles whose rectangles begin in different cells can land in the same slot. |

The two losses are counted separately because they are different kinds of bad:

- **Resized** changes how a tile looks. Annoying, reversible by hand, and
  therefore allowed without ceremony — nagging about it would train people to
  dismiss the warning that matters.
- **Displaced** means a tile has nowhere to be, and converting would delete it.
  That is refused unless explicitly forced, with every affected tile named.
  Losing a layout somebody arranged by hand is always a decision they made,
  never one that happened to them.

A consequence worth stating because it will surprise someone: **canvas → wing →
canvas is not the identity.** The sizes are gone after the first leg, and
coming back gives 1×1 tiles in the right places. There is no hidden copy of the
old geometry, deliberately — a surface that silently remembered a shape it is
no longer showing is a worse surprise than one that forgot.

### 9.8 Arranging the canvas

`[done-decision]` A free canvas is only usable if arranging it is quick, so
§9.1's alignment helpers are built rather than deferred. Three things about
how, each decided by a failure it prevents:

**Refusal happens during the drag, not on the drop.** The engine refuses an
overlapping placement (§9.1), but refusing when the pointer is released is too
late: the gesture is already committed, and being told no then reads as being
told off. The drag therefore carries a ghost that turns red the moment the
position under it would not be accepted, using the same rule the engine
applies. The engine still decides — the preview only avoids asking a question
whose answer is visibly no.

**Alignment is computed as a whole arrangement and applied all or nothing.**
Moving tiles one at a time accepts the first three and fails the fourth,
leaving the page in a shape nobody chose and that the operator now has to
reason about before they can undo it. So the proposed arrangement is checked
entire — against every tile on the page, not only the selected ones — and
either lands or does not.

**Selection order is information.** "Match width" takes its width from the
*first* tile picked, not the widest, because "make these like that one" needs
*that one* to be something the operator chose rather than something the
geometry decided. Shift-adding appends; re-picking an already-selected tile
does not reorder.

Run mode (§9.4) is enforced at the **command layer**, not in the interface. A
lock only the UI respects is not a lock: a macro, a plugin or the agent would
walk straight through it. `TileMove` and `Align` refuse on a page in run mode
and say which command unlocks it. The mode is per page, and a show saved in
edit mode **reopens in run mode** — the safe state is the one a file restores
you to.

| Command | Does |
|---|---|
| `EditMode [on\|off] [page]` | unlocks a page for arranging |
| `TileMove <slot> <col> <row> [w] [h]` | moves or resizes; refuses an overlap |
| `TileSelect [slots…]` | selects tiles, in order; bare clears |
| `Align <how>` | left, right, top, bottom, matchwidth, matchheight, spreadh, spreadv, fillrow |

#### 9.8.1 Surface edits are undoable

`[done-decision]` Arranging a surface records into §8.6's history under the
**Layout** scope. This is the scope that most needs to exist, because its
operations are the hardest to reverse by hand:

- A **conversion** can drop tiles outright, and §9.7 is explicit that the old
  geometry is not kept anywhere. Without undo, forcing one is unrecoverable.
- An **alignment** moves several tiles at once, as one entry. Undoing it tile
  by tile would be absurd, and remembering where four tiles were a moment ago
  is not something anyone does.

The whole tile list is captured rather than a delta. A page holds tens of
tiles, not thousands, so the simple thing is also the affordable thing —
unlike the patch (§8.6), where a bulk operation forced per-fixture records.

Two rules fall out of the general principle that an undo entry must
correspond to a change:

- A **refused** move or alignment records nothing. An undo that does nothing
  is worse than no undo, because it consumes the press that would have worked.
- An alignment that **changes nothing** — tiles that already share the edge
  being aligned to — succeeds and records nothing, for the same reason.

`[flag]` A canvas tile's slot is its top-left cell, so **a tile's slot changes
when it moves**. That is fine for the UI, which knows where it just dragged
from, and awkward for a person typing two moves in a row. Mitigated for now by
naming the occupied slots in the error rather than only saying "not there";
worth revisiting if addressing tiles by hand turns out to be common.

`[open]` Which page a *new show* starts on. Neither is obviously right, and it
is a question about the user rather than the software: the wing wins for
busking a show you did not program, the canvas for a show you built and will
run many times. A starter show ships with one of each (`installStarterSurfaces`)
and opens on the wing, which is reversible in one command.

---

## 10. Popups and presentation modes

Presentation is **separate from content**. The same object — a colour pool, a
cue stack, a rate master — can be surfaced several ways, and which way is a
setting rather than a property of the object. One mechanism, configurable per
use case.

### 10.1 The modes

| Mode | Shape | Suits |
|------|-------|-------|
| **Direct** | no popup; fires immediately | one unambiguous action (Go, Flash, Blackout) |
| **Radial** | ring at the touch point (§5) | 4–8 *actions*, muscle memory, touch |
| **Panel** | floating rectangle anchored near the tile | *selections* — pool grids, colour picker, lists, keypad |
| **Modal** | centred, blocks input until answered | destructive confirms, renumber warnings (§7.4) |
| **Docked** | opens/raises the real window in the layout | sustained work, not a quick pick |
| **Inline** | the tile expands in place to reveal controls | a fader revealing timing, a tile revealing its rate |

**The rule that decides which:** radial for *actions*, panel for *selections*.
A radial menu is bad above 8 items and bad at scanning; a panel is bad at
muscle memory. Picking one preset out of forty is a panel. Choosing between
Store / Update / Merge / Remove is a radial.

- `[want]` Mixed case: a panel may contain a radial, and a radial sector may
  open a panel. The modes compose rather than being exclusive.

### 10.2 Behaviour settings

Each of these is a setting, because the right answer differs per use case:

| Setting | Options |
|---------|---------|
| **Mode** | the six above |
| **Trigger** | tap · long-press (duration configurable) · double-tap · right-click · hold-and-drag |
| **Anchor** | at pointer · at tile edge · centred on surface · fixed position · remembered |
| **Dismiss** | on select · on release · tap outside · explicit close · timeout |
| **Persistence** | single-shot (closes after one pick) · **sticky** (stays open for several picks) |
| **Modality** | blocking · non-blocking |
| **Size** | auto from content · fixed · remembered per popup |

Sticky persistence matters more than it looks: picking six presets in a row
during a busk should not mean reopening the panel six times. `[want]` Sticky
popups show a pin affordance so the state is visible.

### 10.3 Primary and secondary

A tile carries **two** bindings, which is what gives the ChamSys/MA3 hybrid
feel its directness:

- **Primary** — the fast path. Usually `Direct`: tap fires.
- **Secondary** — the options path. Usually `Radial` or `Panel` on long-press.

So an executor tile taps to Go and long-presses to a radial of
Pause / Off / Rate / Goto / Edit. No mode switching, no hunting.

`[done-decision]` **Three bindings**, not two:

- **Primary** — fast path. Usually `Direct`: tap fires.
- **Secondary** — options. Usually `Radial` or `Panel` on long-press.
- **Tertiary** — a third, for edit/config or a less-used action.

`[flag]` **Double-tap conflicts with an immediate-fire primary.** If tap fires
a Go, a double-tap fires it twice before the gesture can resolve. Waiting out
the double-tap window (~250 ms) before firing would make the fast path feel
laggy, which is unacceptable on a Go button. So the tertiary trigger is chosen
per tile, and the settings UI **validates the combination**:

| Primary is | Tertiary may use |
|------------|------------------|
| Immediate-fire (Go, Flash, Blackout) | two-finger tap · modifier+tap · swipe · middle-click. **Not** double-tap. |
| Non-firing or idempotent (select a preset, focus a group) | double-tap also allowed |

Gesture vocabulary available to the three bindings:

- **Touch** — tap, long-press, double-tap, two-finger tap, swipe (4 directions),
  hold-and-drag
- **Mouse** — left / right / middle click, double-click, modifier+click, hover
- **Desk** — encoder push, button chord

- `[want]` Conflicting assignments are refused at assignment time with the
  reason, not discovered later mid-show.

**Discoverability.** Three bindings per tile is only learnable if they can be
seen:

- `[want]` A tile with more than a primary carries a small corner marker — one
  mark for secondary, two for tertiary. Enough to know something is there.
- `[want]` A **Reveal bindings** overlay (hard-button or held modifier) that
  labels every tile on the surface with its three bindings at once. This is the
  thing that makes a dense custom surface usable by someone who did not build
  it.

### 10.4 Setting scope

Precedence, narrowest wins:

```
global default  →  per mode  →  per tile type  →  per tile  →  per input device
```

- `[want]` **Per input device** is the last word, because the right default
  genuinely differs: radial suits touch, panel suits mouse, and a desk with a
  trackball wants neither. A surface used on both a touchscreen and a laptop
  should not have to be rebuilt.
- `[want]` Settings are themselves recallable as named presets
  (`Popups: Touch busking`, `Popups: Programming`), bindable to a hard button.
- `[want]` All of it expressible as command strings, consistent with §5.5 —
  `Popup Mode Radial`, `Popup Sticky On`.

---

## 11. Playback

### 11.1 Tracking — per cue, per layer

`[done-decision]` Tracking is **not** a sequence-wide mode. It is a property of
each *(cue, attribute group)* pair, so one cue can track Position while
blocking Color.

Precedence, narrowest wins:

```
global default  →  sequence default  →  cue  →  cue × attribute group
```

The layers are the five main groups from §6: `Intensity`, `Position`, `Color`,
`Beam`, `Control`.

**Three distinct mechanisms**, often conflated and kept separate here:

| Mechanism | Meaning | Effect on later cues |
|-----------|---------|----------------------|
| **Track** (default) | Cue stores only changes; unstored values inherit from the last cue that set them | changes flow forward |
| **Block** | Cue stores a hard value even where unchanged | an upstream edit stops here |
| **Cue Only** | Changes apply to this cue alone | following cue gets the pre-change value back |

- `[want]` A cue's tracking state is visible in the sheet per group — a five-cell
  strip per cue, not a single flag, so the state is readable at a glance.
- `[want]` `Block` / `Unblock` / `Cue Only` as operations over a cue range, not
  just single cues.
- `[want]` Tracking applies to **phasers** as well as values (§4.3), with the
  same per-group mask.
- `[want]` **Tracking shield**: mark a cue so no upstream edit can track into
  it, whatever the group masks say. For the one cue that must look right.
- `[flag]` **Engine impact.** `Cue` currently holds a full `ValueLayer` applied
  with `overwriteFrom()` — a pure snapshot model. Tracking needs a resolution
  pass that walks from the sequence start (or the nearest Block) to the current
  cue to compute effective values. `Executor::snapshot()` must resolve tracked
  state rather than reading one cue's values.

### 11.2 Cue parts and timing

`[done-decision]` Full MA3-style depth.

- **Per cue**: fade in, fade out, delay in, delay out.
- **Per attribute group**: override any of the above per group — Position
  slower than Color is the common case.
- **Per attribute**: override again at the individual sub-attribute.
- **Cue parts**: named parts within one cue, each with its own fixture set and
  its own full timing. Part 0 is the main part.
- `[want]` Fade curves per cue / group / attribute: linear, S-curve, ease in,
  ease out, custom. The engine's `crossfade()` is currently linear only.
- `[want]` Timing displayed as inherited-or-overridden, so it is obvious which
  level a number came from.
- `[want]` **Learn timing** — play a sequence manually, capture the intervals.

**Trigger types per cue:** `Go` · `Follow +t` · `Wait +t` · `Timecode` ·
`Sound/BPM` · `Link` (jump) · `Loop n`.

- `[want]` **MIB / Mark** — move dark fixtures into position during a preceding
  cue, automatically or on an explicit mark cue.

### 11.3 Release and off

`[done-decision]` Per-playback setting. Each executor chooses:

| Mode | Behaviour |
|------|-----------|
| **To zero** | intensity to 0, everything released |
| **To playback below** | values fall through to the next-priority playback |
| **Hold last** | values stay where they are until something else claims them |

- `[want]` Overridable per attribute group, since "intensity to zero but hold
  position" is the common busking want.
- `[want]` Release time per playback, separate from the cue's own fade out.
- `[want]` `Off` (immediate) distinct from `Release` (timed).

**Bindings:**

| Input | Action |
|-------|--------|
| **Release** | release the current / selected playback |
| **Shift+Release** | **release all playbacks** |
| **Off** | immediate, no release time |
| `[open]` hold Release | proposed: `Off All` — the immediate form, paralleling hold-Clear |

`Shift+Release` honours each playback's own release mode and release time, so a
rig set to hold position and drop intensity does exactly that across the board
rather than snapping to a uniform behaviour.

- It also releases the extraction layer (§13.3), which is a playback layer with
  a priority like any other.
- `[flag]` It does **not** disarm laser zones or touch the safety gate (§14.6).
  Zone content values release; arm state is not a playback value and is never
  changed by a release.
- `[want]` Optional confirmation when a surface is in run mode (§9.4), off by
  default — it is already a deliberate two-key combination, but releasing
  everything mid-show kills the look.

### 11.4 Priority and merging

`[done-decision]` All three mechanisms, configurable per executor.

1. **Priority number** — explicit, breaks ties regardless of executor order.
2. **Merge rule per playback** — HTP or LTP, chosen per playback and
   overridable per attribute group.
3. **Swap / solo groups** — playbacks in the same group release each other, so
   only one position look runs at a time.

Resolution order: priority number first, then merge rule within equal
priority, then executor order as the final tiebreak.

- `[want]` **Assert** — force a playback's values to win even against higher
  priority, for the duration.
- `[flag]` **Engine impact.** `Engine::tick()` currently merges by ascending
  executor id with a fixed HTP-on-`Dimmer` rule. It needs to sort by priority
  number, read a per-executor merge rule, and apply swap-group releases before
  merging.

### 11.5 Chase

`[done-decision]` **Any sequence can run as a chase** — ChamSys's approach, no
separate chase object. A mode switch on the sequence, not a different type.

- Rate (BPM or seconds), direction (forward / reverse / bounce / random),
  crossfade amount 0–100%, per-step level.
- `[want]` Rate bindable to a tile, a hard button, or tapped in.
- `[want]` Sound-to-light / BPM sync as a rate source.

### 11.6 Faders and buttons

Per-playback fader mode: `Master` · `Manual crossfade` · `Rate` · `Temp` ·
`Intensity master only`.

Button functions, assignable per playback:
`Go` · `Back` · `Pause` · `Goto` · `Flash` · `Swap` · `Temp` · `Off` ·
`Release` · `Assert` · `Learn`.

- `[want]` Flash and Swap respect the merge rule, so a flash on an LTP
  playback behaves predictably.

### 11.7 Editing live

- `[want]` **Blind** — programme without affecting output.
- `[want]` **Preview** — view any cue's state without running it.
- `[want]` `Update` with an explicit target chooser: which cue, which playback,
  which preset the change should land in. The ambiguity of "update" is the
  single most common way shows get corrupted, so it asks rather than guesses.
- `[want]` Cue-level **macros**: a cue can fire command strings on entry and on
  exit, consistent with §5.5 and §8.4.

---

### 11.8 Programmer model and Clear

`[done-decision]` The programmer carries **two orthogonal axes per value**:

| Axis | States | Meaning |
|------|--------|---------|
| **Active** | active · inactive | will it be stored? Inactive values stay in the programmer and still output |
| **Kind** | hard · preset reference | a literal number, or a live reference to a preset |

Four combinations, so display has to distinguish them. `[want]` Colour
saturation carries active/inactive; a marker carries kind, and a
preset-referenced value shows **the preset's name** rather than a number, per
§7.1's identity model.

**Clear:**

| Input | Keyword | Effect |
|-------|---------|--------|
| 1st Clear | `ClearSelection` | deselect; values stay and keep outputting |
| 2nd Clear | `ClearActive` | deactivate all values; they remain and still output |
| **Shift+Clear** | `ClearPresets` | remove preset-referenced values, leaving hard values |
| **Shift+Alt+Clear** | `DefaultUnused` | return all unused channels to default; playbacks unaffected |
| 3rd Clear **or hold Clear** | `ClearAll` | empty the programmer |

`[done-decision]` **Driven by programmer state, not a press counter.** This is
the better half of MA3's design: the key resolves against what the programmer
currently holds —

```
selection exists?        → ClearSelection
else active values?      → ClearActive
else anything at all?    → ClearAll
else                     → clear the command line
```

Repeated presses then produce the 1–2–3 behaviour naturally, and the key can
never get out of sync with a hidden counter. No timeout needed, and the
empty-programmer case usefully doubles as a command-line escape.

**`DefaultUnused` (Shift+Alt+Clear)** is the on-demand form of §11.9's
`Unused channels return to default` setting. Where the setting makes LTP
attributes default automatically on release, this does it once, by hand — the
escape hatch for a rig full of stuck positions and colours after a busk.

- Playback levels are untouched. A running cue keeps sending what it sends;
  only residue goes.
- `[flag]` **It releases the residue; it does not write defaults into the
  programmer.** The distinction matters because the alternative reading —
  injecting default values as programmer content — would make them *storable*,
  so the next `Store` would bake every default into the cue. Releasing leaves
  the programmer empty for those attributes, which is almost certainly what is
  wanted. Flagged in case the other behaviour was intended.
- With §11.9's setting on, residue never accumulates and this becomes a no-op.
  The two are complementary rather than redundant.

- `[want]` Clear **never touches playbacks.** `Off` and `Release` (§11.3) do
  that. Users conflate the two, so the separation should be strict.
- Shift+Clear *removes* preset references. `[want]` A separate **Flatten**
  command resolves them to their current literal values and keeps them as hard
  values instead — the other thing people want and a different operation.
- `ClearActive` deactivates both kinds; the axes are independent.

**Storing** (§11.7): active values store, inactive do not. Preset-referenced
values store **as references** by default, so editing the preset updates every
cue using it.

- `[flag]` **This is a major feature that falls out of the kind axis.** Change
  the "Warm" colour preset and every cue referencing it follows. It is how
  designers actually work, and it is safe because the reference is a UID
  (§8.1). `[want]` A per-store and global option to store flattened instead,
  for the cases where a cue must not move.

### 11.9 Unused attributes

`[done-decision]` **"Unused" means not used by the programmer** — scoped to the
programmer, not to the full merge. The question this answers is: when the
programmer stops holding a value for an attribute, what does the output do?

Behaviour splits by the attribute's **effective merge rule** (§6.2, §11.4):

| Merge rule | On release from the programmer |
|------------|-------------------------------|
| **HTP** — intensity/dimmer, and any attribute configured HTP | **always returns to the profile default.** Not optional. |
| **LTP** — everything else | **stays in the output** at its last value, unless the option below is enabled |

So clearing the programmer drops the intensity out but leaves position and
colour where they were — which is the behaviour people expect, because an LTP
value persisting until something else claims it *is* what LTP means.

**The setting** — ChamSys's `Unused channels return to default`:

| State | Effect on LTP attributes released from the programmer |
|-------|------------------------------------------------------|
| **Off** (default) | hold last value — position stays, colour stays |
| **On** | return to profile default — position homes, colour opens |

HTP is unaffected by the setting either way.

- Playbacks are unaffected by all of this. An attribute a playback is driving
  is being driven; release only governs what happens where nothing claims the
  value. This dovetails with §11.3's per-playback release modes, where `Hold
  last` is the same idea applied to an executor.
- `[want]` Per-attribute-group override on the setting (§6), since "position
  returns to default but colour holds" is a plausible want. Secondary to the
  global switch.
- The engine already has the default-fallback path: `renderDmx()` falls back to
  `ch.defaultValue`. What it lacks is the hold-last behaviour for LTP, which
  needs a retained last-output value per attribute.

### 11.10 Fixture sheet — output and source inspection

The sheet is not a value display; it is a **merge-stack inspector**. Neither
reference console exposes this properly — MA3 colours values by source type but
cannot show the full contributing stack or mute one contributor.

#### Value display

Per fixture, per attribute:

| Column | Shows |
|--------|-------|
| **Total** | the final output value — what the fixture is actually doing |
| **Absolute** | the value in its display unit (§2.6.1) |
| **Relative** | the delta from the underlying base — what a phaser or fan is adding (`+14%`) |
| **Source** | which contributor won, and why |
| **Stack** | every contributor, in priority order, with what each offered |

- `[want]` Relative mode also offers *relative to default*, since "how far from
  home is this" is a different useful question from "what is the FX adding".
- `[want]` **Tracked values show their origin** — `72% tracked from Cue 12`.
  §11.1's tracking resolution already walks back to the last cue that set each
  attribute, so the origin is known at no extra cost.

#### Source identification

- `[want]` Each source type carries **a symbol and a colour, not colour alone**:
  programmer · cue · tracked · FX/phaser · extraction layer (§13.3) · highlight
  (§20.7) · default · hold-last (§11.9). Colour alone fails in a dark venue
  across mixed screens, and it is the main readability weakness of the existing
  consoles.
- `[want]` An always-available legend, and §7.1's three-layer identity for the
  attribute itself.

#### Removing a contributing source

The purpose of the stack view is **troubleshooting unwanted values**: find what
is sending, then remove it for real. So each entry in the stack is actionable,
and the action offered depends on what kind of source it is:

| Source in the stack | Removal action |
|---------------------|----------------|
| **Programmer** | clear that attribute from the programmer (§11.8) |
| **Playback / cue** | surgical mask, or `Off` the executor — see below |
| **FX / phaser** | remove that phaser from the programmer or cue |
| **Tracked value** | two options: jump to the originating cue and edit it there, or `Block` at the current cue (§11.1) so it stops tracking in |
| **Extraction layer** | `Off` that layer (§13.3) |
| **Hold-last / stuck** | release to default (§11.9) — this is the "values sticking after a clear" case |
| **Highlight** | turn highlight off (§20.7) |
| **Default** | nothing to remove; it is the floor |

- `[want]` One gesture on a stack entry removes it. The sheet is live, so if
  something immediately re-sends — a running playback re-asserting, a cue
  re-tracking — you see it reappear, and *that* is diagnostic information in
  itself.

#### Jump to source

`[want]` A second action on every stack entry: **go to the thing that is
sending**, landing with the cursor on the exact cell rather than just opening a
window (§2.7).

| Source | Jumps to |
|--------|----------|
| **Programmer** | the programmer, cursor on that attribute |
| **Playback / cue** | that cue in its sequence sheet, on that fixture's cell |
| **FX / phaser** | the recipe editor, at that slot (§4.4) |
| **Tracked value** | the **originating** cue, on that cell |
| **Preset reference** | that preset in its pool editor (§11.8) |
| **Extraction layer** | that layer in the pixel mapper (§13.4) |
| **Default** | the fixture profile's table view, at that channel (§1.2) |
| **Highlight** | the highlight state for that fixture type (§20.7) |
| **Hold-last / stuck** | nothing to edit — residue, not an object. Release only. |

Two of these are quietly valuable: jumping to a **profile default** is how you
fix a fixture type that homes wrong, and jumping to a **preset** is how you fix
a colour everywhere at once instead of per cue.

- Per §10.1 this is sustained work, so it uses **Docked** presentation — the
  real window, raised. Not a popup.
- `[want]` The inverse direction too: **Find usages** — "which cues use this
  preset", "what else drives this attribute". Same query engine as §11.10's
  filters, read the other way. Goto Source walks upstream, Find Usages
  downstream.
- `[flag]` **Tracked values are the only non-local case.** Their source is an
  earlier cue, so removal either edits that cue (affects everything downstream)
  or blocks here (local, leaves the origin alone). The sheet must state which
  it is about to do rather than picking silently.

#### Bulk actions

The same removals at cell-selection, column or row scope:

| Action | Effect |
|--------|--------|
| **Clear from programmer** | removes the programmer's contribution, where it has one |
| **Off sources** | stops playbacks contributing to those cells |
| **Return to default** | both of the above, then force the profile default |

- `[flag]` **"Off playbacks sending to them" is ambiguous in scope**, because an
  executor also drives other fixtures. Both forms are needed and must be
  distinct:
  - **Surgical** — mask that executor's contribution to *these attributes only*.
    Nothing else on stage changes. This should be the default.
  - **Blunt** — `Off` the whole executor. Faster, and affects everything it
    drives, so it asks first.
- Per §10.1: a few actions on a cell is a **radial**; choosing among many
  contributing sources is a **list panel**. The rule already decides which.

#### Mute — a separate, temporary thing

`[want]` Distinct from removal and clearly labelled as such: switch a
contributor off *without changing it*, to see what the output would be without
it. Non-destructive, reverts when cleared, useful for "is this cue the problem?"
before committing to a removal.

- `[flag]` A forgotten mute produces a fixture that is dark for no visible
  reason. So mutes are indicated on the fixture, on the sheet and in a global
  status line, there is a one-press **Clear all mutes**, and they are never
  stored in a cue or show file.
- Removal is the primary mechanism; mute is the look-before-you-act companion.

#### Selection, sorting, filtering, search

- `[want]` **Current selection shown at the top**, in selection order (§20.4),
  clickable to deselect or reorder.
- Sort, filter and search by: name · fixture ID · UID · tags · groups · values ·
  attributes · presets · cues · sequences · playbacks.
- `[flag]` **One query engine, three consumers.** These filters, §20.8's dynamic
  groups and §7.5's preset facets are the same mechanism. Building it once also
  delivers §16.3's introspection and §16.7's diagnostics — "which cues use this
  preset", "which fixtures have no position stored" are this query surface with
  a different presentation.
- `[want]` Value-based filters are the diagnostic win: *show only attributes
  driven by the programmer*, *only tracked values*, *only values above 50%*,
  *only cells with more than one contributor*.

#### Engine impact

- `[flag]` **Provenance is the real cost.** `Engine::tick()` merges into a flat
  `ValueLayer` that remembers nothing about origin. Retaining the full
  contributing stack for every attribute of every fixture is far too expensive
  at 44 Hz — 2000 fixtures × 20 attributes × N sources.
  So provenance is **computed on demand, for the fixtures currently visible in
  a sheet that has it enabled**. The normal merge path stays flat and fast; the
  inspector requests a traced merge for a subset.
- `[flag]` Source mutes are a sparse override set consulted during merge. Cheap
  when empty, which is the normal case.

---

## 12. Learn — remote control bindings

MadMapper's binding flow: pick a target, press Learn, move the control, done.
No dialog, no addresses typed. Applied across the whole app rather than to one
window.

### 12.1 Flow

1. Enter Learn (hard button, radial action, or `Learn On`).
2. Every bindable target highlights; bound ones show their source as a badge
   (`CC 7`, `/lumen/exec/1`, `U2.14`).
3. Focus a target, send the input. Bound.
   - `[done]` Learn pins the MIDI channel it saw — it has to, or a
     two-channel controller's CC 7 collides with itself — but the badge shows
     the channel only when it is not channel 1, so the common case reads
     "CC 7" rather than "CC 7 ch1".
   - `[done]` Re-learning the same target MOVES its binding rather than
     stacking a second one. The same input on a DIFFERENT target warns and
     names what it would join; allowed on confirm, since one input driving
     several things is sometimes the point.
4. `[want]` **Learn next** auto-advances to the adjacent target, so mapping
   eight faders is eight fader moves rather than eight separate operations.
5. `[want]` Unlearn from the same radial, and `Learn Clear` for a whole set.

### 12.2 Targets — anything

- Faders, encoders, buttons, tiles (§9.3), masters
- Options, toggles, dropdowns — any setting in the app
- Presets, groups, executors, FX recipes, panel sets, filters
- Radial sectors and menu nodes (§5.5)
- **A command string**, which is the general escape hatch: any input can fire
  anything typeable, consistent with §8.4

### 12.3 Sources

| Source | Addressing | Notes |
|--------|-----------|-------|
| **MIDI** | note, CC, 14-bit CC, program change, pitch bend | plus MIDI clock as a chase rate source (§11.5), MTC as timecode, MSC for theatre |
| **OSC** | address pattern + argument index | bidirectional |
| **DMX in** | universe + channel, over Art-Net, sACN or a DMX interface | remote control from another console |
| **Keyboard** | key + modifiers | §2.8 |

`[done-decision]` **GPI / contact closure is deferred.** House lights, fire
alarm and other interlocks come in over a DMX-in channel for now — a DMX
channel crossing a threshold is a binding like any other, with the deadband
from §12.4 doing the debounce.

- `[want]` Threshold mode on a DMX-in binding: fire on rising edge, falling
  edge, or while above a level. This is what makes a DMX channel serve as a
  contact closure without a separate mechanism.
- `[later]` GPI as a source slots into this table when the hardware is chosen;
  nothing above needs to change to accommodate it.

### 12.4 Mapping parameters

Per binding, because a fader and a trigger need different handling:

| Parameter | Options |
|-----------|---------|
| Input range | min / max, invert |
| Output range | min / max |
| Curve | linear · log · exponential · S-curve |
| Mode | absolute · relative (endless encoders) · toggle · momentary · trigger |
| Deadband / smoothing | for noisy DMX or analogue input |
| Feedback | off · on (send value back) |

`[done]` **Catch is the implemented default**, and a control in catch mode
reports which direction to move. Crossing the held value in a single jump
counts as catching, so a fader flicked past the value does not stay dead
because no event landed exactly on it; a fader already parked on the value is
already in control. Takeover state resets when the page changes or a set
activates, because a fader caught on the outgoing page has not been caught on
the incoming one. It is deliberately NOT persisted: a reloaded show has not
had its faders touched.

`[done]` **Scale** anchors on the first touch and changes nothing by itself,
then maps the control's remaining travel to the remaining range in whichever
direction it moves — reversible, and it reaches both ends exactly.

**`[flag]` Fader takeover is the real problem here.** If software holds 50% and
the physical fader sits at 0%, touching it snaps the value. Three modes needed,
per binding:

- **Jump** — value follows immediately. Predictable, visibly ugly.
- **Scale** — remaining travel scales to the remaining range.
- **Catch** (soft takeover) — the control does nothing until it passes through
  the current value, then takes over. Safest for show use; the default.

`[want]` A bound control in catch mode shows which direction to move to pick
the value up, or the operator is left guessing why the fader is dead.

### 12.5 Feedback

Bidirectional bindings, so controller surfaces stay in sync:

- MIDI — LED buttons, LED rings, motorised faders
- OSC — send current value to an address, for tablet surfaces
- DMX out — drive an indicator on another device
- `[want]` Feedback sends on state change and on binding-set activation, so a
  controller's LEDs are correct the moment a set loads rather than after the
  first touch.

### 12.6 Binding sets and layering

- `[want]` Named **binding sets** stored in the show and swappable
  (`APC40 busking`, `Launchpad show`, `Tablet OSC`), bindable to a hard button.
- `[want]` **Layering**: global bindings plus page-local bindings, with
  page-local winning. One Launchpad then means different things per executor
  page (§9.1) without remapping.
- `[want]` Enable/disable a whole set without deleting it.
- `[want]` Conflict detection: binding one input to a second target warns and
  shows the existing binding. Allowed on confirm — one input driving several
  things is sometimes the point.
- `[want]` Export/import a set independently of the show, so a controller map
  moves between shows.

### 12.7 Engine impact

- `[flag]` Input handling must not run on the 44 Hz output thread. Inputs land
  on a queue; the engine drains it at the top of `tick()` under the existing
  mutex, so a flood of MIDI cannot stall DMX output.
- `[flag]` DMX **input** is new — the engine currently only transmits.
  `Output.hpp` needs a receive counterpart: sACN and Art-Net listeners with
  source tracking, since two senders on one universe need priority handling.

---

## 13. Pixel and video

### 13.1 Two domains, one boundary

`[flag]` **The `(FixtureId, Attr, float)` abstraction does not scale to video
pixels.** A moving head has ~20 attributes. A 1920×1080 surface has 2 million
pixels; writing those through a hash map at 44 Hz is roughly 6 million float
writes per frame. So video is not "lots of fixtures" — it is a second domain
with its own data path, joined to the first at one defined point.

| Domain | Size | Carrier | Who writes it |
|--------|------|---------|---------------|
| **Control** | sparse, ~thousands | `(FixtureId, Attr, float)` | cues, programmer, FX, Learn |
| **Raster** | dense, millions | GPU textures | the render pipeline |

**The boundary is pixel extraction**, and it runs one way:

```
cues / FX / programmer  →  surface control attributes  →  GPU render
                                                            ↓
                              sampled at fixture sample points
                                                            ↓
                                      sparse colour values  →  merge  →  DMX
```

Control drives raster *parameters*. Raster is sampled back into sparse colour
values. Raster data never enters the attribute system.

### 13.2 Surfaces are fixtures; pixels are not

A **surface** patches like a fixture and carries control attributes, so
everything already specced works on it unchanged:

- `PosX/Y/Z`, `RotX/Y/Z`, `ScaleX/Y` — Position group, `axis` kind (§6.2)
- `Opacity` — Intensity group
- `BlendMode`, `MediaSelect`, `PlaySpeed` — Beam group
- `Shader1..n` — N exposed shader uniforms, Beam group

**This is where §4.2's role binding pays off.** One Circle recipe with slots on
`AxisX`/`AxisY` orbits a moving head, a laser zone *and* a video surface, with
no video-specific FX library.

A **mapped LED pixel** is not a new object — it is an ordinary patched fixture
(the `Pixel RGB` type in the default library) that additionally carries a
**sample point** in mapping space. Extraction samples the composited raster at
that point and writes the fixture's `Red`/`Green`/`Blue`.

- `[want]` Sample point placement: per fixture, per cell on multi-instance
  fixtures (§1.3), so a Pixel Curve 12 gets twelve sample points from one
  patch entry.
- `[want]` Sample area, not just a point: single pixel · box average · gaussian.
  Averaging is what stops LED output flickering on detailed content.
- `[want]` Layout tools — place fixtures in a grid, along a line, along a
  bezier, or import positions from an MVR / 3D stage file.

### 13.3 Extraction as a merge layer

`[want]` Extraction output enters the merge pipeline as **a playback layer with
a priority**, not as a direct write to DMX. So it participates in §11.4's
priority, merge rule and swap groups exactly like an executor.

This is the thing neither reference tool can do: HTP a video look *over* a
conventional colour wash, or let a cue take priority over mapped content
without leaving the console.

- `[want]` Per-extraction-layer merge rule and priority.
- `[want]` An intensity master on the extraction layer, so mapped content
  fades like any playback.
- `[open]` Should extraction be storable into cues as a *state* (which media,
  which opacity) — yes — and should the sampled colours themselves ever be
  storable as a snapshot? Leaning no: that would bake 2 million values into a
  show file.

### 13.4 Composition

- Layer stack with blend modes, opacity, layer-level FX.
- Per-layer media or generative source.
- `[want]` Layer parameters exposed as control attributes, so layers are
  cueable and FX-drivable like surfaces.
- Masks per surface, with feathering.
- `[want]` Edge blending for multi-projector.

### 13.5 Content

**Media** — video files, stills, image sequences. Playback control (play,
pause, speed, loop mode, scrub, in/out points) as control attributes.

- `[flag]` **Codec choice matters for realtime.** H.264 decode is CPU-bound and
  will not hold 60 fps across several layers. HAP (GPU-decoded, DXT) is the
  production standard for this and should be the recommended format, with
  H.264/HEVC supported but flagged as not frame-rate-safe at scale.

**Generative** — shader-based content.

- `[want]` **ISF** (Interactive Shader Format) support, which is what MadMapper
  uses, so an existing shader library is usable rather than needing new
  authoring.
- ISF declares its own parameters, so `[want]` those map automatically onto
  `Shader1..n` control attributes — meaning any ISF shader becomes cueable and
  FX-drivable with no per-shader work.

### 13.6 Inputs and outputs

| Direction | Transports |
|-----------|-----------|
| **In** | NDI · Syphon (macOS) · Spout (Windows) · capture device · screen capture |
| **Out** | display/projector · NDI · Syphon / Spout · **pixel extraction to DMX** |

`[done-decision]` **Target platforms: macOS, Windows, Linux.**

Frame sharing is the one genuinely platform-split piece:

| Platform | Local frame sharing |
|----------|--------------------|
| macOS | Syphon |
| Windows | Spout |
| Linux | **no equivalent exists** — NDI, or DMA-BUF for a native path |

- `[want]` All of these behind one `FrameShare` interface, with NDI as the
  always-available fallback and the only option on Linux.
- `[flag]` The NDI SDK is free but redistribution is governed by its own
  licence terms — needs checking before it ships, same category of issue as the
  ChamSys library in §1.1.

### 13.6.1 GPU API

`[done-decision]` **Recommend bgfx**, with the renderer behind a thin internal
interface so the choice stays reversible.

Why not the obvious alternatives:

| Option | Problem |
|--------|---------|
| OpenGL 4.x only | Apple froze GL at 4.1 and deprecated it in 2018 — **no compute shaders on macOS** (those arrived in 4.3) and no future. Fine for a prototype, a dead end for a shipped product. |
| Vulkan + MoltenVK | One codebase, works everywhere, but far more code for every operation and MoltenVK adds a translation layer on the platform that needs it most. |
| Native Metal + D3D12 + Vulkan | Best performance, roughly triple the renderer work. |
| Dawn / WebGPU | Modern and clean, maps well to all three; younger ecosystem, and WGSL complicates the ISF story below. |

**bgfx** gives native backends per platform (Metal on macOS, so no deprecation
exposure; D3D12/Vulkan/GL on Windows; Vulkan/GL on Linux), is mature and
permissively licensed, and absorbs the per-platform plumbing that would
otherwise be months of work before a single pixel renders.

`[flag]` **ISF is GLSL**, so any non-GL backend needs a shader translation
step: GLSL → SPIR-V → MSL/HLSL via `glslang` + `SPIRV-Cross`. Well-trodden, and
bgfx already carries a shader toolchain — but it means ISF shaders are
*compiled* on import rather than run as source, so the import step needs to
surface compilation errors usefully.

One side benefit of §2.2's custom window manager: because the UI is drawn by
Lumen rather than an OS toolkit, cross-platform UI work is mostly already
accounted for. The platform-specific surface shrinks to windowing, audio, MIDI,
networking and frame sharing.

### 13.7 Projection mapping

Surface types: quad · triangle · circle · mesh/grid warp · bezier · 3D model.
Corner pinning, mesh warp for curved surfaces, keystone, projector calibration.

- `[later]` See §13.9 — this is separable from pixel mapping and much larger.

### 13.8 Engine impact

- `[flag]` **Two clocks.** Video runs at 30/60 fps, DMX at 44 Hz. The render
  thread and the output thread must be decoupled: extraction samples the most
  recent *completed* frame and never blocks `tick()`. Latest-frame-wins
  snapshot, no waiting.
- `[flag]` The render pipeline is its own thread with its own GPU context.
  `Engine::tick()` gains an extraction-layer merge step but must not touch GPU
  state.
- `[flag]` Sample-point lookup wants a spatial index, not a per-fixture loop
  over surfaces, once fixture counts reach the thousands.

**`[flag]` Extraction readback is the performance trap, and it has a good fix.**

The naive version reads the whole composited frame back from GPU to CPU every
DMX frame and samples it there. At 1920×1080 that is ~8 MB per frame, 44 times
a second, and a synchronous readback stalls the render pipeline.

The fix: **a gather pass.** Render a tiny extraction target where each output
pixel *is* one sample point — a shader that reads the composite at each sample
position and writes one texel per sample. For 2000 mapped LED pixels that is a
2000-texel texture, not 2 million.

```
composite (1920×1080)  →  gather shader  →  extraction target (N texels)
                                                     ↓  async readback
                                           sparse colour values  →  merge layer
```

- Readback must be **asynchronous** — PBO in GL, staging buffer plus fence in
  Vulkan/Metal, `blit` + `readTexture` in bgfx — accepting 1–2 frames of
  latency rather than stalling. At 60 fps that is 17–33 ms, below the threshold
  where it matters for lighting.
- Sample *area* modes (§13.2) become taps in the gather shader, so box average
  and gaussian cost nothing extra on the CPU side.
- `[want]` Sample point positions live in a GPU buffer updated only when the
  patch or mapping changes, not per frame.

### 13.9 Staging — the honest version

`[flag]` This is the largest subsystem in the spec, and the two halves are
separable with very different cost-to-value:

**Stage 1 — pixel mapping to LED.** A simple composited raster (media + ISF
layers, 2D surfaces only), fixture sample points, extraction as a merge layer.
This is ChamSys pixel-mapper parity plus the merge-layer advantage, and it is
most of the practical value for a lighting rig.

**Stage 2 — projection mapping.** Warping, mesh deformation, edge blending,
projector calibration, 3D model mapping. This is where MadMapper's real
difficulty lives, and none of Stage 1 depends on it.

Recommend building Stage 1 to completion first. Nothing in Stage 1 needs
reworking to add Stage 2, because both sit behind the same surface-as-fixture
and extraction-layer interfaces.

---

## 14. Laser

### 14.1 A third domain

Laser is neither attributes nor a raster. A projector consumes a **point
stream** — `(X, Y, R, G, B)` samples at 20,000–40,000 points per second — and
the galvanometers trace a path that persistence of vision resolves into a
figure. Three clocks now exist in the app:

| Domain | Rate | Carrier |
|--------|------|---------|
| Control | 44 Hz | `(FixtureId, Attr, float)` |
| Raster | 30–60 fps | GPU textures |
| **Point stream** | **20–40 kpps** | ILDA point buffers |

Same boundary pattern as §13.1: control drives *parameters*, a dedicated
pipeline produces the stream. Point data never enters the attribute system.

### 14.2 Zones are fixtures

A **laser zone** patches like a fixture, exactly as a video surface does:

- `PosX/PosY`, `RotZ`, `ScaleX/ScaleY` — Position group, `axis` kind
- `Opacity` / `Beam intensity` — Intensity group
- `ColorR/G/B`, `ColorMacro` — Color group
- `ContentSelect`, `PlaySpeed`, `PointRate`, `FxParam1..n` — Beam group
- `Arm` — Control group, with §6.1.1's hold-to-fire and confirm behaviour

**§4.2's role binding pays off a third time:** the same Circle recipe that
orbits a moving head and a video surface drives a laser zone's `AxisX`/`AxisY`,
because all three resolve the same roles.

### 14.3 Output

| DAC | Transport | Status |
|-----|-----------|--------|
| **Helios** | USB | open protocol, simple — good first target |
| **Ether Dream** | Ethernet | open, well documented — good first target |
| **Pangolin Beyond** | network API | drives Beyond rather than the hardware |
| ILDA DB25 | analog, via a DAC | the physical standard behind all of them |
| `[later]` | LaserCube, Moncha, FB4 | each its own protocol |

- `[want]` One `LaserOutput` interface, DACs behind it, mirroring §3's
  `IOutput` pattern for DMX.
- Start with Helios and Ether Dream, since both are documented and the user has
  the hardware.

### 14.4 Point pipeline

`[flag]` **This is the genuinely hard part of laser software**, and it has no
analogue in DMX or video. Galvos have finite acceleration; feeding them an
unoptimised path produces distorted figures and can damage the scanners.

The pipeline, per zone, per frame:

1. **Generate** — content produces a path (points, with blanking).
2. **Transform** — zone position, scale, rotation, geometry correction.
3. **Optimise** — the hard stage:
   - **Corner dwell** — extra points at sharp angles so the galvo arrives
     before the beam moves on
   - **Blanking dwell** — points at the start and end of a blanked move, so the
     beam is off before travel and on only after arrival
   - **Travel interpolation** — long moves split into steps under the maximum
     galvo velocity
   - **Point-rate budgeting** — total points across all zones must fit the
     DAC's pps, or the frame rate drops and the figure flickers

`[done]` Stages 1–4 are built and the optimiser is **stable under re-entry**,
which matters because this section leaves per-zone versus per-projector tuning
open and chained passes are therefore plausible. Two things made it unstable
and are fixed: a leg sitting exactly on the step limit measures a hair above it
in floats and was split in half on every pass, and blanking dwell was re-added
at transitions that already had it. Corner dwell wraps a **closed** figure's
seam, or a square gets dwell on three corners and a visibly distorted fourth.
Decimation preserves every blanking transition — dropping one turns a blanked
move into a visible line, which is a safety matter and not a cosmetic one. A
budget overrun is a **fault**, not a truncation: a half-drawn figure at the
wrong rate is a flickering beam.
4. **Colour shift correction** — colour channels lag the galvos; a per-projector
   sample offset compensates.
5. **Safety gate** (§14.6) — last, always.

- `[want]` Per-projector calibration: keystone, barrel/pincushion, size,
  offset, invert per axis, max velocity, colour shift.
- `[open]` Optimiser tuning exposed per zone, or per projector only? Per zone
  is more control and more ways to get it wrong.

### 14.5 Content

- **ILDA frame files** (`.ild`) — the interchange format for graphics and
  animation.
- **Generated beam effects** — fans, tunnels, cones, sheets, scans. Parametric,
  so their parameters are control attributes and therefore cueable and
  FX-drivable.
- **Text and vector graphics**, converted to paths.
- `[want]` A content library organised like the preset pools (§7), with the
  same sorting, filtering and UID referencing.
- `[open]` Convert video/raster content to laser paths (edge detection)? A real
  MadMapper-adjacent feature, but a large one and separable.

### 14.6 Safety — architecturally downstream

`[flag]` **The safety gate is the last stage of the output path and nothing
upstream can bypass it.** Not a cue, not an FX recipe, not a Learn binding, not
a macro. This is a structural statement, not a policy one: if safety limits
were merged alongside ordinary values they could be overridden by priority
(§11.4), and the whole point is that they cannot be.

`[done]` **Enforced by the type system, not by discipline.** A DAC accepts only
a `GatedFrame`, and `GatedFrame` has no public constructor — `SafetyGate` is
the only thing that can make one. Handing raw points to a projector is a
COMPILE ERROR, which is the only version of this rule that survives contact
with future code. Four bypass routes are checked: default-constructing a frame,
passing a path where a frame is expected, mutating an already-gated frame's
points, and inheriting a way in. All four are rejected by the compiler, and the
test suite asserts the type traits so the guarantee cannot be quietly removed.

`[done]` The gate's own state is equally unreachable from the value system.
`Arm` exists as a Control attribute so the state is visible and storable like
anything else, but the armed flag lives in the gate and only an explicit
hold-to-fire act sets it. Tested with an Arm value written straight into the
programmer, stored in a cue, asserted by a running playback and present at full
value in the merged output — and the zone stays dark.

Required behaviours:

- `[done]` **Scan-fail blanking.** A stationary beam at full power is the
  dangerous case. Blanks on point-stream stall, DAC buffer underrun, DAC
  disconnect, render thread stall, loss of galvo motion and point-rate
  overrun. Every fault also **disarms**, so recovery needs the fault cleared
  AND a deliberate re-arm: a beam returning on its own the moment a cable is
  reseated is exactly what must not happen. The stall is detected by a sweep
  rather than by arrival, because nothing will arrive to trigger it; and it is
  measured on frames ARRIVING, not on frames being allowed through, so a
  disarmed zone being fed is not reported as stalled.
- `[done]` **Per-zone masks** with hard geometric limits. A point outside the
  mask is **blanked, not clamped** — clamping would draw a line along the mask
  boundary, which is content nobody asked for. The geometry is left untouched;
  only the beam state changes.
- `[done]` **Per-zone maximum power / attenuation**, applied after all merging,
  and clamped rather than trusted. Zero power is blanking, not a very dim beam.
- `[done]` **Audience-scanning zones** flagged distinctly, with independent
  attenuation that **multiplies** rather than competes — so the normal
  intensity path can only ever lower it, never raise it. An audience zone also
  needs an explicit opt-in: armed, masked and at full power it still emits
  nothing without one, so audience scanning is never an ordinary parameter.
- `[done]` **Emergency stop** on its own path, independent of playback,
  blackout and grand master. A **latch**: it cannot be armed around, and it
  disarms as well as blanks, so clearing the latch alone cannot restore a beam
  that was live when the button was hit. Recovery is two deliberate acts.
- `[done]` **Fail-safe defaults** — one `failSafe()` call covering startup,
  show load, error, panel-set change and unhandled exception, so a new call
  site cannot implement half of it.
- `[done]` **Arm as a deliberate act** — a zone starts disarmed; arming is
  hold-to-fire with confirmation (§6.1.1). Confirming without beginning, or
  before the hold elapses, arms nothing: a fumbled double-press must not arm a
  laser. A zone with an unresolved fault cannot arm at all.

`[want]` A safety status panel showing, per projector: armed state, mask
active, attenuation, scan-fail status, DAC link, point rate vs budget. Visible
state, not buried.

**Standards the operator will need** — named so they can be looked up, not as
legal advice, and compliance remains the operator's responsibility:
IEC/EN 60825-1 (laser product safety), ANSI Z136.10 and FDA 21 CFR 1040.11
(US, where audience scanning needs a variance), and in Germany
DGUV Vorschrift 11 / DIN 56912 for show lasers.

- `[want]` The software should make compliant operation the easy path — masks
  and attenuation on by default, audience zones requiring explicit opt-in — and
  should never present audience scanning as an ordinary parameter.

### 14.7 Engine impact

- `[flag]` **A third thread with the tightest timing in the app.** The DAC
  consumes points at a fixed rate; underrun shows as visible flicker and must
  trigger blanking. This thread needs higher priority than the 44 Hz DMX
  output and must never block on it.
- `[flag]` Point generation is CPU-side and per zone, so it parallelises across
  zones but the optimiser is sequential within a zone.
- `[flag]` The safety gate runs on this thread, after everything else, so a
  stall anywhere upstream still results in blanking rather than a held beam.

### 14.8 Staging

1. **Output and safety first.** `[done]` point stream, safety gate, emergency
   stop, scan-fail blanking, arm/disarm, masks, attenuation, audience opt-in,
   fail-safe defaults and the status panel's data. The Helios and Ether Dream
   **transports** remain: both need the hardware to be worth writing, and
   `ILaserOutput` cannot be handed anything but a gated frame, so adding one
   cannot weaken the gate.
2. **Zones as fixtures** — patch, control attributes, cueable, FX-drivable.
3. **Point optimiser** — `[done]` corner dwell, blanking dwell, travel
   interpolation, colour-shift correction, budgeting and decimation.
4. **Content** — ILDA files, then generated beam effects.
5. `[later]` Raster-to-path conversion, advanced calibration.

Deliberately inverted from the other subsystems: safety and output come before
content, because a laser pipeline that renders beautifully and blanks
unreliably is worse than one that does nothing.

---


### 14.6.1 Where the gate lives

`[done-decision]` **The engine owns the safety gate.** The question was left
open while laser zone tiles were being built (§9.3.1) precisely so it would
not be decided as a side effect of drawing a surface. Decided on its own
terms, it splits cleanly:

- A **zone** is show data — a mask, a power ceiling, an audience-scanning
  flag and its opt-in. These are the numbers a venue signed off on, and
  re-entering them from memory at the next load is how they get entered
  wrong. So they persist, which means the engine has to own them.
- **Arming** is not show data. It never persists and always starts off,
  because the safe state is the one a file restores you to. A show that could
  reopen with a beam live would make opening a file something you had to be
  ready for. `restoreZone` refuses to bring arming back whatever the record
  says, and `failSafe("show loaded")` runs again after the swap — two belts,
  because there is no sensible reason to have one.

Owning it there does **not** weaken §14.6. `GatedFrame`'s constructor is still
private and `SafetyGate` is still its only friend, so bypassing the gate is a
compile error wherever the gate happens to live.

What it *does* change is reach: anything holding an `Engine&` can now call the
arm path. So the arm path carries its own guard.

#### Arming names the physical act that caused it

`[done-decision]` `beginArm`/`confirmArm` take an `ArmSource`, and the enum
has no value a cue, macro, binding or agent could honestly pass — there is no
`Command`, no `Playback`, no `Script`. Only `Panel`, `Pointer` and
`KeySwitch`. Arming from a cue would require writing a lie, in one place, in
full view. That is §14.6's line held at the one boundary where the code can
still tell the difference.

A hold is **one act from start to finish**: `confirmArm` refuses if the source
does not match the one that began the hold. Beginning on a panel key and
finishing from an on-screen control is two halves of two different gestures,
and accepting it would let either half be synthesised.

The source is recorded on the zone, so "who armed this" is answerable after
the fact.

#### What the command layer gets, and what it does not

| Command | Why |
|---|---|
| `Disarm [zone]` | the safe direction should be as reachable as the console can make it |
| `EmergencyStop [stop\|reset]` | likewise; reset also disarms, so recovery cannot silently restore a beam |
| *(no `Arm`)* | a command for it would be reachable from a cue, a macro, a binding and the agent — exactly the single-press path §14.6 exists to prevent |

A laser zone **tile** follows the same rule: it shows state and offers
`Disarm` when armed, and nothing when disarmed. A tile that armed on a tap
would be the single press the whole design is built to prevent.

`[flag]` Found while wiring this up: **zones had no UID at all.** `addZone`
never minted one, so every zone shared the null identity — nothing could
reference a zone durably (§8.1), and two zones collided on the primary key the
moment anyone tried to save them. The gate now takes the show's `UidGen` like
every other store. Worth recording because the bug was invisible until
something tried to persist a second zone, which is the kind of thing that
would otherwise have surfaced on a show file somebody cared about.

## 15. Decision record — language and concurrency

### 15.1 Language: C++ (decided 2026-10-20)

`[done-decision]` **C++20.** Rust was considered seriously and rejected for
reasons specific to this project's dependency graph, not for any general
preference.

**What Rust would have bought.** The hardest bugs in this app will be
concurrency bugs. Four threads with hard timing now exist (§15.2), and Rust's
`Send`/`Sync` would catch most cross-thread mistakes at compile time. Cargo is
also materially better than CMake for iteration speed.

**Why C++ wins anyway.** This project is mostly integration, and nearly
everything it integrates with is C or C++:

bgfx · NDI SDK · Syphon (Objective-C) · Spout (C++/Win32) · HAP decode ·
glslang · SPIRV-Cross · Helios DAC SDK · Ether Dream · RtMidi · ffmpeg ·
Pangolin Beyond (Windows DLL)

In Rust that is FFI at almost every external boundary — concentrating `unsafe`
exactly where the dangerous code already lives, plus permanent binding
maintenance. Full migration cost, diluted safety benefit, because the safe core
is not where the bugs are.

Dear ImGui is also well suited to §2's dense custom-drawn console UI, with far
more precedent in professional tools of this shape than egui has.

**The honest counter-case.** `wgpu` is a first-class pure-Rust renderer and
arguably nicer than bgfx — but ISF is GLSL and naga's GLSL frontend is only
partial, so the shader pipeline (§13.6.1) would be a fight. NDI, Syphon, Spout,
HAP and the DAC SDKs stay FFI regardless.

**What would reverse this decision:** dropping video and projection mapping
entirely. A DMX-plus-laser console has almost no C++ dependency surface, and
Rust would then be the better choice. Revisit if §13 is ever cut.

### 15.2 Concurrency architecture

Rust's real advantage was concurrency discipline. That is obtainable in C++
architecturally, and the spec needs it regardless. Four threads:

| Thread | Rate | Owns |
|--------|------|------|
| **Control / UI** | input-driven | patch, cues, programmer, UI state |
| **DMX output** | 44 Hz | universe buffers, sACN/Art-Net sockets |
| **Render** | 30–60 fps | GPU context, composite, extraction gather |
| **Laser** | 20–40 kpps | point buffers, DAC link, safety gate |

Rules, adopted now rather than retrofitted:

1. **No shared mutable state.** Each thread owns its data outright. Cross-thread
   handoff is by value or by immutable snapshot, never by lock around a shared
   structure.
2. **Lock-free SPSC ring buffers** for thread-to-thread traffic. Input events,
   extraction results and point frames all move this way.
3. **Triple buffering** for state one thread publishes and another consumes
   (merged values → DMX; composite → extraction; zone params → laser).
4. **No allocation on the DMX or laser threads.** Buffers preallocated at
   patch time; the laser thread especially must never touch the allocator.
5. **The laser thread outranks the others** (§14.7) and never blocks on them.
   Its safety gate must still run if every other thread has stalled.

`[flag]` **Immediate engine change.** `Engine::startOutput()` currently holds a
`std::mutex` and copies `universes_` under it, with `tick()` running inside the
lock. That works with two threads and will not survive four. Replace with a
triple-buffer handoff — merged values published by the control thread, consumed
by the DMX thread — **before** the render or laser threads are written, not
after.

---

## 16. Scripting, plugins and AI assistance

### 16.1 Three tiers, deliberately separated

| Tier | What it does | Cost | Risk |
|------|--------------|------|------|
| **Operate** | emits command strings the console already understands | low — the layer exists | **high** — live output |
| **Configure** | authors data: menus, panel sets, bindings, filters, profiles, preset batches | low | low — reviewable before use |
| **Extend** | writes Lua plugins: new generators, windows, importers | medium | medium — sandboxed |

Conflating these is the mistake. Operating a live rig and writing a preset
generator need completely different guardrails.

`[flag]` **What "add features by prompt" can and cannot mean.** A running C++
binary cannot recompile itself, so prompts cannot alter the engine, add a GPU
render path, or write a DAC driver — those are development-time work (what this
spec is for). At runtime, "new features" means **plugins in an embedded
scripting language**, exactly as MA3's Lua plugins do. That covers a great deal
and should not be oversold as covering everything.

### 16.2 The command layer is the API

`[flag]` **Architectural rule, adopted now:** the UI calls the command layer,
never the model directly. Every operation — including ones with no sensible
text form — exists as a command. No UI-only actions.

This is already implied by §5.5, §8.4 and §12.2, but it has to be *enforced*
rather than assumed, because the moment one feature reaches around it, agents,
macros, plugins and tests all lose access to that feature.

Worth adopting even with no AI at all: it is what makes the app testable,
scriptable, macro-able and headless.

- `[want]` Commands carry structured metadata — name, parameters, types,
  destructive flag, required privilege — from one declaration. That metadata
  generates the parser, the Lua bindings, the agent's tool schema and the
  documentation. One source, four consumers.

#### 16.2.1 What enforcement caught

`[done-decision]` Building the browser console (§21.3) against this rule
immediately found two operations with **no command form at all**: selecting a
group, and applying a preset. Both existed in the model and both were reachable
from nowhere.

That is exactly the failure §16.2 predicts, and it is the argument for the rule
in miniature: the gap was invisible for as long as nothing had to go through
the front door. Each is now grammar:

- **`Group <n>[ + <n>…]`** is a *selector clause*, the same shape as `Fixture`:
  it may stand alone as a selection, or carry a command — `Group 1 At 50`.
  Each group enters the selection **as a group**, not as its members flattened
  in, because §20.8's tree is what multi-level fan reads. Abbreviates to `grp`
  and `g`. A missing or empty group is named and refused rather than quietly
  selecting nothing, which would look like it worked.
- **`Preset <pool>.<id>`**, or `Preset <name>` when exactly one pool holds that
  name (§8.2 — a collision lists the pools rather than guessing). It applies as
  a **reference, not a copy** (§11.8), resolved per fixture so a palette with
  fixture-specific entries and a generic wildcard behaves as one object. A bare
  number is refused: "which pool" is a real question. A preset that says nothing
  for the selected fixtures — a position palette on LED pars — is refused with
  that reason, and a *partial* apply reports how many values were skipped,
  because "it half worked" is usually a patching mistake worth seeing.
- **`ExecMaster <executor> <percent>`** (`xm`), found the same way when the
  executor surface (§9) needed a fader. The single most-used control on a
  console had no command form at all. Not undoable, because it is playback
  rather than an edit (§8.6).

The pattern is worth naming: **each gap was found by building a surface, not
by reading the code.** Nothing is missing until something has to go through
the front door.

### 16.3 Introspection

An agent or plugin cannot operate blind. A query API over the show model:

- Patch: fixtures, types, attributes available per fixture, addresses
- Pools: presets, groups, recipes — names, IDs, UIDs, tags
- Playback: sequences, cues, executors, current state
- Programmer: selection, active values
- `[want]` Queries are structured and paginated, never a show dump. A 2000-cue
  show cannot be handed over as context, and should not need to be.

### 16.4 Plugins

`[done-decision]` **Embedded Lua**, matching MA3 so existing knowledge
transfers.

Plugin capabilities:
- Run commands, query the model (§16.2, §16.3)
- Register new commands, radial menu actions, tile types
- Custom windows drawn through a restricted UI API
- Content and preset generators (§7.3's auto-stepper is a natural plugin)
- Importers and exporters

Sandbox:
- `[want]` No arbitrary filesystem or network access by default; both are
  explicit per-plugin permissions.
- `[want]` **Cannot bypass the laser safety gate (§14.6)** or raise
  attenuation. Non-negotiable, enforced at the gate rather than by policy.
- `[want]` Execution budget — a plugin that loops cannot stall the control
  thread, and never touches the DMX, render or laser threads (§15.2).

### 16.5 Agent operation and safety

`[flag]` **This is live show control, and an agent emitting commands can black
out a rig, strike lamps, corrupt a show, or fire a laser.** The guardrails are
architecture, not a warning dialog.

**Blind-first.** The agent works in Blind (§11.7) by default, so output is
unaffected while it works. The operator reviews and merges. The console already
has this mechanism; the agent just lives inside it.

**Propose, then apply.** Default mode is a reviewable batch — the agent emits a
command list, the operator sees it as a diff with a plain-language summary, and
approves or rejects. Direct execution is opt-in, per session, never the default.

**Privilege tiers.** Agent privilege is strictly below the operator's. Blocked
unless explicitly and separately enabled:

- Laser `Arm`, any laser output change, anything touching §14.6
- Emergency stop, blackout, grand master
- Control group — lamp strike/off, reset (§6.1.1)
- Delete operations, renumber (§7.4), show file overwrite
- Panel set and binding changes during a show

**Show-mode lockout.** `[want]` When a surface is in run mode (§9.4) or a show
flag is set, the agent drops to read-only and propose-only. No direct execution
path exists in that state — not "is discouraged", does not exist.

**Atomic and undoable.** `[want]` An agent batch applies as one transaction and
undoes as one. This requires a grouping undo system in the show model, which is
wanted regardless.

**Audit.** `[want]` Every agent action logged with the prompt that produced it,
the commands emitted, and the result. Reviewable and exportable — if a show
goes wrong, "what did it do and why" must be answerable.

### 16.6 Network, privacy, offline

`[flag]` **AI must be strictly additive. No core operation may depend on it.**
Many venues have no usable internet, and a console that degrades without a
network connection is not a console.

- `[want]` Explicit opt-in, with a clear statement of what leaves the machine.
  Show files can contain client names, venue details and schedules.
- `[want]` Context sent is the structured subset from §16.3, not the show file.
- `[want]` Visible indicator when the agent is enabled and when it is
  transmitting.
- `[want]` Fully functional with the feature disabled or unreachable — the UI
  shows it as unavailable, nothing else changes.
- `[open]` Local model support for air-gapped venues. Smaller models can
  plausibly handle command emission given a good tool schema (§16.2), which is
  a narrower task than open-ended reasoning.

### 16.7 Where it actually helps

The tedious tasks worth targeting, since this should be judged on these rather
than on novelty:

- Bulk patching from a plot or spreadsheet
- Generating stepped preset banks across many fixture types (§7.3)
- Building a radial menu or binding set from a description (§5.4, §12.6)
- Renaming and re-tagging pools consistently
- Finding things: "which cues use this preset", "what is unpatched",
  "which fixtures have no position stored"
- Diagnosing: "why is this fixture dark" — walking the merge stack (§11.4) and
  explaining the answer
- Converting fixture profiles and mapping unmapped attributes (§6.3)

That last pair is where an agent beats a macro, because the answer requires
reading state and reasoning about it rather than replaying steps.

### 16.8 Code structure this requires

All four are good practice independent of AI:

1. **Command layer as the only mutation path** (§16.2)
2. **Introspectable model** with structured queries (§16.3)
3. **Transactional, grouping undo** (§16.5)
4. **Headless core** — the engine runs and is fully driveable with no UI, which
   is also what makes it testable

- `[flag]` Point 4 affects the current code: `Engine` is already UI-free, which
  is correct, and must stay that way. The console (§3) must remain a *client*
  of the command layer, not a privileged path into the model.

---

## 17. Patch, stage and show data

### 17.1 Patch views

Same two-view principle as the fixture editor (§1.2) — one model, two surfaces:

- **Patch sheet** — a table, spreadsheet-fast. Columns: fixture ID, name, type,
  mode, universe, address, footprint, position, tags. Multi-select, fill-down,
  paste from a spreadsheet, bulk edit in place.
- **Layout view** — fixtures placed in 2D/3D space. Used for selection as well
  as for arrangement, since picking "the upstage truss" is faster by position
  than by number.

- `[want]` Selection is shared between views and with the programmer, so
  selecting in the layout selects in the sheet and in the command line.
- `[want]` The layout view doubles as the pixel-mapping placement surface
  (§13.2), rather than being a separate tool.

### 17.2 Addressing

- `[want]` **Auto-address** — next free slot, with configurable gap and
  starting offset. Bulk patch of 48 fixtures is one operation.
- Footprint collision rejection — already in `Patch::addFixture()`.
- `[want]` A footprint may not span universes. Standard rule, enforced at
  patch time with a clear message.
- `[want]` **Bulk re-address** preserving all programming. Safe by
  construction: cues reference fixture UIDs (§8.1), not addresses, so
  re-patching cannot break a show.
- `[want]` Offset patching (`+10 to every selected fixture`), clone patching,
  mirror patching for symmetrical rigs.
- `[want]` Unpatched-but-programmable fixtures — patch later, programme now.
  Useful when the rig spec arrives before the addressing does.
- `[want]` A patch conflict report: overlaps, gaps, universes over capacity.

### 17.3 Fixture IDs and sub-fixtures

Three numbers exist and must not be conflated:

| Number | Meaning |
|--------|---------|
| **Fixture ID** | the user-facing number typed at the command line |
| **DMX address** | universe + start channel |
| **UID** | the immutable reference (§8.1) |

- Sub-fixture addressing `12.3` for multi-instance fixtures (§1.3), selectable
  as ranges: `Fixture 12.1 Thru 12.12`.
- `[want]` ID ranges organised by type or position (moving heads 101+, washes
  201+), with auto-assign respecting the convention.
- Renumbering fixture IDs uses §7.4's flow — warn, offer backup, remap,
  report — and is safe for the same UID reason as re-addressing.

### 17.4 3D stage

Positions are not decoration; four subsystems need them:

1. Position presets calculated from a stage point rather than dialled by hand
2. Pixel-mapping fixture placement (§13.2)
3. Laser zone placement (§14.2)
4. Visualisation

Per fixture: `X/Y/Z`, rotation, and the GDTF geometry for shape.
Plus stage geometry: truss, floor, walls, set pieces, screens.

- `[want]` **Point-at** — select fixtures, click a stage location, pan/tilt
  solved for each. The single highest-value use of a 3D stage on a console.
`[done-decision]` **Full 3D authoring in-app**, staged — MVR import is still
the fast path to a working stage, and authoring builds on top of it rather than
replacing it:

1. MVR import plus move/rotate/duplicate of what came in
2. Primitives and truss — parametric truss runs, floor, walls, risers, screens
3. 3D model import (glTF / OBJ / 3DS — GDTF already carries these, so the
   loader is shared with the fixture library)
4. Full authoring — extrusion, snapping, measurement, layers, groups

- `[flag]` Honest scope: stages 2–4 are a CAD-lite tool and a large subsystem
  in their own right. It reuses the §13 render pipeline and the §2.2 window
  manager, so it is not starting from nothing — but it should not block the
  console itself.

### 17.5 MVR and import

`[want]` **MVR import is a priority.** MVR (My Virtual Rig) is GDTF's companion
format and carries fixtures, addresses, positions, rotations and the GDTF
profiles themselves in one file. Vectorworks, Depence and Capture all export
it, so a designer's plot imports complete — patch, positions and profiles in
one step, rather than retyped.

- `[want]` MVR **export** too, so Lumen's patch round-trips back to the plot.
- `[want]` CSV / spreadsheet patch import, with column mapping, since paperwork
  arrives this way more often than MVR does.
- `[flag]` **Console-to-console show import is largely infeasible.** MA3 and
  ChamSys show files are proprietary and undocumented. Patch can come across
  via MVR or CSV; cues, presets and FX realistically cannot. Worth stating
  plainly rather than leaving as an implied promise.

### 17.6 Visualiser

`[flag]` **External visualisation is already free.** Capture, WYSIWYG, Depence
and MA3 3D all consume sACN or Art-Net, which §3 already outputs. Nothing needs
building for a designer to pre-visualise a Lumen show today.

- `[later]` A built-in visualiser reuses the §13 render thread and the §17.4
  stage data. Genuinely useful for busking without a rig, but it is the lowest
  value-per-effort item in the spec and should wait.

### 17.7 Show file format

`[done-decision]` **SQLite**, single file.

Why, against the alternatives:

| Option | Problem |
|--------|---------|
| JSON / YAML | human-diffable, but slow and memory-hungry at show scale, and a crash mid-write corrupts the file |
| Custom binary | fast and opaque; every change is a migration problem |
| Directory of files | git-friendly, but no atomicity across files |

SQLite gives **transactional writes**, which is the requirement that decides
it: a console crashing mid-save must not destroy the show. It is also
queryable, which §16.3's introspection wants anyway, handles large shows, and
is a single file to copy to a USB stick at 2am.

- `[want]` Text export/import alongside it, for version control, diffing and
  merging between programmers. The binary file is the working format; text is
  the interchange format.
- `[want]` Config separate from show: panel sets (§2.5), binding sets (§12.6)
  and user preferences export independently, so a desk setup moves between
  shows.
- `[flag]` **Media and content are referenced, not embedded.** Video files and
  ILDA content live outside the show file, referenced by path plus content
  hash, with a relink workflow when a path breaks — the same model video
  editors use. Per §13.3, sampled pixel values are never stored.

### 17.8 Autosave, backup, recovery

For live use these are not conveniences:

- `[want]` Autosave on a timer and before every destructive operation
  (§7.4's renumber, delete, bulk re-patch), to a rolling set of versions rather
  than overwriting one.
- `[want]` Crash recovery — on restart, offer the last autosave with a clear
  statement of what was and was not saved.
- `[want]` Named snapshots the operator takes manually before a risky change.
- `[want]` Autosave must never block the control thread or stall output.
  Written on a background thread from a consistent snapshot (§15.2).

### 17.9 Engine impact

| Change | Note |
|--------|------|
| `Fixture` gains `uid` | §8.1 |
| `Fixture` gains `x/y/z`, rotation | §17.4 |
| Sub-fixture / cell model | §1.3, §17.3 |
| MVR importer (zip + XML + GDTF) | §17.5 |
| `Patch::addFixture()` collision scan is O(n) | fine now; wants an interval index at thousands of fixtures |
| Persistence layer | SQLite, transactional, background-writable |

---

## 18. I/O and networking

### 18.1 Output routing

`[want]` Routing is **per universe**, not global. One show commonly needs
universe 1 on sACN multicast, universe 2 unicast to a specific node, and
universe 3 on a USB interface.

Per universe: protocol · destination (multicast / broadcast / unicast IP) ·
network interface · sACN priority · output rate · enable/disable ·
preview-only.

- `[want]` Output rate per universe. Some fixtures mis-handle full-rate DMX,
  and a slower universe is the standard fix.
- `[want]` A routing overview page — every universe, its destination, its
  measured output rate, packet counters. The first thing anyone looks at when
  nothing is lighting up.

### 18.2 Protocols out

| Protocol | Status |
|----------|--------|
| sACN (E1.31) | `[done]` multicast + unicast |
| Art-Net | `[done]` broadcast + unicast |
| **ArtPoll / ArtPollReply** | `[want]` — device discovery, so nodes self-populate the routing page |
| sACN universe discovery | `[want]` — E1.31 discovery packets |
| USB DMX | `[want]` — Enttec Open/Pro, FTDI-based, uDMX |
| `[later]` | KiNET, Pathport, ShowNet — niche |

### 18.3 Protocols in, and source arbitration

`[flag]` DMX input is new work (§12.7) and arbitration is the part that is easy
to get wrong.

- `[done]` sACN in with **per-source priority** — E1.31 carries a priority
  field, and two sources on one universe resolve by it, not by arrival order.
  A backup desk at priority 50 is invisible while the main desk sends at 100,
  and takes over the moment the main desk stops. **There is no switchover
  logic**: the main desk simply stops being a candidate, which is why §18.8's
  failover needs nothing built for it.
- `[done]` Per-slot arbitration where priorities tie, per universe: HTP merge,
  or one source owns the universe — resolved by **oldest** or **latest**, both
  deterministic, so the winner cannot flip frame to frame with packet arrival
  order.
- `[done]` Source tracking: every sender seen per universe with CID, origin,
  protocol, priority, packet and out-of-order counts, last-seen time, and
  whether it is winning or being held back. A terminated source stays listed,
  because knowing a desk sent a terminate is itself diagnostic.
- `[done]` Source timeout and failover, 2.5s by default — E1.31's network data
  loss timeout. A universe whose sources all go quiet **stops existing** rather
  than freezing on its last values: a stale universe that looks live is how a
  dead feed goes unnoticed.
- `[done]` Art-Net in, and DMX in from a USB interface — the transports
  themselves aside. Art-Net carries neither a CID nor a priority, so an
  identity is synthesised from its origin and the universe's configured
  priority stands in; without that an Art-Net source could not be arbitrated
  against an sACN one at all.
- `[done]` Rejections that matter: out-of-order sequence numbers per E1.31 4.1
  (behind by up to 20, so a sender restarting its counter is still followed),
  a non-zero START code, preview data, and a short packet that must not zero
  the slots it did not send.
- Uses: remote triggering, another console's output merged in, house-light
  interlocks via the threshold bindings from §12.3.

### 18.4 Network interfaces

`[flag]` Wrong-interface output is the single most common networking failure in
this field — Art-Net broadcast leaving on the wrong NIC and nothing lighting.

- `[want]` Explicit interface selection per protocol and per universe, never
  "default route".
- `[want]` Interface list showing IP, netmask, link state, and whether traffic
  is actually flowing.
- `[want]` Link-loss detection with a visible alarm, not silence.
- `[want]` Multiple NICs simultaneously — art-net on one, session traffic on
  another, management on a third.

### 18.5 Timecode and clock

Sources feeding §11.2's cue triggers:

| Source | Notes |
|--------|-------|
| **LTC** | audio-embedded; needs an audio input and a decoder |
| **MTC** | MIDI timecode |
| **Art-Net timecode** | network |
| **Internal** | free-running, for programming without an external source |

- `[done]` Multiple timecode slots, so several timelines run independently.
- `[done]` Visible timecode state — source, rate, running/stopped, drift in
  frames, jump count, and staleness. A stale slot keeps its last position but
  says it is stale; an internal slot cannot go stale, being its own source.
- `[done]` **Drop-frame is implemented properly.** 29.97 drops frame NUMBERS,
  not frames — two at the start of every minute except every tenth — and
  treating it as plain 30 drifts 3.6 seconds an hour. One hour is 107,892
  frames; a reading naming a dropped number is rejected as invalid rather than
  silently accepted. Verified exhaustively over the first eleven minutes:
  every valid reading round-trips, numbering is consecutive with no gaps, and
  exactly eighteen readings do not exist.
- `[done]` Cue triggers ask whether the timeline **passed** a point, not
  whether it equals it — which is what makes a trigger survive a dropped frame
  or a coarse tick. Backward crossings are reported separately, so a cue list
  can decide for itself what a scrub means.
- `[want]` MIDI clock as a chase rate source (§11.5), separate from timecode.
- `[open]` Clock sync between session members (§18.7) — PTP is the correct
  answer and more work than a simple periodic sync; decide when session
  networking is built.

### 18.6 MIDI and OSC

- MIDI: port selection in and out, MSC (MIDI Show Control) for theatre, clock,
  MTC. Bindings per §12.
- OSC: configurable listen port, send targets, and a published address
  namespace mirroring the command layer (§16.2) — so `/lumen/cmd` can carry any
  command string, rather than needing a hand-built OSC API.
- `[want]` OSC feedback namespace for bidirectional surfaces (§12.5).

### 18.7 Session networking

`[flag]` This is the largest item in this section and the authority model has
to be chosen deliberately, because concurrent editing is a genuinely hard
distributed-systems problem.

Staged, in increasing difficulty:

**Stage 1 — Master plus output nodes.** One machine owns the show. Additional
machines act as output nodes doing nothing but DMX, driven over the network.
Low risk, immediately useful for large rigs.

**Stage 2 — Remote clients.** Tablets and phones as remote focus units. They
hold no authority: they query state (§16.3) and submit commands (§16.2), the
master executes. Reuses the agent architecture exactly.

**Stage 3 — Tracking backup.** See §18.8.

**Stage 4 — Multi-user editing.** Several programmers editing one show
concurrently. `[flag]` This is CRDT/operational-transform territory, and
getting it wrong corrupts shows. Realistic intermediate: **ownership
partitioning** — a programmer checks out a sequence or a pool and holds
exclusive write on it, which gives most of the practical benefit without
concurrent-merge semantics.

### 18.8 Tracking backup

For anything with a real audience, a single machine is a single point of
failure.

- `[want]` A second machine mirrors show state and playback position
  continuously.
- `[want]` Failover detection via heartbeat, with a configurable hold before
  takeover.
- `[flag]` **The elegant trick: fail over using sACN priority.** Both machines
  output the same universes continuously, the backup at a lower E1.31 priority.
  Receivers already arbitrate by priority, so when the master stops, the backup
  wins automatically — no switching hardware, no handoff gap, and nothing to go
  wrong at the moment of failure. The protocol does the work.
- `[want]` Manual takeover, and a clear indication of which machine is live.
- `[open]` Does the backup mirror the full show model, or replay a command
  stream? Command-stream replay is simpler and keeps them in lockstep, but a
  dropped command desynchronises; full-state sync is heavier and
  self-correcting.

### 18.9 Security

`[flag]` Worth stating because consoles habitually ignore it, and venue
networks are not trustworthy. sACN and Art-Net have no authentication by design
and that cannot be fixed. Everything else can be.

- `[want]` Remote clients, OSC control and the agent API (§16) bind to a chosen
  interface only — never all interfaces by default.
- `[want]` Session members and remote clients authenticate with a shared
  show key, not an open port.
- `[want]` Control ports off by default, enabled explicitly.
- `[want]` A visible indicator of what is currently listening and what is
  connected.

### 18.10 Engine impact

| Change | Note |
|--------|------|
| Per-universe routing config | §18.1 — currently one global output list |
| `IOutput` gains per-universe destination and interface binding | §18.1 |
| `IInput` counterpart with source tracking and priority | §18.3 |
| ArtPoll responder, sACN discovery | §18.2 |
| Timecode decoder + clock abstraction | §18.5 |
| Session transport, state replication | §18.7 |
| Interface enumeration and link monitoring | §18.4 |

`[flag]` §18.8's priority-based failover needs nothing new in the output path —
`SacnOutput` already carries a priority field. Setting it per universe is all
that is required, which is a good sign the layer is shaped correctly.

---

## 19. Hardware surfaces

### 19.1 A wing is already mostly solved

§12's Learn system means any MIDI, OSC or HID controller is *already* a wing —
bind its faders and buttons to anything, with feedback. So this section is only
about what hardware needs **beyond** generic bindings.

### 19.2 The proprietary problem

`[flag]` **MA3 and ChamSys wings cannot be supported.** Their USB protocols are
undocumented and proprietary; a MagicQ PC Wing or an MA3 Command Wing will not
work with Lumen without reverse engineering or a licence. This is the same
category of constraint as the ChamSys library in §1.1 and should be stated
plainly rather than left as an implied promise.

What *is* supportable:

| Class | Examples |
|-------|----------|
| Generic MIDI control surfaces | APC40, Launchpad, X-Touch, BCF2000, nanoKONTROL |
| OSC surfaces | TouchOSC, Lemur, custom tablets |
| USB HID | keypads, jog wheels, trackballs, custom panels |
| DIY | Arduino / Teensy / RP2040 as HID or network devices |
| Third-party wings | anyone implementing §19.5 |

### 19.3 Device profiles

A profile is device identity plus a binding set (§12.6), so the machinery
already exists:

- Controls declared: faders, encoders, buttons, displays — with their
  addressing, resolution and feedback capability.
- `[want]` **Auto-configure on connect.** Plug in a known controller and it
  maps itself. Plug in an unknown one and Learn handles it.
- `[want]` Profiles authored by using Learn and saving the result — no separate
  editor, no file format to hand-write.
- `[want]` Shipped profiles for the common controllers above, with
  user-overridable bindings.
- Connecting a wing selects the `Wing` panel set per §2.5, reclaiming the
  on-screen button bands.

### 19.4 Controls and feedback

Beyond simple bindings, four hardware behaviours need explicit support:

- **Endless encoders** — relative mode with acceleration curves, per §12.4's
  `relative` mode. An endless encoder sending relative deltas is the correct
  input for attribute work.
- **Motorised faders** — feedback per §12.5, and they must not fight the
  operator: motor updates suppressed while the fader is touched.
- **Touch-sensitive faders** — touch detect is what makes §12.4's **catch**
  takeover mode work properly. Touching the fader arms the pickup; releasing
  disarms it. Without touch sense, catch mode has to infer intent from
  movement.
- **Surface displays** — scribble strips, LCD/OLED segments. `[want]` They
  render §7.1's three-layer identity, so an encoder's physical label matches
  its on-screen label exactly.

### 19.5 An open wing protocol

`[want]` Publish a documented protocol so third parties and DIY builders can
make hardware that works natively — rather than every wing being a MIDI
approximation of one.

- Transport: USB HID and Ethernet, same message set over both.
- Declares its own controls on connect, so Lumen configures itself with no
  profile needed.
- Carries feedback: LED state, display text, motor positions.
- `[open]` Build on an existing open protocol if a suitable one exists, rather
  than inventing another. Worth a survey before specifying.

This is cheap to define and is the only route to hardware that feels native,
given §19.2.

### 19.6 Touchscreens

The UI in §2 and §10 is touch-first, so this matters more than it would
elsewhere.

- Multi-touch, since §10.3's gesture vocabulary uses two-finger tap.
- `[want]` Calibration for resistive and older panels.
- `[want]` Cursor hidden in touch mode; palm rejection; edge-swipe suppression
  so an OS gesture cannot interrupt a show.
- `[want]` Multiple touchscreens with independent panel sets (§2.5).
- `[flag]` **macOS has poor external touchscreen support.** Given §13.6's
  all-platforms decision, a touch-heavy console surface on a Mac will be
  limited to the trackpad and mouse paths. Worth knowing before planning a
  Mac-based touch rig — Windows and Linux are the better touch hosts.

### 19.7 Engine impact

| Change | Note |
|--------|------|
| HID enumeration and hotplug | cross-platform; hidapi is the usual answer |
| MIDI in/out | RtMidi, already in §15.1's dependency list |
| Device profile store | a binding set (§12.6) plus identity |
| Surface display rendering | text out to hardware, driven from §7.1 |
| Touch input paths | evdev (Linux), WM_POINTER (Windows), limited (macOS) |

`[later]` Purpose-built Lumen hardware. Out of scope, but §19.5's protocol is
what would make it possible without special-casing.

---

## 20. Selection, order and grids

### 20.1 Why this is not cosmetic

Selection order already drives the FX engine: §4.1's phase spread is
`phaseSpreadDeg × idx / total`, so **the order *is* the effect**. The same
ordering drives fan direction, sequential operations and chase direction.

Both reference consoles have two separate concepts here — a linear order and a
spatial layout — and expose them through different features. Unifying them is
the opportunity.

### 20.2 The generalisation: spread sources

`[want]` One abstraction serves FX phase, fan, and any sequential operation: a
**spread source** is a function over the selection that yields a normalised
`0..1` per fixture. Everything that spreads reads one.

| Source | Yields |
|--------|--------|
| `index` | linear selection order — the current behaviour |
| `gridX` / `gridY` / `gridZ` | grid coordinate |
| `gridRadial` | distance from a grid point |
| `gridAngle` | angle around a grid point |
| `stageX/Y/Z` | real 3D position (§17.4) |
| `stageRadial` / `stageAngle` | distance / angle from a stage point |
| `patchOrder` | universe + address |
| `random` | seeded, so it is stable and storable |

This is where §17.4's stage positions pay off a second time: a circular chase
across a rig that *is* physically circular needs `stageAngle`, and no amount of
linear ordering produces it.

### 20.3 Phase modifiers

MA3's phaser parameters become transforms on the spread source rather than
separate mechanisms:

| Modifier | Effect | Formula |
|----------|--------|---------|
| **Groups** | how many full phase cycles across the selection | `× groups` |
| **Blocks** | N adjacent members share one phase | quantise the source |
| **Wings** | mirror the spread into N symmetric wings | fold the source |
| **Offset** | rotate the whole spread | `+ offset` |

```
phase = fold( quantise( source, blocks ), wings ) × groups + offset
```

- `[want]` All four available on any spread source, not just `index`. Wings
  over `gridX` gives a mirrored horizontal wipe; blocks over `stageAngle` gives
  segmented radial chases.

### 20.4 Selection order controls

- `[want]` **Capture order as selected.** `Fixture 1 + 5 + 3` orders 1, 5, 3 —
  not re-sorted to 1, 3, 5. The typing order is information.
- Reorder operations: reverse · shuffle (seeded) · odd/even split · every Nth ·
  manual drag in a list
- Sort by: fixture ID · patch order · name · type · grid position ·
  stage position
- `[want]` **Groups store their order.** Recalling a group restores its
  ordering, so a group is a reusable ordered set rather than just a membership
  list.
- `[want]` Order visible — index numbers overlaid on fixtures in the layout
  view, toggleable.

### 20.5 Grids and layouts

`[want]` A **Layout** is a first-class object — named, UID'd (§8.1), in a pool
with the same sorting and filtering as presets (§7). It maps fixtures to
coordinates.

- A group may reference a layout; the programmer has a current layout, derived
  from the selection or set explicitly.
- Dimensions: auto (squared-off), explicit `W×H`, or `W×H×D`.
- **Gaps are allowed.** A cell may be empty, which is what makes
  non-rectangular arrangements — a circle of fixtures, a chevron, a logo —
  expressible at all.
- Manual placement by dragging fixtures into cells.

**Auto-arrange fill orders:**

| Order | Use |
|-------|-----|
| Row-major / column-major | regular matrices |
| **Serpentine** (boustrophedon) | **LED strips and battens wired in a zigzag** |
| Spiral | centre-out arrangements |

- `[flag]` **Serpentine matters more than it sounds.** LED battens, strips and
  tiles are routinely wired in a zigzag, so DMX order alternates direction every
  row. Without a serpentine fill, every horizontal effect tears on alternate
  rows — and it is the single most common pixel-mapping complaint. A fill-order
  setting fixes it at the layout rather than per effect.
- `[want]` **Derive a layout from stage positions** — project §17.4's 3D
  positions onto a 2D grid automatically. Fastest route to a correct layout
  once MVR is imported.
- `[want]` Sub-fixtures (§1.3) occupy their own cells, so a Pixel Curve 12 fills
  twelve cells from one patch entry.
- `[want]` **Layouts are shared with pixel mapping** (§13.2) — the layout view
  *is* the mapping placement surface, not a parallel tool.

### 20.6 Fan

Fan reads the same spread source, so it inherits grids and stage positions for
free.

- **Fan from**: start · end · centre · centre-mirrored · a nominated fixture
- **Fan over**: any spread source (§20.2)
- **Fan curve**: linear · exponential · S-curve
- `[want]` **Multi-level fan** — fan across groups, then within each group, so
  a rig of four trusses fans truss-to-truss and fixture-to-fixture
  independently.
- `[want]` Fan is live and adjustable after application, not a one-shot
  calculation — adjusting the amount re-derives from the stored source.

### 20.7 Highlight, solo and lowlight

`[want]` Not three modes — **two independent axes**. Both reference consoles
expose these as separate features, which is why they overlap confusingly:

| Axis | Options |
|------|---------|
| **Selected fixtures** | unaffected · **highlight** (forced visible) |
| **Unselected fixtures** | unaffected · **lowlight** (dimmed for context) · **blackout** (solo) |

Every behaviour either console offers is a combination of the two:
highlight + unaffected = MA3 Highlight; highlight + blackout = ChamSys Solo;
highlight + lowlight = focusing with context visible.

**Highlight state per fixture type.** `[want]` A user-editable highlight preset
per type — dimmer full, shutter open, colour open, gobo open, iris open, zoom
mid, frost out — with defaults auto-derived from the taxonomy (§6): set the
`intensity`-kind attribute full, and where a type has none, drive colour to
white instead. That is what makes highlight work on an LED with no dimmer
channel, or a video surface.

**Position in the merge pipeline.** `[flag]` Highlight must sit **above the
programmer** and above every playback including asserted ones (§11.4) — a
highlight that can be beaten by a playback is useless. It sits below only the
laser safety gate (§14.6).

- `[flag]` **Highlight is never stored.** `Store` reads the programmer layer,
  not the highlight layer. This has to be structural, or every focus session
  bakes open-white into cues.
- Highlight snaps; it never fades.
- `[want]` **Sequential highlight** — step through the selection one fixture at
  a time with highlight following, which is how focusing actually gets done.
- `[want]` **Domain-aware.** For a laser zone, highlight is a UI indicator and
  a boundary outline, **never beam output** (§14.6). For a video surface, a
  solid colour fill. Highlight means "show me which one this is", and what that
  means differs per domain.

### 20.8 Groups

Three kinds, which compose:

**Static** — an ordered list of fixtures (§20.4). The default.

**Nested** — a group containing other groups.

- `[flag]` **Selection is a tree, not a list.** This is the real decision. A
  flattened selection loses the structure that §20.6's multi-level fan needs —
  "fan across four trusses, then within each truss" requires the hierarchy at
  apply time. So the programmer holds a *hierarchical* selection and flattens
  to an ordered list on demand for §20.2's linear spread. Both
  representations, one source.
- Each nested group contributes its own stored order, so nesting composes
  ordered sets rather than discarding them.
- `[want]` Cycle prevention — A containing B containing A is rejected at
  assignment.
- Edits to a nested group propagate to its parents. That is the point of
  nesting rather than copying.

**Dynamic** — defined by a query rather than a list, reusing §7.5's facets
exactly: by fixture type, universe, address range, tag, main group, or
sub-attribute presence ("everything with a Prism").

- Live: patching a new fixture that matches adds it to the group.
- `[want]` The query carries a **sort rule**, since a dynamic group has no
  manual order to store — by patch order, ID, stage position or name.
- `[flag]` **Cues are safe from dynamic groups.** A cue stores per-fixture
  values (`ValueLayer` is keyed by fixture), not a group reference — so storing
  from a dynamic group selection captures whatever matched at store time, and
  later changes to the group cannot alter an existing cue. Worth stating
  explicitly because the opposite would be a serious trap.
- By contrast a **tile** (§9.2) bound to a dynamic group *is* a live reference,
  which is desirable — the tile follows the rig.
**Manual overrides — a per-group toggle.** `[done-decision]` Not a fourth group
kind: a switch on any dynamic group, `Allow manual overrides`. Off by default,
so a dynamic group stays purely a query unless the user opts in.

With it on, the group holds three things:

```
membership = (query results − excludes) + includes
```

- **Includes** — fixtures manually added that the query does not match
- **Excludes** — fixtures the query matches but the user suppressed

`[done-decision]` **Both lists are sticky by UID.** The rule is the same in
both directions, which is what makes it predictable:

| Event | Result |
|-------|--------|
| An excluded fixture stops matching the query | exclusion kept; if it matches again later it stays excluded |
| An included fixture starts matching the query | inclusion kept; harmless, and survives if it stops matching |

The alternative — cleaning up redundant entries — means a fixture the user
explicitly removed can silently reappear after a re-patch. Sticky is the
behaviour that respects the instruction.

- `[want]` Overrides must be **visible**, or someone will wonder why "all
  moving heads" is missing one: a badge on the group showing override counts,
  a view listing them, and a `Reset to query` action.
- `[want]` Stale-override indicator — an exclusion for a fixture that no longer
  matches anyway is marked, so the list can be tidied deliberately rather than
  automatically.
- `[want]` **Freeze** — convert a dynamic group to static, capturing current
  membership and order. Commonly wanted before a show, when the rig should stop
  moving underneath the programming.
- `[want]` Order: includes participate in the query's sort rule by default, with
  manual ordering available as a separate option on the same toggle.

All three kinds share: a pool with §7's sorting and filtering, §7.1's
symbol/label/indicator identity, UIDs (§8.1), and tags.

### 20.9 Engine impact

| Change | Note |
|--------|------|
| `Phaser::wavePoint()` takes a spread value, not `idx/total` | §20.2 — currently hardwired to linear index |
| `Phaser` gains `blocks`, `wings`, `groups` | §20.3 |
| `Phaser` generator becomes waveform-or-steps | §4.5 |
| Stack mode per phaser; clipping detection | §4.7 |
| Clock source per phaser; speed-master subscriptions | §4.8 |
| FX fade envelope; stop-mode handling | §4.9 |
| Per-fixture variation from seeded noise | §4.11 |
| `Programmer` selection becomes a hierarchical ordered set | §20.4, §20.8 |
| `Layout` object + pool | §20.5 |
| Spread source resolution needs stage positions and grid coords | §17.4, §20.5 |
| Highlight layer above programmer, excluded from `Store` | §20.7 |
| Highlight state per fixture type, derived from `kind` | §20.7 |
| `Group` object: static / nested / dynamic, query evaluation, UID-keyed include/exclude sets | §20.8 |

`[flag]` The `Phaser::fixtures` vector is already ordered, so §20.2 is a change
of *where the spread value comes from* rather than a restructure — the
`idx/total` term becomes a lookup into a resolved spread table computed once
per selection change.

`[flag]` The highlight layer is a small addition to `Engine::tick()` — one more
`overwriteFrom()` after the programmer — but `Programmer::storeCue()` must keep
reading only `values_`, never the merged result, or highlight leaks into
storage.

---

## 21. Build order

Grouped by what becomes *usable*, not by subsystem. The reason for this
ordering is that **M1 + M2 is a working DMX console** — everything after that
is depth and new domains.

### M0 — Rework what exists

Cheap now, expensive later. Both items touch every file.

- `Attr` enum → `{group, sub, kind}` (§6.3)
- `mergeRuleFor()` reads the `intensity` kind (§6.3)
- Mutex + `universes_` copy → triple-buffer handoff (§15.2)

### M1 — Engine core, headless

Outcome: a scriptable, saveable console with no UI. Fully testable.

- Command layer as the only mutation path (§16.2), with the metadata
  declaration that generates parser, bindings, schema and docs
- Patch: addressing, collision, sub-fixtures, UIDs (§8.1, §17.2)
- Programmer: selection, values, clear semantics
- Cues with per-cue/per-group tracking, Block, Cue Only (§11.1)
- Cue parts and timing, fade curves (§11.2)
- Executors: priority, merge rules, swap groups, release modes (§11.3, §11.4)
- Per-universe output routing (§18.1)
- SQLite persistence, autosave, crash recovery (§17.7, §17.8)
- Introspection queries (§16.3)

### M2 — Console UI

Outcome: usable on a real rig.

- Window manager, placement grid, window header convention (§2.2, §2.4)
- Panel sets and bands, encoder bar (§2.5, §2.6)
- Window open + cursor focus restore, keyboard navigation (§2.7, §2.8)
- Executor surface with tiles (§9)
- Radial menus (§5) — prototype exists at `proto/radial-menu.html`
- Popup presentation modes (§10)
- Patch sheet and layout view (§17.1)

### M3 — Programming depth

- FX: recipes → slots → phasers, role binding (§4)
- Presets: three-layer identity, range labels, auto-stepper (§7.1–7.3)
- Sorting, filtering, Control presets (§7.4–7.6)
- Groups, fan modes, highlight/solo, selection order (§20)

### M4 — Fixture library

- GDTF import and the taxonomy mapping (§6.3) — **built**
- MVR import (§17.5) — **built**: profiles, patch, positions, layer groups
- Fixture editor: table + tree views (§1.2, §1.3) — **built** (model; the
  views are data, not pixels, until §2 renders)
- ChamSys `.hed` import from the user's own install (§1.1) — **blocked**, see
  §21.2
- GDTF export (§1.4) — **built**: writes a valid `.gdtf`, used only for
  fixtures authored or edited here (see §1.4.1)
- MVR export (§17.5) — **built**: positions, layers from tags, and the kept
  profiles embedded
- Fixture library (§1.4.1) — **built**: definitions plus their original
  packages, persisted in the show and reachable from the command layer
- GDTF builder UI: geometry and wheel authoring, schema validation (§1.4)

### M5 — Bindings and I/O

- Learn, binding sets, layering (§12) — **built**: sources, targets, mapping,
  the three takeover modes, conflict detection, the input queue, and sets
  persisted in the show
- Feedback sends (§12.5) — needs a transport to send on
- DMX-in source arbitration (§18.3) — **built**: priority, tie-breaks, source
  tracking, timeout and failover, with sACN and ArtDmx parsers
- Timecode (§18.5) — **built**: slots, drop-frame, drift, staleness, trigger
  crossings
- MIDI, OSC and DMX-in **transports** (§18.6) — the sockets and ports, which
  need real hardware to be worth writing. Everything above them is done and
  takes normalised events, so each transport is a thin edge that parses a
  packet and pushes onto the input queue
- Device profiles, hardware surfaces (§19)

### M6 — Pixel mapping (video stage 1)

- bgfx renderer, 2D surfaces as fixtures (§13.2, §13.6.1)
- Media (HAP) and ISF shaders (§13.5)
- Gather-pass extraction as a merge layer (§13.3, §13.8)
- NDI / Syphon / Spout in and out (§13.6)

### M7 — Laser

Safety and output before content, per §14.8.

- Helios + Ether Dream, point stream, safety gate, scan-fail blanking
- Zones as fixtures
- Point optimiser: dwell, interpolation, budgeting
- ILDA content, then generated beam effects

### M8 — Session and redundancy

- Master + output nodes, remote clients (§18.7)
- Tracking backup via sACN priority failover (§18.8)
- Security: interface binding, show keys (§18.9)

### M9 — Scripting and AI

- Embedded Lua, sandboxed (§16.4)
- Agent: blind-first, propose-then-apply, privilege tiers, audit (§16.5)

### M10 — Later

- Projection mapping (video stage 2) — §13.9
- 3D authoring stages 2–4 — §17.4
- Built-in visualiser — §17.6
- Raster-to-laser conversion — §14.5

### 21.1 Thin areas

Specced lightly or not at all, and worth a pass before M3:

- **Highlight / solo / lowlight** behaviour and its interaction with §11.4
  priority
- **Groups** — nested groups, dynamic groups from a filter
- *(Fan and selection order now specced — §20)*
- **Clear semantics** — what Clear clears at each press, and off/release
  interaction
- **Encoder detail** — acceleration curves, fine mode, snap to ChannelSet
  boundaries (§7.2)
- *(Undo model now specced and built — §8.6)*

### 21.2 ChamSys `.hed` import — blocked

`[flag]` This one cannot be built responsibly yet, and saying so is better than
shipping a parser that half-works.

The MagicQ head file format has no published specification, and nothing
reliable describes it in enough detail to parse. A parser written against a
guessed syntax would appear to work on the one file it was guessed from and
mangle everything else — worse than no importer, because a silently wrong
patch is discovered during a show.

What unblocks it, in order of preference:

1. A handful of real `.hed` files from a MagicQ install, ideally covering a
   plain dimmer, an RGBW LED, a moving head with 16-bit pan/tilt and a
   multi-instance fixture. Four files are enough to infer the grammar and
   validate it against known-good channel layouts.
2. MagicQ's own documentation of the format, if any exists.

Until then, **GDTF and MVR cover the same ground**: both import the attribute
and channel-set detail §7.2 and §7.3 need, and the GDTF library is far larger
than ChamSys's. The `.hed` path is about honouring an existing ChamSys user's
own edited library, not about fixture coverage.

### 21.3 The testable build

`[done-decision]` Everything above the hardware line runs today, with no
hardware and no GUI toolchain, in three pieces:

**`lumen-server`** — the real C++ engine with an HTTP server bound to
**loopback only** (§18.9), serving a browser console. The UI is a web page
purely because that is the one surface available on every machine with nothing
to install; it is **not** the shipping renderer (§2 still wants a native one).

The arrangement has a property worth keeping whatever the final renderer is:
the browser *cannot* reach the model except by sending a command string, so
§16.2 is enforced by the transport rather than by discipline. That is what
exposed the two missing commands in §16.2.1 within an hour of starting.

**`lumen-cli`** — the same engine from a terminal. Every console command is
typeable; `.tick`, `.dmx` and `.help` are the only additions, and they are
shell commands rather than console ones so the vocabularies never blur.

**Simulated hardware** — `SimulatedOutput`, `SimulatedLaser` and
`SimulatedInput` implement the same interfaces the real devices will. The
laser simulation takes only a `GatedFrame`, so §14.6's safety gate applies to
the simulation exactly as to a projector: there is no "test mode" that skips
it, because there is no type that can skip it.

What this deliberately leaves out, all of it *transport* rather than logic:
MIDI and OSC sockets, Art-Net and sACN on the wire, and the laser DACs. The
layers above each of them are built and tested; each needs a device plugged in
to finish, not a design.

#### 21.3.1 Developing with no display

`[done-decision]` The workspace has no screen, which would normally make UI
work unverifiable — the usual outcome being code that compiles and is never
looked at. Headless Chromium plus Playwright solves it completely:

- The page is rendered and screenshotted, and the image inspected directly.
- It is also **driven**: clicking a group chip, dragging a fader, typing a
  command, pressing blackout — each checked against the *engine's* state
  afterwards rather than against the page, so a UI that looks right while
  doing nothing fails the test.
- JavaScript console errors and page errors are collected and treated as
  failures.

This is a test harness, not a workaround: a UI test that asserts against
engine state is the right test whether or not anyone has a monitor.

### 21.4 Built so far

Engine and show model, with the command layer as the only mutation path:

| Area | Spec | State |
| --- | --- | --- |
| Attribute taxonomy, kinds, roles | §6 | built |
| Patch, cells, types, conflicts | §17.2, §17.3 | built |
| Programmer, Clear machine, highlight | §11.8, §20.7 | built |
| Phasers: waves, steps, bezier curves, gaps, bounce | §4.5–4.16 | built |
| Cues, parts, tracking, block, shield | §11.1–11.7 | built |
| Executors, merge, priority, release | §11.3, §11.4 | built |
| Presets, pools, auto-stepper, range labels | §7.1–7.3 | built |
| FX recipes, slots, role binding, parameter presets | §4.1–4.4, §4.17 | built |
| Groups: static, nested, dynamic, overrides | §20.8 | built |
| Spread sources and modifiers | §20.2–20.6 | built |
| Window model, grid, panel sets, navigation | §2.2–2.8 | model + browser console |
| Radial menu geometry and tree | §5 | model + prototype |
| Show file, autosave, atomic save | §17.7, §17.8 | built |
| Command layer, privileges, agent schema | §16.2, §16.5 | built |
| GDTF import, attribute mapping | §1.1, §6.3 | built |
| MVR import | §17.5 | built |
| Fixture editor: geometry tree, table and tree views | §1.2, §1.3 | built |
| Fixture library, packages kept verbatim | §1.4.1 | built |
| GDTF export, MVR export | §1.4, §17.5 | built |
| ZIP writer | — | built |
| Bindings, Learn, layering, takeover | §12 | built |
| Input queue off the output thread | §12.7 | built |
| Undo, redo, scoped undo, action log | §8.6 | built |
| DMX input, source arbitration, failover | §18.3 | built |
| Timecode slots, drop-frame, triggers | §18.5 | built |
| Laser safety gate, un-bypassable by type | §14.6 | built |
| Laser point optimiser | §14.4 | built |
| Group selector, preset and executor master as grammar | §16.2.1 | built |
| Executor surface, both styles, as pages | §9.6 | built |
| Page style conversion, with a loss report | §9.7 | built |
| Canvas drag, resize, alignment, run/edit lock | §9.8 | built |
| Surface edits in the undo history | §9.8.1 | built |
| Tile kinds: group, preset, recipe, timecode, masters, label | §9.3.1 | built |
| Laser gate owned by the engine; zones persist | §14.6.1 | built |
| Arming carries its physical source; no Arm command | §14.6.1 | built |
| Surfaces and tiles in the show file | §9.6, §17.7 | built |
| Simulated output, laser and input | §21.3 | built |
| Local server and browser console | §21.3 | built |
| Headless UI rendering and interaction tests | §21.3.1 | built |

Not started, and each blocked on a device rather than a decision: MIDI and OSC
transports, Art-Net and sACN on the wire (§18), laser DACs (§14.7), hardware
surfaces (§19).

Not started, and not blocked: pixel and video (§13), session networking
(§18.7), Lua plugins and the agent (§16.3, §16.4), 3D authoring (§17.4
stages 2–4), the native renderer (§2).

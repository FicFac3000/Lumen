# Lumen — working notes

A lighting control system in C++20. The goal is a console usable on a real
show: MA3's depth and ChamSys's immediacy, without the twenty years of
decisions neither can revisit.

## Read SPEC.md first

`SPEC.md` (~3,900 lines, 21 sections) is the single source of truth and the
project's memory. Every decision is recorded there **with the reasoning**,
tagged `[done-decision]`, `[want]`, `[open]` or `[flag]`. §21.4 is the
built-so-far table.

Keep it current. A decision made and not written down will be re-litigated
or silently reversed. When you make a call, record *why* — the reasoning is
worth more than the conclusion, because the conclusion can be re-derived and
the reasoning cannot.

## The rule that matters most

**§16.2: the command layer is the only mutation path.** The UI, macros,
plugins and the agent all go through `Console::execute`. There are no
UI-only actions.

This is enforced, not aspirational, and it keeps paying: building a surface
against it found four operations with no command form at all (`Group`,
`Preset`, `ExecMaster`, and the whole surface-editing set). Nothing is
missing until something has to go through the front door.

So: **write the command before the UI that needs it.**

## How to work here

- Build: `cmake -S . -B build && cmake --build build -j`
- Test: `ctest --test-dir build` — 22 binaries, all must pass
- Run: `./build/lumen-server --demo`, then http://127.0.0.1:7373
- Terminal instead: `./build/lumen-cli --demo`
- UI checks: `python3 tools/ui_drive.py out.png` against a **freshly started**
  server (see RUNNING.md)

**Run the sanitisers before trusting a change:**

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake --build build-asan -j && ctest --test-dir build-asan

Two use-after-frees hid in the tests for months and passed on Linux every
time. A green suite on one machine is not evidence of correctness when the
bug is undefined behaviour — it only means that machine's allocator was
forgiving.

## Conventions that are not obvious

- **Comments say WHY, never what.** The code says what. A comment that
  restates the line below it is noise; one that records the failure a
  decision prevents is the only durable form of the reasoning.
- **Errors name the thing and the fix.** "no tile at slot 7 (tiles are at
  1, 4, 9)" rather than "not found". An error that sends someone hunting is
  half an error.
- **Refuse rather than guess.** A preset that says nothing for the selected
  fixtures is refused with that reason; an overlapping tile placement is
  refused, not nudged. Silent approximation is how a show breaks at 20:00.
- **A default id of 0 means unassigned**, never 1 — two stores shipped with
  `id = 1` and silently shared it between objects. Check any new store.
- **`deque` keeps references stable across GROWTH, not erasure.** Re-look-up
  after a remove rather than holding a reference over one.
- Three identities per object: immutable UID (what references store),
  mutable id, mutable name (§8.1).

## What is deliberately not built

Everything needing hardware, behind simulation stubs that implement the same
interfaces: MIDI and OSC sockets, Art-Net and sACN on the wire, laser DACs.
The layers above each are built and tested — each needs a device plugged in,
not a design.

Likewise §13's raster half: the renderer, media decode, ISF shaders. §13.1's
boundary runs one way, so everything downstream works on a `RasterFrame`
whatever filled it, and a GPU slots in behind the same interface.

Blocked on the user: ChamSys `.hed` import (§21.2) needs four real sample
files; nothing else can unblock it responsibly.

## Safety

§14.6 is structural, not policy. `GatedFrame` has a private constructor and
`SafetyGate` is its only friend, so bypassing the laser gate is a **compile
error**. Arming names the physical act that caused it (`ArmSource`), and the
enum has no value a cue, macro or agent could honestly pass. **There is no
`Arm` command and there must not be one.** Disarm and emergency stop are
commands, because the safe direction should be as reachable as possible.

Do not weaken any of this for convenience. If it is in the way, that is the
design working.

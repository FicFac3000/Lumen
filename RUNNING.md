# Running Lumen

No hardware is needed, and nothing has to be installed beyond a C++20
compiler and CMake. Everything that would touch a wire is simulated behind
the same interfaces the real devices will implement.

## Build

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j
    ctest --test-dir build          # 20 test binaries

## The console

    ./build/lumen-server --demo

Then open the address it prints — `http://127.0.0.1:7373` by default. The
server listens on loopback only (§18.9); it is not reachable from the
network.

`--demo` patches a rig to play with: eight LED pars, four moving spots, a
pixel bar, three groups, colour and position palettes, and a three-cue
sequence on executor 1. Use `--show my.lumen` to open a saved show instead.

Things to try, all of which also work typed into the command line at the
bottom, because the UI has no other way to reach the engine (§16.2):

    Group 1 At 80
    Fixture 101 Thru 104 At 100
    Preset 1.3
    Go
    Undo

## The terminal

    ./build/lumen-cli --demo

The same engine, no browser. `Help` lists the commands. Three shell
commands are not console commands and start with a dot: `.tick` advances
the engine a pass, `.dmx` prints the simulated output, `.quit` exits.

One-shot, for scripting and bug reports:

    ./build/lumen-cli --demo -c 'Fixture 1 Thru 4 At 100; Fixture 1 At Red 100'

## Checking the interface

The UI is tested by driving it in a headless browser and asserting against
the **engine's** state, not the page — a console that looks right while
doing nothing has to fail. Needs `pip install playwright` and a Chromium;
set `LUMEN_CHROME` if yours is not at the default path.

    ./build/lumen-server --demo --port 7373 &
    python3 tools/ui_drive.py out.png     # clicks, drags, types; reports
    python3 tools/ui_shot.py http://127.0.0.1:7373 shot.png

Run it against a **freshly started** `--demo` server. It converts a surface
page between styles, and canvas → wing → canvas is not the identity: the
first leg discards the tile sizes, by design (§9.6). A server you have
already been driving by hand will fail the check, and it says so rather than
failing obscurely.

## What is not here yet

Transports only, each blocked on a device rather than a decision: MIDI and
OSC sockets, Art-Net and sACN on the wire, laser DACs. The layers above all
of them are built and tested — see §21.3 of SPEC.md.

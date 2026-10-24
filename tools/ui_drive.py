"""Drive the Lumen console in a headless browser and report what happened.

Run `lumen-server --demo --port 7373` first, then:  python3 tools/ui_drive.py

This is the interaction half of having no display: not "does it render" but
"does clicking the thing do the thing", verified against the engine's own
state rather than against the page.
"""
import os, sys, asyncio, json, urllib.request
from playwright.async_api import async_playwright

BASE = os.environ.get("LUMEN_URL", "http://127.0.0.1:7373")
CHROME = os.environ.get(
    "LUMEN_CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")


def state():
    with urllib.request.urlopen(BASE + "/state") as r:
        return json.load(r)


def cmd(c):
    urllib.request.urlopen(urllib.request.Request(
        BASE + "/cmd", data=c.encode(), method="POST")).read()


async def main(out):
    results = []
    async with async_playwright() as p:
        b = await p.chromium.launch(
            executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1600, "height": 1000})
        errs = []
        pg.on("console", lambda m: errs.append(f"{m.type}: {m.text}")
              if m.type == "error" else None)
        pg.on("pageerror", lambda e: errs.append(f"pageerror: {e}"))

        cmd("ClearAll")

        # This drives a show and then checks the engine, so it needs the demo
        # rig as `--demo` builds it. Converting a canvas page to a wing and
        # back is NOT the identity — the sizes are gone after the first leg —
        # so a server somebody has already been poking at will fail here for
        # reasons that have nothing to do with the code.
        pages = {p["name"]: p for p in state()["surfaces"]}
        fresh = (pages.get("Show", {}).get("style") == "canvas" and
                 any(t["w"] > 1 for t in pages.get("Show", {}).get("tiles", [])))
        if not fresh:
            print("  !! restart `lumen-server --demo` first: page 2 is not the "
                  "demo canvas any more")
            await b.close()
            return 1

        await pg.goto(BASE, wait_until="domcontentloaded")
        await pg.wait_for_timeout(1200)

        async def step(name, action, check):
            try:
                await action()
                await pg.wait_for_timeout(700)
                ok, detail = check(state())
            except Exception as e:                      # noqa: BLE001
                ok, detail = False, repr(e)
            results.append((ok, name, detail))

        # A group chip must make a selection.
        await step(
            "click group chip 'Front Wash'",
            lambda: pg.click("text=Front Wash"),
            lambda s: (len(s["programmer"]["selection"]) == 8,
                       f'{len(s["programmer"]["selection"])} selected'))

        # A fader must set a level on that selection.
        await step(
            "drag Intensity fader",
            lambda: pg.eval_on_selector_all(
                "#grid input[type=range]",
                "els => { const e = els[0]; e.value = 70;"
                " e.dispatchEvent(new Event('change', {bubbles:true})); }"),
            lambda s: (any(a["value"] is not None and abs(a["value"] - 0.70) < 0.02
                           for f in s["fixtures"] if f["selected"]
                           for a in f["attrs"] if a["attr"] == "Dimmer"),
                       "dimmer at 70%"))

        # A preset chip must apply as a reference, not a copy.
        await step(
            "click preset 'Amber'",
            lambda: pg.click("text=Amber"),
            lambda s: (any(a["isRef"] for f in s["fixtures"] if f["selected"]
                           for a in f["attrs"]), "stored as a reference"))

        # Blackout must round-trip through the command layer.
        await step(
            "click Blackout",
            lambda: pg.click("#blackout"),
            lambda s: (s["blackout"] is True, "blackout on"))
        await step(
            "click Blackout again",
            lambda: pg.click("#blackout"),
            lambda s: (s["blackout"] is False, "blackout off"))

        # The command line is the same path as every button.
        await step(
            "type a command",
            lambda: type_cmd(pg, "Fixture 101 Thru 104 At 55"),
            lambda s: (any(abs((a["value"] or 0) - 0.55) < 0.02
                           for f in s["fixtures"] if f["id"] in (101, 102, 103, 104)
                           for a in f["attrs"] if a["attr"] == "Dimmer"),
                       "movers at 55%"))

        # A bad command must report, not break the page.
        await step(
            "type a nonsense command",
            lambda: type_cmd(pg, "Wibble 3"),
            lambda s: (True, "page survived"))
        reply = await pg.inner_text("#reply")
        results.append(("unknown command" in reply, "error shown on the command line",
                        reply.strip()))

        # The grand master is a master, not a value edit.
        await step(
            "move the grand master",
            lambda: pg.eval_on_selector(
                "#gm", "e => { e.value = 40;"
                " e.dispatchEvent(new Event('change', {bubbles:true})); }"),
            lambda s: (abs(s["grandMaster"] - 0.40) < 0.02,
                       f'grand master {s["grandMaster"]:.2f}'))

        # Opening a closed window.
        await step(
            "open the Laser window",
            lambda: pg.click("#windowbar >> text=LASER"),
            lambda s: (True, "clicked"))
        laser_open = await pg.locator(".win >> text=LASER").count()
        results.append((laser_open > 0, "Laser window appears", f"{laser_open} match"))

        # ---- the executor surface (§9.6) ---------------------------------
        # The style belongs to the PAGE, and the engine owns the pages. So
        # both styles have to render from the same snapshot, and whatever
        # works in one must work in the other.
        cmd("SurfaceStyle wing 1")
        cmd("SurfaceStyle canvas 2")
        await pg.wait_for_timeout(500)

        for page, style, cls in [("Wing", "wing", ".wing-cell"),
                                 ("Show", "canvas", ".c-tile")]:
            await pg.click(f".style-pick >> text={page} · {style}")
            await pg.wait_for_timeout(600)
            cur = next(p for p in state()["surfaces"]
                       if p["id"] == state()["surface"])
            results.append((cur["name"] == page,
                            f"selecting {page} is a command",
                            f'engine says page {cur["id"]}'))
            n = await pg.locator(cls).count()
            results.append((n > 0, f"{style} page renders", f"{n} tiles"))

            # A tile must Go its executor.
            cmd("Off 1")
            await pg.wait_for_timeout(400)
            await pg.locator(f"{cls}:has-text('Main Looks')").first.click()
            await pg.wait_for_timeout(700)
            live = next((x for x in state()["executors"] if x["id"] == 1), None)
            results.append((bool(live and live["active"]),
                            f"{style}: tile Go", "executor 1 active"))

            # The fader strip must set the master, through the command layer.
            cmd("ExecMaster 1 100")
            await pg.wait_for_timeout(400)
            box = await pg.locator(
                f"{cls}:has-text('Main Looks') .x-fader").first.bounding_box()
            await pg.mouse.click(box["x"] + box["width"] / 2,
                                 box["y"] + box["height"] * 0.75)
            await pg.wait_for_timeout(700)
            m = next(x["master"] for x in state()["executors"] if x["id"] == 1)
            results.append((0.15 < m < 0.45, f"{style}: fader sets master",
                            f"master {m:.2f}"))

        # Converting a page is a command, and the page really changes.
        await pg.click(".style-pick >> text=Show · canvas")
        await pg.wait_for_timeout(600)
        await pg.click(".style-pick >> text=Make it a wing")
        await pg.wait_for_timeout(800)
        now = next(p for p in state()["surfaces"] if p["id"] == 2)
        results.append((now["style"] == "wing", "convert canvas to wing",
                        f'page 2 is now {now["style"]}'))
        tiles_after = len(now["tiles"])
        results.append((tiles_after == 6, "conversion kept every tile",
                        f"{tiles_after} tiles"))
        await pg.click(".style-pick >> text=Make it a canvas")
        await pg.wait_for_timeout(800)
        back = next(p for p in state()["surfaces"] if p["id"] == 2)
        results.append((back["style"] == "canvas", "and back again",
                        f'page 2 is {back["style"]}'))

        # Side by side must show every page, each in its own style, and the
        # wing must NOT reflow — reflowing destroys the one property it is
        # there to demonstrate.
        await pg.click(".style-pick >> text=Side by side")
        await pg.wait_for_timeout(700)
        halves = await pg.locator(".half").count()
        wings = await pg.locator(".wing-cell").count()
        results.append((halves == 2 and wings == 20,
                        "side by side shows both pages",
                        f"{halves} halves, {wings} wing slots"))

        # ---- canvas editing (§9.1, §9.4) ---------------------------------
        await pg.click(".style-pick >> text=Show · canvas")
        await pg.wait_for_timeout(600)

        # Run mode is the default and the engine enforces it, so a move sent
        # while locked must be refused at the command layer, not just greyed
        # out in the UI.
        cmd("EditMode off 2")
        await pg.wait_for_timeout(400)
        locked = state()["surfaces"][1]["tiles"][0]
        import urllib.error
        req = urllib.request.Request(
            BASE + "/cmd", data=b"TileMove 1 2 4", method="POST")
        with urllib.request.urlopen(req) as r:
            reply = json.load(r)
        results.append((reply["ok"] is False and "run mode" in reply["message"],
                        "run mode refuses a move", reply["message"][:46]))
        still = state()["surfaces"][1]["tiles"][0]
        results.append((still["x"] == locked["x"] and still["y"] == locked["y"],
                        "and the tile did not move",
                        f'at {still["x"]},{still["y"]}'))

        # Edit mode is a command too, and the button is a view of the page.
        await pg.click(".wing-pages >> text=Edit layout")
        await pg.wait_for_timeout(700)
        results.append((state()["surfaces"][1]["editMode"] is True,
                        "the mode button is a command", "page 2 in edit mode"))

        # Drag a tile two rows down. The ghost previews, the engine commits.
        tiles = pg.locator(".c-tile")
        before = state()["surfaces"][1]["tiles"]
        first = sorted(before, key=lambda t: (t["y"], t["x"]))[0]
        box = await tiles.nth(0).bounding_box()
        await pg.mouse.move(box["x"] + 40, box["y"] + 12)
        await pg.mouse.down()
        await pg.mouse.move(box["x"] + 40, box["y"] + 12 + 4 * 50, steps=10)
        ghost = await pg.locator(".ghost").count()
        results.append((ghost == 1, "a drag shows a ghost", f"{ghost} ghost"))
        await pg.mouse.up()
        await pg.wait_for_timeout(900)
        after = state()["surfaces"][1]["tiles"]
        moved = [t for t in after if t["y"] != first["y"]]
        results.append((len(moved) > 0, "drag moves the tile",
                        f'{len(moved)} tile(s) in a new row'))

        # A drag onto an occupied cell must be refused BEFORE the drop, and
        # must leave the page exactly as it was.
        snapshot_before = [(t["x"], t["y"], t["w"], t["h"]) for t in state()["surfaces"][1]["tiles"]]
        b0 = await tiles.nth(1).bounding_box()
        b1 = await tiles.nth(2).bounding_box()
        await pg.mouse.move(b0["x"] + 40, b0["y"] + 12)
        await pg.mouse.down()
        await pg.mouse.move(b1["x"] + 40, b1["y"] + 12, steps=10)
        bad = await pg.locator(".ghost.bad").count()
        results.append((bad == 1, "an overlapping drag is marked before the drop",
                        "ghost turns red"))
        await pg.mouse.up()
        await pg.wait_for_timeout(800)
        snapshot_after = [(t["x"], t["y"], t["w"], t["h"]) for t in state()["surfaces"][1]["tiles"]]
        results.append((snapshot_before == snapshot_after,
                        "and nothing moved", "page unchanged"))

        # Alignment: select two tiles and align. Applied all or nothing.
        await tiles.nth(0).click()
        await pg.wait_for_timeout(500)
        await tiles.nth(1).click(modifiers=["Shift"])
        await pg.wait_for_timeout(600)
        sel = [t for t in state()["surfaces"][1]["tiles"] if t["selected"]]
        results.append((len(sel) == 2, "shift-click adds to the selection",
                        f"{len(sel)} selected"))
        await pg.click(".wing-pages >> text=Match W")
        await pg.wait_for_timeout(800)
        widths = {t["w"] for t in state()["surfaces"][1]["tiles"] if t["selected"]}
        results.append((len(widths) == 1, "match width makes them one width",
                        f"widths {widths}"))
        after_align = state()["surfaces"][1]
        results.append((not after_align["problems"],
                        "alignment never leaves an overlap",
                        "page still valid"))

        # Arranging a surface is an edit, so it has to be undoable (§8.6).
        # A conversion can drop tiles and an alignment moves several at once:
        # neither is something anyone can put back by hand from memory.
        before_undo = [(t["x"], t["y"], t["w"], t["h"])
                       for t in state()["surfaces"][1]["tiles"]]
        cmd("TileMove " + str(state()["surfaces"][1]["tiles"][0]["slot"] + 1) +
            " 1 7")
        await pg.wait_for_timeout(600)
        after_move = [(t["x"], t["y"], t["w"], t["h"])
                      for t in state()["surfaces"][1]["tiles"]]
        results.append((before_undo != after_move, "a move changes the page",
                        "moved"))
        await pg.click(".wing-pages >> text=↶ Undo")
        await pg.wait_for_timeout(800)
        restored = [(t["x"], t["y"], t["w"], t["h"])
                    for t in state()["surfaces"][1]["tiles"]]
        results.append((restored == before_undo, "undo puts the tile back",
                        "page restored exactly"))

        # And the layout edit shows up in the history with its own scope,
        # rather than being filed under something else.
        hist = state()["history"]
        results.append((any(h["scope"] == "layout" for h in hist),
                        "layout edits have their own undo scope",
                        f"{len(hist)} entries"))

        # §9.3's other tile kinds. A tile that renders but does nothing, or
        # resolves but does not render, is worse than one that is absent.
        await pg.click(".style-pick >> text=Wing · wing")
        await pg.wait_for_timeout(700)
        page1 = state()["surfaces"][0]
        kinds = {t["kind"] for t in page1["tiles"]}
        results.append(({"executor", "group", "preset", "blackout",
                         "grandmaster"} <= kinds,
                        "the wing carries several tile kinds",
                        ", ".join(sorted(kinds))))

        drawn = await pg.locator(".wing-cell:not(.empty)").count()
        results.append((drawn == len(page1["tiles"]),
                        "every placed tile is drawn",
                        f'{drawn} drawn of {len(page1["tiles"])}'))

        # A colour preset shows the colour it produces, resolved by the
        # engine rather than derived from its name.
        swatch = next(t for t in page1["tiles"]
                      if t["kind"] == "preset" and t["colour"])
        results.append((swatch["colour"].startswith("#"),
                        "a colour preset carries its real colour",
                        f'{swatch["legend"]} {swatch["colour"]}'))

        # Pressing a group tile selects, pressing a preset tile applies.
        cmd("ClearAll")
        await pg.wait_for_timeout(300)
        gslot = next(t for t in page1["tiles"] if t["kind"] == "group")
        await pg.locator(".wing-cell").nth(gslot["slot"]).click()
        await pg.wait_for_timeout(700)
        results.append((len(state()["programmer"]["selection"]) > 0,
                        "a group tile selects",
                        f'{len(state()["programmer"]["selection"])} fixtures'))

        # A COLOUR preset specifically: a position palette applied to LED pars
        # is correctly refused, and picking whichever preset came first would
        # make this test depend on tile order rather than on behaviour.
        pslot = next(t for t in page1["tiles"]
                     if t["kind"] == "preset" and t["colour"])
        await pg.locator(".wing-cell").nth(pslot["slot"]).click()
        await pg.wait_for_timeout(700)
        refs = [a for f in state()["fixtures"] if f["selected"]
                for a in f["attrs"] if a["isRef"]]
        results.append((len(refs) > 0, "a preset tile applies as a reference",
                        f"{len(refs)} referenced values"))

        # A blackout tile reflects state and toggles it.
        bslot = next(t for t in page1["tiles"] if t["kind"] == "blackout")
        await pg.locator(".wing-cell").nth(bslot["slot"]).click()
        await pg.wait_for_timeout(700)
        results.append((state()["blackout"] is True, "a blackout tile toggles",
                        "blackout on"))
        await pg.locator(".wing-cell").nth(bslot["slot"]).click()
        await pg.wait_for_timeout(700)
        results.append((state()["blackout"] is False, "and back off", "blackout off"))

        # §14.6: a laser tile shows state and can disarm. It must NEVER
        # offer to arm — a tile that armed on a tap is the single-press path
        # the whole design exists to prevent.
        lz = next((t for t in page1["tiles"] if t["kind"] == "laserzone"), None)
        results.append((lz is not None, "a laser zone tile resolves",
                        lz["legend"] if lz else "absent"))
        if lz:
            results.append((not lz["missing"] and not lz["unsupported"],
                            "and is neither broken nor inert", lz["detail"]))
            results.append(("Arm" not in lz["command"] or
                            lz["command"].startswith("Disarm"),
                            "and never offers to arm", repr(lz["command"])))

        # There is deliberately no Arm command for anything to reach.
        req = urllib.request.Request(BASE + "/cmd", data=b"Arm 1", method="POST")
        with urllib.request.urlopen(req) as r:
            armed = json.load(r)
        results.append((armed["ok"] is False and armed["parsed"] is False,
                        "no Arm command exists at all", armed["message"][:40]))

        # §9.1: no two tiles may overlap on the canvas.
        clash = await pg.locator(".clash").count()
        results.append((clash == 0, "canvas has no overlapping tiles",
                        "none" if clash == 0 else "overlap reported"))

        cmd("GrandMaster 100")
        await pg.wait_for_timeout(600)
        await pg.screenshot(path=out, full_page=False)
        await b.close()

    bad = [r for r in results if not r[0]]
    for ok, name, detail in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name:38} {detail}")
    for e in errs[:10]:
        print("  JS  ", e)
    print(f"\n{len(results) - len(bad)}/{len(results)} passed, "
          f"{len(errs)} JS error(s)")
    return 1 if (bad or errs) else 0


async def type_cmd(pg, text):
    await pg.click("#cmd")
    await pg.fill("#cmd", text)
    await pg.press("#cmd", "Enter")


sys.exit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "drive.png")))

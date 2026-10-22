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

        # ---- the executor surface, in both styles (§9) -------------------
        # Both render from the same state, so whatever works in one must work
        # in the other. That is the point of having both.
        for style, cls in [("ChamSys wing", ".wing-cell"), ("MA3 canvas", ".c-tile")]:
            await pg.click(f".style-pick >> text={style}")
            await pg.wait_for_timeout(500)
            n = await pg.locator(cls).count()
            results.append((n > 0, f"{style} renders", f"{n} tiles"))

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

        # Side by side must show both, and the wing must NOT reflow to fit —
        # reflowing would destroy the one property it exists to demonstrate.
        await pg.click(".style-pick >> text=Side by side")
        await pg.wait_for_timeout(600)
        wings = await pg.locator(".wing-cell").count()
        tiles = await pg.locator(".c-tile").count()
        results.append((wings == 20 and tiles > 0, "side by side shows both",
                        f"{wings} wing slots, {tiles} canvas tiles"))

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

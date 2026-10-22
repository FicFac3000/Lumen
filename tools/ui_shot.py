import os, sys, asyncio
from playwright.async_api import async_playwright
async def main(url, out, w=1600, h=1000):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=os.environ.get("LUMEN_CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"))
        pg = await b.new_page(viewport={"width": w, "height": h}, device_scale_factor=1)
        errs = []
        pg.on("console", lambda m: errs.append(f"{m.type}: {m.text}") if m.type in ("error","warning") else None)
        pg.on("pageerror", lambda e: errs.append(f"pageerror: {e}"))
        # The console polls forever, so networkidle never arrives.
        await pg.goto(url, wait_until="domcontentloaded", timeout=30000)
        await pg.wait_for_timeout(1500)
        await pg.screenshot(path=out, full_page=False)
        await b.close()
        for e in errs[:10]: print("JS", e)
        print("ok", out)
asyncio.run(main(sys.argv[1], sys.argv[2],
                 int(sys.argv[3]) if len(sys.argv)>3 else 1600,
                 int(sys.argv[4]) if len(sys.argv)>4 else 1000))

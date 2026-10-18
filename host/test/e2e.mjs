// End-to-end test: starts the PC harness, opens the viewer in headless
// Chromium and checks that frames arrive upright (corner markers), that
// layouts request only visible screens, and that the viewer reconnects.
import { spawn } from "node:child_process";
import { createRequire } from "node:module";

// Resolved through NODE_PATH so a globally installed Playwright works too.
const { chromium } = createRequire(import.meta.url)("playwright");

const port = 18000 + Math.floor(Math.random() * 1000);
const bin = new URL("../../build/host/3dslink-host", import.meta.url).pathname;
const env = { ...process.env, ...(process.env.TDSL_ENV ? JSON.parse(process.env.TDSL_ENV) : {}) };
let server = spawn(bin, [String(port)], { env, stdio: ["ignore", "inherit", "inherit"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error("FAIL:", msg); process.exitCode = 1; };

await sleep(400);
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
// Connection errors are expected while the test restarts the server.
page.on("console", (m) => { if (m.type() === "error" && !/ERR_CONNECTION_REFUSED|ERR_EMPTY_RESPONSE/.test(m.text())) errors.push(m.text()); });

try {
  await page.goto(`http://127.0.0.1:${port}/`);
  await page.waitForFunction(() => document.getElementById("statusText").textContent.startsWith("Live"), null, { timeout: 5000 });
  await sleep(1500);

  // Sample the centre of each corner marker on both canvases (canvas pixels).
  const sample = () => page.evaluate(() => {
    const px = (id, x, y) => Array.from(document.getElementById(id).getContext("2d").getImageData(x, y, 1, 1).data.slice(0, 3));
    return {
      top: [px("top", 12, 12), px("top", 388, 12), px("top", 12, 228), px("top", 388, 228)],
      bottom: [px("bottom", 12, 12), px("bottom", 308, 12), px("bottom", 12, 228), px("bottom", 308, 228)],
    };
  });
  const expect = {
    top: [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]],
    bottom: [[255, 0, 255], [0, 255, 255], [255, 255, 0], [0, 0, 0]],
  };
  const near = (a, b) => a.every((v, i) => Math.abs(v - b[i]) < 40);
  const got = await sample();
  for (const s of ["top", "bottom"])
    got[s].forEach((c, i) => { if (!near(c, expect[s][i])) fail(`${s} corner ${i}: got ${c}, want ${expect[s][i]}`); });
  console.log("corners", JSON.stringify(got));

  const fps = await page.evaluate(() => document.getElementById("statusText").textContent);
  console.log("status:", fps);

  // Top-only layout must only request (and capture) the top screen.
  const getStatus = async () => (await fetch(`http://127.0.0.1:${port}/api/status`)).json();
  await page.keyboard.press("3");
  await sleep(500);
  const a = await getStatus();
  await sleep(1500);
  const status = await getStatus();
  const topNew = status.screens.top.frames - a.screens.top.frames;
  const bottomNew = status.screens.bottom.frames - a.screens.bottom.frames;
  console.log("top-only: new top frames", topNew, "new bottom frames", bottomNew, "streams", status.streams);
  if (status.streams !== 1) fail("expected exactly one stream after layout change");
  if (bottomNew !== 0) fail("bottom screen still being captured in top-only layout");
  if (topNew < 30) fail("top screen did not speed up when it had the 3DS to itself");

  // Settings round-trip.
  await page.evaluate(() => fetch("/api/settings?quality=40&fps_top=12"));
  const st2 = await getStatus();
  if (st2.settings.quality !== 40 || st2.settings.fps_top !== 12) fail("settings not applied");
  await sleep(600);
  const b0 = await getStatus();
  await sleep(2000);
  const b1 = await getStatus();
  const capped = (b1.screens.top.frames - b0.screens.top.frames) / 2;
  console.log("capped top fps:", capped);
  if (capped > 13.5 || capped < 9) fail("fps cap not respected");
  await page.evaluate(() => fetch("/api/settings?quality=70&fps_top=30"));

  // Restart the "3DS": the viewer must notice and reconnect by itself.
  server.kill("SIGTERM");
  await sleep(1200);
  const offline = await page.evaluate(() => document.getElementById("statusText").textContent);
  console.log("after kill:", offline);
  server = spawn(bin, [String(port)], { env, stdio: ["ignore", "inherit", "inherit"] });
  await page.waitForFunction(() => document.getElementById("statusText").textContent.startsWith("Live"), null, { timeout: 10000 });
  console.log("reconnected");

  await page.keyboard.press("1");
  await sleep(1200);
  await page.screenshot({ path: process.env.SHOT || "/tmp/3dslink-e2e.png" });

  // Pop-out: main tab must pause its own stream while the pop-out runs.
  const [popup] = await Promise.all([page.waitForEvent("popup"), page.click("#popoutBtn")]);
  await popup.waitForFunction(() => document.getElementById("top").getContext("2d").getImageData(12, 12, 1, 1).data[0] > 200, null, { timeout: 8000 });
  await sleep(2500);
  const st4 = await getStatus();
  const mainNotice = await page.evaluate(() => document.getElementById("noticeTitle").textContent);
  console.log("with pop-out: streams", st4.streams, "| main tab says:", mainNotice);
  if (st4.streams !== 1) fail("main tab did not pause while pop-out is open");
  if (process.env.POPSHOT) await popup.screenshot({ path: process.env.POPSHOT });
  await popup.close();
  await page.waitForFunction(() => document.getElementById("statusText").textContent.startsWith("Live"), null, { timeout: 8000 });
  console.log("main tab resumed after pop-out closed");

  if (errors.length) fail("page errors: " + errors.join(" | "));
} catch (e) {
  fail(e.stack || String(e));
} finally {
  await browser.close();
  server.kill("SIGTERM");
}
if (!process.exitCode) console.log("PASS");

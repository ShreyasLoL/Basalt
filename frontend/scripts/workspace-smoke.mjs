// Run against a disposable Chrome profile and a running local production gateway.
import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import { join } from "node:path";

const [gateway, debuggerPort, screenshotDirectory, integrationFlag] = process.argv.slice(2);
assert.equal(new URL(gateway).hostname, "127.0.0.1");
const targets = await fetch("http://127.0.0.1:" + Number(debuggerPort) + "/json/list").then(r => r.json());
const target = targets.find(t => t.type === "page" && (t.url === "about:blank" || t.url.startsWith(gateway + "/")));
assert.ok(target, "Expected a dedicated blank or gateway tab");
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let seq = 0;
const pending = new Map();
const exceptions = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.method === "Runtime.exceptionThrown") exceptions.push(message.params.exceptionDetails.text);
  const callback = pending.get(message.id);
  if (callback) {
    pending.delete(message.id);
    callback(message);
  }
};
function send(method, params = {}) {
  const id = ++seq;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error(method + " timed out")), 5000);
    pending.set(id, message => {
      clearTimeout(timer);
      if (message.error) reject(Error(message.error.message)); else resolve(message.result);
    });
    socket.send(JSON.stringify({ id, method, params }));
  });
}
async function evaluate(expression) {
  const { result, exceptionDetails } = await send("Runtime.evaluate", { expression, returnByValue: true });
  if (exceptionDetails) throw Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
  return result.value;
}
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(expression) {
  for (let i = 0; i < 100; ++i) {
    if (await evaluate("Boolean(" + expression + ")")) return;
    await wait(50);
  }
  throw Error("Never observed " + expression);
}
try {
  await send("Runtime.enable");
  await send("Page.navigate", { url: gateway });
  await until('document.querySelector(".workspace-picker select")');
  for (const width of [1440, 768, 390, 320]) {
    await send("Emulation.setDeviceMetricsOverride", { width, height: 900, deviceScaleFactor: 1, mobile: false });
    const names = await evaluate('Array.from(document.querySelector(".workspace-picker select").options, o => o.value)');
    assert.ok(names.includes("Benchmarks") && names.includes("Executions"));
    for (const name of names) {
      const value = JSON.stringify(name);
      await evaluate(width > 820
        ? 'Array.from(document.querySelectorAll(".nav-item")).find(b => b.textContent.trim() === ' + value + ').click()'
        : '(() => { const s = document.querySelector(".workspace-picker select"); s.value = ' + value + '; s.dispatchEvent(new Event("change", {bubbles:true})); })()');
      await until('document.querySelector("h1")?.textContent === ' + value);
      const actual = await evaluate("document.documentElement.scrollWidth");
      if (actual > width) console.log(await evaluate('Array.from(document.querySelectorAll("body *")).filter(e => e.getBoundingClientRect().right > innerWidth && !e.closest(".table-scroll")).slice(0,10).map(e => e.className || e.tagName)'));
      assert.ok(actual <= width, name + " overflows at " + width + "px (" + actual + ")");
      if (screenshotDirectory && ["Overview", "Market Replay", "Portfolio & Risk"].includes(name) && [1440,390].includes(width)) {
        const { data } = await send("Page.captureScreenshot", { format: "png" });
        await writeFile(join(screenshotDirectory, width + "-" + name.replaceAll(/[^a-z]/gi, "") + ".png"), Buffer.from(data, "base64"));
      }
    }
    console.log(width + "px: " + names.length + " workspaces");
  }
  if (integrationFlag === "--integration") {
    // Use only with a disposable gateway. The benchmark submits a bounded job.
    const choose = async name => {
      await evaluate('(() => { const select = document.querySelector(".workspace-picker select"); select.value = ' +
        JSON.stringify(name) + '; select.dispatchEvent(new Event("change", { bubbles: true })); })()');
      await until('document.querySelector("h1")?.textContent === ' + JSON.stringify(name));
    };
    await choose("Instrument Workstation");
    await until('Array.from(document.querySelectorAll("dt")).some(dt => dt.textContent === "Price tick")');
    console.log("Live instrument catalog loaded");
    await choose("Benchmarks");
    await evaluate(`(() => {
      const count = document.querySelector('input[type="number"]');
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(count, "37");
      count.dispatchEvent(new Event("input", {bubbles:true}));
    })()`);
    await evaluate('document.querySelector(".benchmark-form").requestSubmit()');
    await until('document.querySelectorAll(".table-scroll tbody tr").length > 0');
    console.log("Live benchmark rows:", await evaluate('document.querySelectorAll(".table-scroll tbody tr").length'));

    // This gateway has no positive bars route. Supply chart data only inside the QA tab.
    await send("Fetch.enable", { patterns: [{ urlPattern: "*/bars?*" }] });
    const onRequest = async ({ data }) => {
      const { method, params } = JSON.parse(data);
      if (method !== "Fetch.requestPaused") return;
      const bars = [0,1,2].map(i => ({ open_time_ns: (1700000000 + 60 * i) * 1e9,
        open_micro: 10010+i, low_micro: 10001+i, high_micro: 10015+i, close_micro: 10013+i }));
      await send("Fetch.fulfillRequest", { requestId: params.requestId, responseCode: 200,
        responseHeaders: [{ name: "Content-Type", value: "application/json" }], body: Buffer.from(JSON.stringify({ bars })).toString("base64") });
    };
    socket.addEventListener("message", onRequest);
    try {
      await choose("Charts");
      await until('document.querySelector(".chart-panel")?.textContent.includes("3 bars") && document.querySelector(".chart-canvas canvas")');
      console.log("Fixture chart rendered three bars on canvas");
    } finally {
      await send("Fetch.disable");
      socket.removeEventListener("message", onRequest);
    }
  }
  await send("Page.navigate", { url: gateway });
  await until('document.querySelector(".skip-link")');
  for (const [key, code] of [["Tab",9], ["Enter",13]]) {
    for (const type of ["keyDown","keyUp"]) await send("Input.dispatchKeyEvent", { type, key, windowsVirtualKeyCode: code });
    if (key === "Tab") assert.equal(await evaluate("document.activeElement.className"), "skip-link");
  }
  await until('document.activeElement.id === "main-content"');
  assert.deepEqual(exceptions, []);
  console.log("Keyboard skip link passed. No uncaught runtime exceptions.");
} finally {
  socket.close();
}

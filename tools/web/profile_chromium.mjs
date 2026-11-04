// CPU profile of an Emscripten page in Chromium, across the main thread and
// every worker (pthreads), with the V8 sampling profiler over CDP. Prints the
// functions with the most self time. For readable wasm names link with
// --profiling-funcs.
//
//   node tools/web/profile_chromium.mjs --dir <build dir> --page <page.html> [--args "<argv>"]
//        [--seconds N] [--start-after <regex>] [--top N] [--profile <chromium profile dir>]
//        [--keyboard <spec>]
//
// Serves --dir on 127.0.0.1:47816 with COOP/COEP (the same origin as
// browser_test.mjs with LGP_PICK_VIA_FETCH/LGP_SAFARI_KEEP_OPFS, so OPFS data
// copied there is visible), opens the page with autostart=1 and profiles for
// --seconds after the first console line matching --start-after.
// LGP_CHROMIUM_PATH / LGP_PLAYWRIGHT_DIR as in browser_test.mjs. Local only.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';

const opts = { seconds: 20, top: 40, args: '' };
for (let i = 2; i < process.argv.length; ++i)
  opts[process.argv[i].replace(/^--/, '').replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = process.argv[++i];
if (!opts.dir || !opts.page) {
  console.error('usage: profile_chromium.mjs --dir <dir> --page <page> [--args ..] [--seconds N] [--start-after re]');
  process.exit(2);
}

const require = createRequire(import.meta.url);
const roots = [process.env.LGP_PLAYWRIGHT_DIR, path.join(execSync('npm root -g').toString().trim(), 'playwright')];
let pw = null;
for (const r of roots.filter(Boolean)) { try { pw = require(r); break; } catch {} }
if (!pw) { console.error('Playwright not found'); process.exit(77); }

const root = path.resolve(opts.dir);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((req, res) => {
  const file = path.resolve(root, '.' + decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream',
    'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cache-Control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(47816, '127.0.0.1', r));

const launchOpts = {
  headless: false,
  executablePath: process.env.LGP_CHROMIUM_PATH || undefined,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--remote-debugging-port=9333'],
};
const context = opts.profile
  ? await pw.chromium.launchPersistentContext(opts.profile, launchOpts)
  : await (await pw.chromium.launch(launchOpts)).newContext();
const page = await context.newPage();

// Raw CDP on the browser endpoint: auto-attach (flatten) reaches the page's
// dedicated workers (pthreads), which Playwright's CDP sessions do not.
const version = await (await fetch('http://127.0.0.1:9333/json/version')).json();
const ws = new WebSocket(version.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let nextId = 1;
const waiting = new Map();
const threads = new Map();  // sessionId -> label
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = nextId++;
  waiting.set(id, { resolve, reject });
  ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
});
let profiling = false;
const startProfiler = async (sid) => {
  try {
    await send('Profiler.enable', {}, sid);
    await send('Profiler.setSamplingInterval', { interval: 500 }, sid);
    await send('Profiler.start', {}, sid);
  } catch {}
};
ws.addEventListener('message', async (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && waiting.has(m.id)) {
    const w = waiting.get(m.id); waiting.delete(m.id);
    if (m.error) w.reject(new Error(m.error.message)); else w.resolve(m.result);
    return;
  }
  if (m.method === 'Target.attachedToTarget') {
    const { sessionId, targetInfo } = m.params;
    threads.set(sessionId, `${targetInfo.type} ${targetInfo.url.split('/').pop()}`);
    // Workers of the page: attach recursively, keep them running.
    send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId).catch(() => {});
    send('Runtime.runIfWaitingForDebugger', {}, sessionId).catch(() => {});
    if (profiling) startProfiler(sessionId);
  }
});
await send('Target.setDiscoverTargets', { discover: true });
await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });

let started = false;
const startRe = opts.startAfter ? new RegExp(opts.startAfter) : null;
page.on('console', (m) => {
  const t = m.text();
  if (!started && (!startRe || startRe.test(t))) started = true;
});
// Nobody answers the software keyboard's page dialog: --keyboard (or env
// LGP_KEYBOARD, default none) unless --args names one, as in browser_test.mjs.
const args = /(^|\s)--keyboard(\s|=|$)/.test(opts.args) ? opts.args
  : `${opts.args} --keyboard ${opts.keyboard || process.env.LGP_KEYBOARD || 'none'}`.trim();
const url = `http://localhost:47816/${opts.page}?args=${encodeURIComponent(args)}&autostart=1`;
await page.goto(url);
while (!started) await page.waitForTimeout(100);
profiling = true;
for (const sid of threads.keys()) await startProfiler(sid);
console.log(`profiling ${threads.size} targets for ${opts.seconds} s`);
await page.waitForTimeout(Number(opts.seconds) * 1000);

const cdps = [];
for (const [sid, name] of threads) {
  try { const r = await send('Profiler.stop', {}, sid); cdps.push({ name, profile: r.profile }); } catch {}
}
const self = new Map();  // function -> microseconds
let total = 0;
for (const { name, profile } of cdps) {
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const dt = profile.timeDeltas;
  let threadTotal = 0;
  for (let i = 0; i < profile.samples.length; ++i) {
    const n = byId.get(profile.samples[i]);
    const f = n.callFrame.functionName || '(anonymous ' + (n.callFrame.url.split('/').pop() || '?') + ')';
    if (f === '(idle)' || f === '(program)') continue;
    const us = dt[i] || 0;
    self.set(f, (self.get(f) || 0) + us);
    threadTotal += us;
  }
  total += threadTotal;
  // Per thread: its own top functions without waits (blocked threads).
  const mine = new Map();
  for (let i = 0; i < profile.samples.length; ++i) {
    const n = byId.get(profile.samples[i]);
    const f = n.callFrame.functionName;
    if (!f || /^\((idle|program)\)$|futex_wait|mailbox_await/.test(f)) continue;
    mine.set(f, (mine.get(f) || 0) + (dt[i] || 0));
  }
  const work = [...mine.values()].reduce((a, b) => a + b, 0);
  if (work > 5e5) {
    console.log(`${name}: ${(work / 1e6).toFixed(2)} s working`);
    for (const [f, us] of [...mine.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6))
      console.log(`    ${(us / 1e6).toFixed(3).padStart(7)} s  ${f.slice(0, 110)}`);
  }
}
const rows = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, Number(opts.top));
console.log(`top self time (all threads, ${(total / 1e6).toFixed(2)} s busy):`);
for (const [f, us] of rows) console.log(`${(us / 1e6).toFixed(3).padStart(8)} s  ${(100 * us / total).toFixed(1).padStart(5)} %  ${f.slice(0, 120)}`);
ws.close();
await context.close();
server.close();

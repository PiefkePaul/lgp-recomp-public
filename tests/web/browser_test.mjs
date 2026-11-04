// Runs an Emscripten page in Chromium (Playwright) and checks its output
// (P4.4). Serves the build directory itself with the COOP/COEP headers
// threads need, opens page?args=..., waits until a console line matches
// --expect and the program exited with 0, and checks that the canvas showed
// rendered pixels while the program ran (run it with enough frames;
// --no-canvas-check for programs that render offscreen and check themselves).
//
//   node browser_test.mjs --dir <build dir> --page <page.html> --args "<argv>"
//                         --expect <regex> [--timeout ms] [--headless]
//                         [--no-canvas-check] [--argv "<argv>"]
//                         [--pick <dir>] [--then-args "<argv>" --then-expect <regex>]
//                         [--lines <file>] [--fixture-expected <file>] [--absent <text>]
//                         [--press-after <regex> --press <keys>] [--keyboard <spec>]
//                         [--webgpu-context-fails <n>]
//
// Pages of lgp_add_web_page() (P6.1, platform/web/game.html.in): --pick
// chooses <dir> in the page's folder picker, which copies the user data
// into OPFS and starts the program (autostart=1); --argv replaces the page's
// default argv instead of appending. --then-args/--then-expect load the page
// a second time in the same browser context (OPFS kept, no pick) and check
// that run too. --lines: the lines of <file> must appear in the program
// output in this order (`*` at the end matches a prefix, `#` comments);
// --fixture-expected: debug/exit lines equal a P2.8 end-state file (like
// tests/boot/run_boot.py); --absent: <text> must not appear. --press holds
// keys (Playwright key names, comma-separated) once a console line matches
// --press-after (input tests, P5.1). --keyboard (or env LGP_KEYBOARD):
// software keyboard of boot pages, e.g. text:Red; default none (the game's
// initial text), only added when the arguments name no --keyboard.
// --webgpu-context-fails: the page's first <n> canvas.getContext('webgpu')
// calls on the main thread return null, as Safari on iPadOS sometimes does
// on the first start (Playwright browsers only; skipped with safari).
//
// Browser (option --browser or env LGP_BROWSER, default chromium):
//   chromium  Playwright Chromium, WebGPU on SwiftShader (CI)
//   webkit    Playwright WebKit (same engine as Safari, not Safari itself)
//   firefox   Playwright Firefox (WebGPU on Linux needs Playwright >= 1.64,
//             Firefox 157, via LGP_PLAYWRIGHT_DIR; LGP_FIREFOX_PATH: another
//             Playwright-patched Firefox binary)
//   safari    the installed Safari (macOS). Without WebDriver: the page is
//             opened with `open -a Safari` (later tests reuse that tab: the
//             server listens on a fixed port, LGP_SAFARI_PORT, default
//             47816, so run them one at a time), the server injects a script
//             that posts console lines, page errors and the canvas brightness
//             back; --pick hands the folder's files to the page's own copy
//             code, --press sends synthetic keydown events (not trusted input).
//             LGP_SAFARI_SPIN_PROBE=1: when the page's main thread polls
//             performance.now() for 2 s without returning to the event loop
//             (an active wait), its JavaScript/wasm stack is reported.
//             LGP_SAFARI_SYNC_LOG=<regex>: page lines matching it are sent
//             synchronously (they arrive while the main thread is busy).
//             Memory watchdog: LGP_SAFARI_MAX_MB (default 10000) per web
//             content process; a failed run kills its game page (> 2 GB).
//             LGP_SAFARI_KEEP_OPFS=1 keeps OPFS from an earlier run (large game
//             data copied once) and autostarts the page without --pick.
//             A test that needs a real user gesture (--needs-user-gesture, e.g.
//             resuming an AudioContext under Safari's autoplay policy) is
//             skipped there.
//   LGP_CHROMIUM_PATH: another Chromium binary for --browser chromium.
//   LGP_CHROMIUM_PROFILE: persistent profile directory for --browser chromium
//             (OPFS on disk; needed for the whole game's data). The server then
//             listens on a fixed port, LGP_CHROMIUM_PORT, default 47817.
//   LGP_CHROMIUM_REAL_GPU=1: Chromium on the host's GPU, not SwiftShader.
//   LGP_CHROMIUM_ARGS: extra Chromium command-line switches, separated by
//             spaces (e.g. --enable-dawn-features=use_dxc for Dawn toggles).
//   LGP_PICK_VIA_FETCH=1 (any browser): --pick streams the files from the test
//             server straight into OPFS (where the page's copy would put them)
//             and loads the page again with autostart; Playwright's upload is
//             impractical for gigabytes.
//
// Exit codes: 0 ok, 1 failed, 77 skipped (no Playwright, no Chromium, no
// WebGPU adapter). WebGPU needs a GPU or SwiftShader; headless Chromium loses
// the SwiftShader device on some hosts, so by default the browser runs headed
// (ctest wraps it in xvfb-run when there is no display).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';

const SKIP = 77;
// LGP_PICK_VIA_FETCH=1: stream --pick files from the test server into OPFS.
// Playwright's setInputFiles and Safari's in-memory Blobs are impractical for
// gigabytes of game data.
const fetchPick = process.env.LGP_PICK_VIA_FETCH === '1';
const T0 = Date.now();
const opts = { timeout: 60000, headless: false, canvas: true, args: '', absent: [],
               browser: process.env.LGP_BROWSER || 'chromium' };
for (let i = 2; i < process.argv.length; ++i) {
  const a = process.argv[i];
  if (a === '--headless') opts.headless = true;
  else if (a === '--no-canvas-check') opts.canvas = false;
  else if (a === '--needs-user-gesture') opts.needsUserGesture = true;
  else if (a === '--absent') opts.absent.push(process.argv[++i]);
  else opts[a.replace(/^--/, '').replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = process.argv[++i];
}
for (const k of ['dir', 'page', 'expect']) {
  if (!opts[k]) { console.error(`missing --${k}`); process.exit(2); }
}

async function loadPlaywright() {
  const require = createRequire(import.meta.url);
  const roots = [];
  if (process.env.LGP_PLAYWRIGHT_DIR) roots.push(process.env.LGP_PLAYWRIGHT_DIR);
  try { roots.push(path.join(execSync('npm root -g', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(), 'playwright')); } catch {}
  for (const r of [...roots, 'playwright']) {
    try { return require(r); } catch {}
  }
  return null;
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json' };
const root = path.resolve(opts.dir);
const isolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cache-Control': 'no-store',
};
// --browser safari: reports from the injected script go to the current page.
let reportSink = null;
const safariCommands = [];
let safariNext = null;  // {url, taken}: the page the Safari tab should load
let safariRun = '';     // lgp_run of the current test page
let lastReport = 0;     // Date.now() of the current page's last report
let prefilling = false; // a prefill runs (its streams can hold up the reports)
const server = http.createServer((req, res) => {
  if (req.method === 'POST' && req.url.startsWith('/__lgp_report')) {
    // Reports of other tabs (an earlier run, or one Safari restored after a
    // restart) are dropped: they would mix into this test's output. Every
    // report of the current page carries its run, so untagged ones are foreign.
    const from = new URL(req.url, 'http://x').searchParams.get('run');
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => {
      try {
        if (reportSink && from && from === safariRun) {
          lastReport = Date.now();
          for (const ev of JSON.parse(body)) reportSink(ev);
        }
      } catch {}
      res.writeHead(204, isolation); res.end();
    });
    return;
  }
  if (req.method === 'GET' && req.url.startsWith('/__lgp_next')) {
    // Polled by an open test tab: the page to load next, if it is not that tab's.
    const have = new URL(req.url, 'http://x').searchParams.get('run');
    let next = null;
    // A command meant for one tab (`only`: the close at the end) goes to that tab only.
    if (safariNext && !safariNext.taken && safariNext.run !== have && (!safariNext.only || safariNext.only === have)) {
      next = safariNext.url;
      safariNext.taken = true;
    } else if (have && safariRun && have !== safariRun) {
      // A second test tab (left over or restored by Safari) closes, so no
      // two game instances run at the same time.
      next = 'close';
    }
    res.writeHead(200, { 'Content-Type': 'application/json', ...isolation });
    res.end(JSON.stringify({ next }));
    return;
  }
  if (req.method === 'GET' && req.url.startsWith('/__lgp_fresh')) {
    // Safari reuses one origin for all tests: start each test like a fresh
    // browser context (empty OPFS and localStorage), then load the test page.
    const to = new URL(req.url, 'http://x').searchParams.get('to') || '/';
    const run = new URL(to, 'http://x').searchParams.get('lgp_run') || '';
    res.writeHead(200, { 'Content-Type': 'text/html', ...isolation });
    // If OPFS cannot be emptied (an entry still held by another tab's access
    // handle), the test fails with that reason instead of running on stale data.
    res.end(`<!doctype html><meta charset="utf-8"><title>lgp test</title><script>(async () => {
      try { localStorage.clear(); } catch (e) {}
      let error = 'not emptied';
      for (let attempt = 0; attempt < 10 && error; ++attempt) {
        if (attempt) await new Promise((r) => setTimeout(r, 300));
        try {
          const root = await navigator.storage.getDirectory();
          for await (const name of root.keys()) await root.removeEntry(name, { recursive: true });
          let left = 0;
          for await (const name of root.keys()) ++left;
          error = left ? left + ' entries left' : '';
        } catch (e) { error = String(e); }
      }
      if (!error) { location.replace(${JSON.stringify(to)}); return; }
      await fetch(${JSON.stringify('/__lgp_report?run=' + encodeURIComponent(run))}, { method: 'POST', body: JSON.stringify([
        { t: 'console', text: 'fresh: OPFS cleanup failed: ' + error }, { t: 'console', text: 'exit code 1' }]) });
    })();</script>`);
    return;
  }
  if (req.method === 'GET' && req.url.startsWith('/__lgp_cmd')) {
    // Commands go to the current test page only: a left-over tab (e.g. one
    // Safari restored) must not take the prefill and run the game under its
    // own, ignored run id.
    const have = new URL(req.url, 'http://x').searchParams.get('run') || '';
    res.writeHead(200, { 'Content-Type': 'application/json', ...isolation });
    res.end(JSON.stringify(safariRun && have !== safariRun ? [] : safariCommands.splice(0)));
    return;
  }
  if (req.method === 'GET' && req.url.startsWith('/__lgp_pick/')) {
    // Files of the --pick folder for the injected script (Safari).
    const sub = decodeURIComponent(req.url.slice('/__lgp_pick/'.length));
    const base = path.resolve(opts.pick || '.');
    const file = path.resolve(base, sub);
    if (!opts.pick || !file.startsWith(base + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404, isolation); res.end(); return;
    }
    res.writeHead(200, { 'Content-Type': 'application/octet-stream', ...isolation });
    fs.createReadStream(file).pipe(res);
    return;
  }
  const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.resolve(root, '.' + rel);
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream', ...isolation });
  if ((opts.browser === 'safari' || fetchPick) && path.extname(file) === '.html') {
    const html = fs.readFileSync(file, 'utf8');
    const at = html.search(/<head[^>]*>/i);
    const end = at < 0 ? 0 : at + html.slice(at).indexOf('>') + 1;
    res.end(html.slice(0, end) + SAFARI_CAPTURE + html.slice(end));
    return;
  }
  fs.createReadStream(file).pipe(res);
});

// Injected first into every page in --browser safari: mirrors console output
// and page errors to the test server and samples the canvas (a 64x36 copy;
// WebGPU canvases can be read with drawImage while a frame is presented).
const SAFARI_CAPTURE = `<script>(() => {
  const q = [];
  // One report in flight at a time: parallel POSTs can reach the server out
  // of order. No keepalive: Safari rejects keepalive bodies over 64 KiB, and
  // a rejected batch goes back to the front of the queue.
  let inFlight = null;
  const run = new URLSearchParams(location.search).get('lgp_run') || '';
  const send = () => {
    if (!q.length || inFlight) return;
    const batch = q.splice(0);
    inFlight = fetch('/__lgp_report?run=' + encodeURIComponent(run), { method: 'POST', body: JSON.stringify(batch) })
      .then((r) => { if (!r.ok) throw new Error('status ' + r.status); },
            () => { throw new Error('network'); })
      .catch(() => { q.unshift(...batch); })
      .finally(() => { inFlight = null; });
  };
  // LGP_SAFARI_SYNC_LOG=<regex>: matching lines go out at once with a
  // synchronous request, so they arrive even while the main thread is too busy
  // for the regular reports (those wait for the event loop).
  const syncLog = ${JSON.stringify(process.env.LGP_SAFARI_SYNC_LOG || '')};
  const syncRe = syncLog ? new RegExp(syncLog) : null;
  // LGP_SAFARI_SPIN_PROBE=1: a main thread that waits actively (Emscripten's
  // futex wait on the main thread polls performance.now()) never returns to
  // the event loop. After 2 s of such polling without a turn of the event loop,
  // send the JavaScript stack (with wasm function names) once, synchronously.
  if (${JSON.stringify(process.env.LGP_SAFARI_SPIN_PROBE === '1')}) {
    let turn = performance.now(), sent = 0;
    const tick = () => { turn = origNow(); setTimeout(tick, 100); };
    const origNow = performance.now.bind(performance);
    setTimeout(tick, 100);
    performance.now = () => {
      const t = origNow();
      if (t - turn > 2000 && sent < 3) {
        ++sent;
        turn = t;
        try {
          const x = new XMLHttpRequest();
          x.open('POST', '/__lgp_report?run=' + encodeURIComponent(run), false);
          x.send(JSON.stringify([{ t: 'console', text: 'spin probe: main thread busy for 2 s, stack: ' + new Error().stack.replace(/\\n/g, ' <- ') }]));
        } catch (e) {}
      }
      return t;
    };
  }
  for (const k of ['log', 'info', 'warn', 'error', 'debug']) {
    const o = console[k].bind(console);
    console[k] = (...a) => {
      const text = a.map(String).join(' ');
      if (syncRe && syncRe.test(text)) {
        try {
          const x = new XMLHttpRequest();
          x.open('POST', '/__lgp_report?run=' + encodeURIComponent(run), false);
          x.send(JSON.stringify([{ t: 'console', text }]));
        } catch (e) {}
      } else {
        q.push({ t: 'console', text });
      }
      o(...a);
    };
  }
  addEventListener('error', (e) => q.push({ t: 'pageerror', text: String(e.message) }));
  addEventListener('unhandledrejection', (e) => q.push({ t: 'pageerror', text: String(e.reason) }));
  const probe = document.createElement('canvas');
  probe.width = 64; probe.height = 36;
  setInterval(() => {
    const c = document.getElementById('canvas');
    if (c && c.width) {
      try {
        const g = probe.getContext('2d', { willReadFrequently: true });
        g.clearRect(0, 0, 64, 36);
        g.drawImage(c, 0, 0, 64, 36);
        const d = g.getImageData(0, 0, 64, 36).data;
        let lit = 0;
        for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) ++lit;
        q.push({ t: 'canvas', lit: lit / (64 * 36), size: c.width + 'x' + c.height });
      } catch (e) {}
    }
    send();
  }, 200);
  addEventListener('pagehide', send);
  // Commands from the test: {pick: [{path, rel}]} feeds the page's folder
  // picker as if the user had chosen the folder (the page's own planCopy and
  // copyToOpfs run); {key: {key, code}} sends a keydown.
  // One tab for all tests: load the next test's page when the test server
  // (same origin, fixed port) has one.
  setInterval(async () => {
    try {
      const r = await (await fetch('/__lgp_next?run=' + encodeURIComponent(run))).json();
      // replace(), not href: with one history entry Safari lets the
      // script close the tab at the end of the test.
      if (r.next === 'close') {
        q.push({ t: 'console', text: 'safari: closing the test tab (history ' + history.length + ')' });
        send();
        await inFlight;
        window.close();
        location.replace('about:blank');
      }
      else if (r.next) location.replace(r.next);
    } catch (e) {}
  }, 500);
  let polling = false;
  setInterval(async () => {
    if (polling) return;
    polling = true;
    let cmds = [];
    try {
      cmds = await (await fetch('/__lgp_cmd?run=' + encodeURIComponent(run))).json();
    } catch (e) {}  // no test server between two tests
    try {
      for (const c of cmds) {
        if (c.pick) {
          const entries = [];
          for (const f of c.pick) {
            const blob = await (await fetch('/__lgp_pick/' + encodeURIComponent(f.rel))).blob();
            entries.push({ path: f.path, file: new File([blob], f.path.split('/').pop()) });
          }
          window.LgpWeb.entriesFromFileList = () => entries;
          document.getElementById('pick-dir').dispatchEvent(new Event('change'));
        }
        if (c.prefill) {
          // Stream the files straight into OPFS where the page's own copy
          // would put them (planCopy), then reload: the page autostarts from
          // OPFS. For gigabytes of data, where Blobs in memory fail.
          const W = window.LgpWeb;
          const plan = W.planCopy(c.prefill.map((f) => ({ path: f.path, file: { size: f.size, rel: f.rel } })));
          await W.clearData();
          const root = await navigator.storage.getDirectory();
          let done = 0;
          for (const cp of plan.copies) {
            let dir = root;
            const parts = [...W.DATA_DIR, ...cp.dest.split('/')];
            for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create: true });
            const fh = await dir.getFileHandle(parts[parts.length - 1], { create: true });
            const res = await fetch('/__lgp_pick/' + encodeURIComponent(cp.file.rel));
            await res.body.pipeTo(await fh.createWritable());
            if (++done % 2000 === 0) console.log('prefill: ' + done + '/' + plan.copies.length);
          }
          console.log('prefill: copied ' + done + ' files, ' + plan.bytes + ' bytes');
          send();
          // The first load had no autostart (it would have started on stale
          // OPFS data from an earlier run); this one starts on the new data.
          const next = new URL(location.href);
          next.searchParams.set('autostart', '1');
          // replace(): one history entry, so the tab may close itself later.
          setTimeout(() => location.replace(next.href), 300);
        }
        if (c.key) {
          const ev = new KeyboardEvent(c.type || 'keydown', { key: c.key.key, code: c.key.code, bubbles: true });
          (document.activeElement || document.body).dispatchEvent(ev);
        }
      }
    } catch (e) { console.log('lgp test command failed: ' + e); }
    polling = false;
  }, 200);
})();</script>`;
// Safari: a fixed port, so the tab of the previous test (same origin) can
// pick up the next page instead of a new tab being opened (see runSafari).
// Persistent Chromium profile: a fixed port too, since OPFS is per origin and
// every new port would leave another copy of the game data in the profile.
const port = opts.browser === 'safari' ? Number(process.env.LGP_SAFARI_PORT || 47816)
  : opts.browser !== 'webkit' && process.env.LGP_CHROMIUM_PROFILE ? Number(process.env.LGP_CHROMIUM_PORT || 47817)
  : 0;
await new Promise((r, j) => { server.once('error', j); server.listen(port, '127.0.0.1', r); });
function pageUrl(args, argv, pick) {
  const q = new URLSearchParams();
  // Boot pages (game.html.in) start the program on their own here. Nobody
  // answers the software keyboard's page dialog in a test, so these runs get
  // --keyboard (option --keyboard or env LGP_KEYBOARD, default none = the
  // game's initial text) unless the arguments already name one.
  const boot = opts.pick || opts.thenArgs !== undefined || process.env.LGP_SAFARI_KEEP_OPFS === '1';
  const withKeyboard = (a) => !boot || /(^|\s)--keyboard(\s|=|$)/.test(a || '') ? a
    : `${a || ''} --keyboard ${opts.keyboard || process.env.LGP_KEYBOARD || 'none'}`.trim();
  if (argv !== undefined) q.set('argv', withKeyboard(argv));
  else q.set('args', withKeyboard(args));
  // Prefill (fetch pick, any browser) sets autostart itself once the new
  // data is in OPFS; with it here, a persistent profile would start right away
  // on what an earlier run left there.
  const prefill = pick && fetchPick;
  if (!prefill && boot) q.set('autostart', '1');
  return `http://localhost:${server.address().port}/${opts.page}?${q}`;
}

// Firefox: WebGPU is on by default on Windows (141) and macOS (147), not in
// Linux release builds (Firefox 157 measured); the prefs turn it on and let
// audio start without a gesture. Firefox's AudioContext
// only runs with a real audio output (e.g. PulseAudio with a null sink),
// Chromium fakes one (docs/knowledge/firefox-als-zielbrowser.md).
const FIREFOX_PREFS = {
  'dom.webgpu.enabled': true,
  'media.autoplay.default': 0,
  'media.autoplay.block-webaudio': false,
};

let code = 1;
const pw = opts.browser === 'safari' ? null : await loadPlaywright();
if (opts.browser === 'safari') {
  code = await runSafari();
} else if (!pw) {
  console.log('skipped: Playwright not found (set LGP_PLAYWRIGHT_DIR)');
  code = SKIP;
} else {
  let browser;
  try {
    if (opts.browser === 'webkit') {
      browser = await pw.webkit.launch({ headless: opts.headless });
    } else if (opts.browser === 'firefox') {
      browser = await pw.firefox.launch({
        executablePath: process.env.LGP_FIREFOX_PATH || undefined,
        headless: opts.headless,
        firefoxUserPrefs: FIREFOX_PREFS,
      });
    } else {
      const launch = {
        // LGP_CHROMIUM_PATH: another Chromium build than Playwright's own
        // (e.g. when the matching download is not available).
        executablePath: process.env.LGP_CHROMIUM_PATH || undefined,
        headless: opts.headless,
        // LGP_CHROMIUM_REAL_GPU=1: the host's GPU instead of SwiftShader
        // (timing measurements on a real machine).
        args: process.env.LGP_CHROMIUM_REAL_GPU === '1'
          ? ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required']
          : ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
             '--use-webgpu-adapter=swiftshader', '--ignore-gpu-blocklist',
             '--autoplay-policy=no-user-gesture-required'],
      };
      if (process.env.LGP_CHROMIUM_ARGS)
        launch.args.push(...process.env.LGP_CHROMIUM_ARGS.split(/\s+/).filter(Boolean));
      // LGP_CHROMIUM_PROFILE: a persistent profile directory. The default
      // context keeps OPFS in memory with a small quota, too small for a
      // whole game (copying a 4.4 GB romfs.bin fails with a size mismatch).
      if (process.env.LGP_CHROMIUM_PROFILE) {
        const context = await pw.chromium.launchPersistentContext(process.env.LGP_CHROMIUM_PROFILE,
          { ...launch, viewport: { width: 1400, height: 1000 } });
        browser = { newPage: async () => context.pages()[0] || context.newPage(), close: () => context.close() };
      } else {
        browser = await pw.chromium.launch(launch);
      }
    }
  } catch (e) {
    console.log(`skipped: ${opts.browser} did not start: ` + e.message.split('\n')[0]);
    code = SKIP;
  }
  if (browser) {
    const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
    if (opts.webgpuContextFails)
      await page.addInitScript((n) => {
        const getContext = HTMLCanvasElement.prototype.getContext;
        let left = n;
        HTMLCanvasElement.prototype.getContext = function (type, ...rest) {
          if (type === 'webgpu' && left > 0) { --left; return null; }
          return getContext.call(this, type, ...rest);
        };
      }, Number(opts.webgpuContextFails));
    const runs = [{ url: pageUrl(opts.args, opts.argv, opts.pick), expect: opts.expect, pick: opts.pick }];
    if (opts.thenArgs !== undefined)
      runs.push({ url: pageUrl(opts.thenArgs), expect: opts.thenExpect || opts.expect });
    code = 0;
    for (const run of runs) {
      const r = await runPage(page, run);
      if (r !== 0) { code = r; break; }
    }
    await browser.close();
  }
}
server.close();
process.exit(code);

// One page load: run the program and check its output. 0 ok, 1 failed, 77 skipped.
async function runPage(page, run) {
  const lines = [];
  let exitCode = null, noGpu = false, ready = false, lastLineAt = Date.now();
  // For the report of a black canvas: when the surface came up and the
  // program ended, seconds since the page load.
  const pageAt = Date.now();
  let surfaceAt = null, exitAt = null;
  const expect = new RegExp(run.expect);
  let matched = false;
  const pressAfter = opts.pressAfter ? new RegExp(opts.pressAfter) : null;
  let pressPending = false, pressed = false;
  const onConsole = (m) => {
    const t = m.text();
    if (pressAfter && !pressed && pressAfter.test(t)) pressPending = true;
    lines.push(t);
    lastLineAt = Date.now();
    // LGP_TIMESTAMPS=1: seconds since the test started in front of each line
    // (frame rates from the boot program's "N frames presented" lines).
    console.log('  | ' + (process.env.LGP_TIMESTAMPS === '1' ? ((Date.now() - T0) / 1000).toFixed(3) + ' ' : '') + t);
    if (expect.test(t)) matched = true;
    if (/^no WebGPU|has no WebGPU/.test(t)) noGpu = true;
    if (t === 'ready') ready = true;
    if (surfaceAt === null && /^webgpu: surface /.test(t)) surfaceAt = (Date.now() - pageAt) / 1000;
    const ex = /^exit code (-?\d+)/.exec(t);
    if (ex) { exitCode = Number(ex[1]); exitAt = (Date.now() - pageAt) / 1000; }
  };
  const onError = (e) => { lines.push('pageerror ' + e.message); console.log('  | pageerror ' + e.message); };
  page.on('console', onConsole);
  page.on('pageerror', onError);
  // A renderer that crashes looks like a hang: say when it happened.
  const onCrash = () => { lines.push('page crashed'); console.log(`  | page crashed after ${((Date.now() - pageAt) / 1000).toFixed(1)} s`); };
  page.on('crash', onCrash);
  console.log('page ' + run.url);
  await page.goto(run.url);
  const deadline = Date.now() + Number(opts.timeout);
  if (run.pick) {
    while (!ready && Date.now() < deadline && !opts.abortReason) await page.waitForTimeout(100);
    if (fetchPick) {
      const list = listPick(path.resolve(run.pick));
      for (const f of list) f.size = fs.statSync(path.join(path.resolve(run.pick), f.rel)).size;
      safariCommands.push({ prefill: list });
      prefilling = true;
    }
    else await pickFiles(page, path.resolve(run.pick), lines, deadline);
  }
  // Sample the canvas while the program runs: on exit the runtime releases
  // the WebGPU context and the canvas goes blank.
  // Two independent samples per round, the higher one counts: a screenshot of
  // the element and a drawImage copy of the canvas made in the page. Firefox
  // (CI, 2 of many runs) once returned only black screenshots of a canvas
  // whose WebGPU frames come from a worker (--gpu-thread) while the program
  // presented all its frames.
  let best = 0, size = '', bestShot = 0, bestCopy = 0, samples = 0, sampleErrors = 0, sampleError = '';
  let sampling = true;
  const timeline = [];  // per sample: time, both results, page state (printed when the canvas stays black)
  while (exitCode === null && Date.now() < deadline && !opts.abortReason) {
    await page.waitForTimeout(200);
    if (pressPending && !pressed) {
      pressed = true;
      for (const k of (opts.press || '').split(',').filter(Boolean)) await page.keyboard.down(k);
    }
    if (exitCode !== null || !opts.canvas || !sampling) continue;
    try {
      if (page.sampleCanvas) {
        const smp = page.sampleCanvas();
        if (smp) { best = Math.max(best, smp.lit); size = smp.size; }
        continue;
      }
      ++samples;
      const entry = { t: (Date.now() - pageAt) / 1000, shot: null, copy: null, state: '' };
      timeline.push(entry);
      // A main thread that hangs never answers: stop sampling, so the
      // deadline and the hang report below still come.
      const copy = await within(page.evaluate(() => {
        const c = document.getElementById('canvas');
        if (!c || !c.width) return null;
        const p = document.createElement('canvas');
        p.width = 64; p.height = 36;
        const g = p.getContext('2d', { willReadFrequently: true });
        g.drawImage(c, 0, 0, 64, 36);
        const d = g.getImageData(0, 0, 64, 36).data;
        let lit = 0;
        for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) ++lit;
        // A hidden or unfocused page may stop a worker's canvas from presenting.
        return { lit: lit / (64 * 36),
                 state: `${c.width}x${c.height} ${document.visibilityState}${document.hasFocus() ? ' focus' : ''}` };
      }).catch((e) => { ++sampleErrors; sampleError = String(e).split('\n')[0]; return null; }), 10000);
      if (copy === 'timeout') {  // wait for the exit until the deadline, without sampling
        console.log('  (the page did not answer a canvas sample within 10 s: sampling stopped)');
        sampling = false;
        continue;
      }
      if (copy !== null) {
        bestCopy = Math.max(bestCopy, copy.lit);
        entry.copy = copy.lit;
        entry.state = copy.state;
      }
      const { width, height, data } = decodePng(await page.locator('#canvas').screenshot({ timeout: 10000 }));
      let lit = 0;
      for (let i = 0; i < width * height * 4; i += 4) if (data[i] + data[i + 1] + data[i + 2] > 60) ++lit;
      bestShot = Math.max(bestShot, lit / (width * height));
      entry.shot = lit / (width * height);
      size = `${width}x${height}`;
    } catch (e) {
      ++sampleErrors;
      sampleError = String(e).split('\n')[0];
    }
    best = Math.max(bestShot, bestCopy);
  }
  // Output printed by a pthread reaches the page through the main thread and
  // can arrive just after onExit (seen in Safari): give it a moment.
  // Wait until the output has been quiet for 1.5 s (at most 10 s), also
  // after --expect matched: --lines/--absent/--fixture-expected need the rest.
  if (exitCode !== null) {
    const until = Date.now() + 10000;
    while (Date.now() < until && Date.now() - lastLineAt < 1500) await page.waitForTimeout(100);
  }
  page.off('console', onConsole);
  page.off('pageerror', onError);
  page.off('crash', onCrash);
  if (noGpu && !matched) {
    console.log('skipped: no WebGPU adapter in this browser');
    return SKIP;
  }
  if (exitCode === null) {
    console.log(`FAIL: no exit within ${opts.timeout} ms`);
    await reportHang(page);
    return 1;
  }
  if (exitCode !== 0 || !matched) {
    console.log(`FAIL: exit code ${exitCode}, expected output ${matched ? 'seen' : 'missing'}`);
    return 1;
  }
  if (!checkOutput(lines)) return 1;
  if (!opts.canvas) return 0;
  // The canvas must have shown rendered pixels, not its initial black.
  console.log(`canvas ${size}, at most ${(100 * best).toFixed(1)} % lit while running`);
  if (samples)
    console.log(`  ${samples} samples: screenshot ${(100 * bestShot).toFixed(1)} %, drawImage ` +
                `${(100 * bestCopy).toFixed(1)} %, ${sampleErrors} failed${sampleError ? ' (' + sampleError + ')' : ''}`);
  if (best > 0.25) return 0;
  // Enough to tell "samples missed the frames" from "no frame ever reached
  // the canvas" (Firefox, --gpu-thread, sporadic in CI).
  const pct = (v) => v === null ? '-' : (100 * v).toFixed(1) + ' %';
  console.log(`  ${opts.browser} ${page.context?.()?.browser()?.version() ?? ''}; surface at ` +
              `${surfaceAt === null ? '-' : surfaceAt.toFixed(1) + ' s'}, exit at ` +
              `${exitAt === null ? '-' : exitAt.toFixed(1) + ' s'}`);
  for (const e of timeline)
    console.log(`  sample at ${e.t.toFixed(1)} s: screenshot ${pct(e.shot)}, drawImage ${pct(e.copy)}` +
                (e.state ? ` (canvas ${e.state})` : ''));
  console.log('FAIL: canvas stayed (almost) black');
  return 1;
}

// Hands dir to the page's folder picker. The page logs "picked N files" from
// its change handler, then copies, autostarts and disables the input while
// the program runs. Playwright's Firefox sometimes does not return from
// setInputFiles after the change event fired and then waits for the (now
// disabled) input until its 30 s timeout, although the page got the files and
// the program already ran (seen in CI on browser.boot_basic_nro). So the
// page's own "picked" line counts as done; a setInputFiles error only fails
// the run when the page never saw the files.
async function pickFiles(page, dir, lines, deadline) {
  const picked = () => lines.some((l) => l.startsWith('picked '));
  let result;
  const call = page.locator('#pick-dir').setInputFiles(dir).then(() => { result = 'ok'; }, (e) => { result = e; });
  while (result === undefined && !picked() && Date.now() < deadline && !opts.abortReason)
    await Promise.race([call, page.waitForTimeout(100)]);
  if (result !== undefined && result !== 'ok' && !picked()) throw result;
  if (result === undefined && !picked()) console.log('note: setInputFiles has not returned and the page saw no files');
}

// --browser safari: one page object with the subset of Playwright's Page that
// runPage() uses, fed by the injected capture script.
async function runSafari() {
  if (process.platform !== 'darwin' || !fs.existsSync('/Applications/Safari.app')) {
    console.log('skipped: Safari only exists on macOS');
    return SKIP;
  }
  if (opts.needsUserGesture) {
    console.log('skipped: needs a real user gesture, synthetic events are not trusted in Safari');
    return SKIP;
  }
  if (Number(opts.webgpuContextFails) > 0) {
    console.log('skipped: --webgpu-context-fails needs Playwright (no init script in Safari)');
    return SKIP;
  }
  const maxMb = Number(process.env.LGP_SAFARI_MAX_MB || 10000);
  // Memory per Safari web content process as `top` reports it (the
  // footprint, which includes compressed and swapped pages; `ps` RSS does not
  // and stays low exactly when the Mac starts swapping).
  const contentProcesses = () => {
    try {
      const pids = execSync('ps -axo pid=,comm=').toString().split('\n')
        .filter((l) => l.includes('com.apple.WebKit.WebContent')).map((l) => Number(l.trim().split(/\s+/)[0]));
      if (pids.length === 0) return [];
      const unit = { B: 1 / 2 ** 20, K: 1 / 1024, M: 1, G: 1024 };
      return execSync(`top -l 1 -stats pid,mem ${pids.map((p) => '-pid ' + p).join(' ')}`).toString().split('\n')
        .map((l) => /^\s*(\d+)\s+([\d.]+)([BKMG])/.exec(l)).filter(Boolean)
        .map((m) => ({ pid: Number(m[1]), mb: Number(m[2]) * unit[m[3]] }));
    } catch { return []; }
  };
  // Web content processes that existed before this test, with their memory:
  // a failed run kills only processes that are new or grew by more than 1 GB
  // since, never an unrelated tab or WebKit app.
  const before = new Map(contentProcesses().map((p) => [p.pid, p.mb]));
  const ofThisTest = (p) => !before.has(p.pid) || p.mb - before.get(p.pid) > 1000;
  const safariRunning = () => { try { execSync('pgrep -x Safari'); return true; } catch { return false; } };
  // Safari 27 leaves the temporary file of every OPFS createWritable() (the
  // prefill) in its networking temp directory: ≈ 4.4 GB per game prefill,
  // which once filled the Mac's disk. Removed while Safari is not running.
  const cleanSafariTemp = () => {
    if (safariRunning()) return;
    try {
      const dir = path.join(execSync('getconf DARWIN_USER_TEMP_DIR').toString().trim(), 'com.apple.WebKit.Networking+com.apple.Safari');
      let freed = 0;
      for (const f of fs.readdirSync(dir))
        if (f.startsWith('FileSystemWritableStream')) {
          freed += fs.statSync(path.join(dir, f)).size;
          fs.rmSync(path.join(dir, f), { force: true });
        }
      if (freed > 0) console.log(`safari: removed ${Math.round(freed / 2 ** 20)} MB of left-over OPFS write files`);
    } catch {}
  };
  cleanSafariTemp();
  const launchedSafari = !safariRunning();
  let firstLoad = true;
  const handlers = { console: new Set(), pageerror: new Set(), crash: new Set() };  // crash: never reported
  let canvas = null;
  reportSink = (ev) => {
    if (ev.t === 'console' && ev.text.startsWith('prefill: copied')) prefilling = false;
    if (ev.t === 'console' && ev.text.startsWith('safari: ')) console.log(ev.text);
    else if (ev.t === 'console') for (const h of handlers.console) h({ text: () => ev.text });
    else if (ev.t === 'pageerror') for (const h of handlers.pageerror) h({ message: ev.text });
    else if (ev.t === 'canvas') canvas = ev;
  };
  const page = {
    on: (n, h) => handlers[n].add(h),
    off: (n, h) => handlers[n].delete(h),
    goto: async (url) => {
      // Reuse the tab of an earlier test if it asks for the next page within
      // 3 s; otherwise open a new one. Only local test pages are ever opened.
      if (!/^http:\/\/localhost:\d+\//.test(url)) throw new Error('refusing to open ' + url);
      const run = process.pid + '-' + Date.now();
      url += (url.includes('?') ? '&' : '?') + 'lgp_run=' + run;
      // LGP_SAFARI_KEEP_OPFS=1: keep what an earlier run copied (large game
      // data); then pass no --pick and let the page autostart from OPFS.
      if (firstLoad && process.env.LGP_SAFARI_KEEP_OPFS === '1') firstLoad = false;
      if (firstLoad) {
        const u = new URL(url);
        url = `${u.origin}/__lgp_fresh?to=${encodeURIComponent(u.pathname + u.search)}`;
        firstLoad = false;
      }
      safariNext = { url, run, taken: false };
      safariRun = run;
      for (let i = 0; i < 30 && !safariNext.taken; ++i) await new Promise((r) => setTimeout(r, 100));
      if (!safariNext.taken) {
        // No new tab while a game page of an earlier run still holds memory:
        // that page is hung (it did not ask for the next page), and each new
        // tab beside it once added another few GB.
        const hung = contentProcesses().filter((p) => p.mb > 2000);
        if (hung.length !== 0) {
          opts.abortReason = `a hung Safari game page (pid ${hung.map((p) => p.pid).join(', ')}) did not take the next page, not opening another tab`;
          return;
        }
        execSync(`open -a Safari ${JSON.stringify(url)}`);
        safariNext.taken = true;
      }
    },
    waitForTimeout: (ms) => new Promise((r) => setTimeout(r, ms)),
    sampleCanvas: () => { const c = canvas; canvas = null; return c; },
    keyboard: {
      down: async (k) => {
        const code = /^[a-z]$/i.test(k) ? 'Key' + k.toUpperCase() : /^[0-9]$/.test(k) ? 'Digit' + k : k;
        safariCommands.push({ key: { key: k, code } });
      },
    },
    locator: () => ({
      setInputFiles: async (dir) => { safariCommands.push({ pick: listPick(dir) }); },
    }),
  };
  const runs = [{ url: pageUrl(opts.args, opts.argv, opts.pick), expect: opts.expect, pick: opts.pick }];
  if (opts.thenArgs !== undefined) runs.push({ url: pageUrl(opts.thenArgs), expect: opts.thenExpect || opts.expect });
  // Memory watchdog: a hung game page once grew Safari to 38 GB and froze
  // the Mac. Web content processes above LGP_SAFARI_MAX_MB (default 10000)
  // are killed and the test fails; after a failed run, processes above
  // 2 GB (a game page; ordinary tabs stay far below) are killed too, so a
  // hung page neither keeps spinning nor holds its OPFS locks.
  let peak = 0, ticks = 0, silent = false;
  lastReport = 0;
  const watchdog = setInterval(() => {
    const procs = contentProcesses();
    // LGP_SAFARI_MEMLOG=1: the largest web content process every 10 s.
    if (process.env.LGP_SAFARI_MEMLOG === '1' && ++ticks % 5 === 0 && procs.length)
      console.log(`safari: memory ${Math.round(Math.max(...procs.map((p) => p.mb)))} MB at ${ticks * 2} s`);
    // The page reports every 200 ms from its main thread; silence means the
    // main thread is blocked (pthread output then stops too).
    if (lastReport !== 0 && Date.now() - lastReport > 5000 && !silent && !prefilling) {
      silent = true;
      let cpu = '';
      try { cpu = execSync('ps -axo pid=,%cpu=,comm= | grep WebKit.WebContent').toString().trim().split('\n').map((l) => l.trim().split(/\s+/).slice(0, 2).join(' ')).join(', '); } catch {}
      console.log(`safari: no report from the page for 5 s (main thread blocked?); web content pid/%cpu: ${cpu}`);
    } else if (silent && Date.now() - lastReport < 5000) {
      silent = false;
      console.log('safari: page reports again');
    }
    for (const p of procs) {
      peak = Math.max(peak, p.mb);
      if (p.mb > maxMb && !opts.abortReason) {
        opts.abortReason = `Safari web content process ${p.pid} at ${Math.round(p.mb)} MB (limit ${maxMb} MB), killed`;
        try { process.kill(p.pid, 'SIGKILL'); } catch {}
      }
    }
  }, 2000);
  let code = 0;
  for (const run of runs) {
    const r = await runPage(page, run);
    if (r !== 0) { code = r; break; }
  }
  clearInterval(watchdog);
  console.log(`safari: peak web content memory ${Math.round(peak)} MB`);
  if (opts.abortReason) {
    console.log('FAIL: ' + opts.abortReason);
    code = 1;
  }
  if (code !== 0 && code !== SKIP) {
    for (const p of contentProcesses())
      if (p.mb > 2000 && ofThisTest(p)) {
        console.log(`safari: killing web content process ${p.pid} (${Math.round(p.mb)} MB) of the failed run`);
        try { process.kill(p.pid, 'SIGKILL'); } catch {}
      }
  }
  // Clean up: the test tab closes itself, and Safari quits again if this
  // test started it (LGP_SAFARI_QUIT=0 keeps it open).
  safariNext = { url: 'close', run: '', only: safariRun, taken: false };
  for (let i = 0; i < 20 && !safariNext.taken; ++i) await new Promise((r) => setTimeout(r, 100));
  await new Promise((r) => setTimeout(r, 500));
  if (launchedSafari && process.env.LGP_SAFARI_QUIT !== '0') {
    try { execSync('pkill -TERM -x Safari'); } catch {}
    for (let i = 0; i < 50 && safariRunning(); ++i) await new Promise((r) => setTimeout(r, 100));
    cleanSafariTemp();
  }
  return code;
}

// --lines, --fixture-expected and --absent (same rules as tests/boot/run_boot.py).
function checkOutput(lines) {
  let ok = true;
  if (opts.lines) {
    const want = fs.readFileSync(opts.lines, 'utf8').split('\n').filter((l) => l.trim() && !l.startsWith('#'));
    let i = 0;
    for (const line of lines) {
      if (i < want.length && (line === want[i] || (want[i].endsWith('*') && line.startsWith(want[i].slice(0, -1))))) ++i;
    }
    if (i < want.length) {
      console.log(`FAIL: missing (in order) from line ${i + 1} of ${opts.lines}: ${want[i]}`);
      ok = false;
    }
  }
  if (opts.fixtureExpected) {
    const exp = fs.readFileSync(opts.fixtureExpected, 'utf8').split('\n');
    const expDebug = exp.filter((l) => l.startsWith('debug ')).map((l) => l.slice(6));
    const expExit = (exp.find((l) => l.startsWith('exit ')) || '').slice(5) || null;
    const gotDebug = lines.filter((l) => l.startsWith('debug: ')).map((l) => l.slice(7));
    const gotExit = (lines.find((l) => l.startsWith('exit: ')) || '').slice(6) || null;
    if (JSON.stringify(gotDebug) !== JSON.stringify(expDebug)) {
      console.log(`FAIL: debug output differs from ${opts.fixtureExpected}`);
      for (let n = 0; n < Math.max(gotDebug.length, expDebug.length); ++n)
        if (gotDebug[n] !== expDebug[n]) console.log(`  line ${n}: expected ${expDebug[n]}, got ${gotDebug[n]}`);
      ok = false;
    }
    if (gotExit !== expExit) {
      console.log(`FAIL: exit ${gotExit}, expected ${expExit}`);
      ok = false;
    }
  }
  for (const text of opts.absent) {
    if (lines.some((l) => l.includes(text))) {
      console.log(`FAIL: unexpected output ${text}`);
      ok = false;
    }
  }
  return ok;
}

// Minimal PNG decoder for Playwright screenshots (8-bit RGB/RGBA, no interlace).
function decodePng(buf) {
  const zlib = createRequire(import.meta.url)('node:zlib');
  let pos = 8, width = 0, height = 0, channels = 4;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos), type = buf.toString('ascii', pos + 4, pos + 8);
    const body = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0); height = body.readUInt32BE(4);
      channels = body[9] === 6 ? 4 : 3;
    } else if (type === 'IDAT') idat.push(body);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels, out = new Uint8Array(width * height * 4);
  let prev = new Uint8Array(stride);
  for (let y = 0; y < height; ++y) {
    const f = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const cur = new Uint8Array(stride);
    for (let x = 0; x < stride; ++x) {
      const a = x >= channels ? cur[x - channels] : 0, b = prev[x], c = x >= channels ? prev[x - channels] : 0;
      let p = line[x];
      if (f === 1) p += a; else if (f === 2) p += b; else if (f === 3) p += (a + b) >> 1;
      else if (f === 4) { const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c); p += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      cur[x] = p & 255;
    }
    for (let x = 0; x < width; ++x)
      for (let ch = 0; ch < 4; ++ch) out[(y * width + x) * 4 + ch] = ch < channels ? cur[x * channels + ch] : 255;
    prev = cur;
  }
  return { width, height, data: out };
}

// Files below dir for the injected pick: paths start with the folder name,
// like <input webkitdirectory>; follows symlinks.
function listPick(dir) {
  const top = path.basename(dir), list = [];
  // Follows symlinks; one that points back to an ancestor is a cycle.
  const walk = (d, rel, up) => {
    const real = fs.realpathSync(d);
    if (up.has(real)) throw new Error('symlink cycle at ' + d);
    up.add(real);
    for (const e of fs.readdirSync(d)) {
      const r = rel ? rel + '/' + e : e;
      if (fs.statSync(path.join(d, e)).isDirectory()) walk(path.join(d, e), r, up);
      else list.push({ path: top + '/' + r, rel: r });
    }
    up.delete(real);
  };
  walk(dir, '', new Set());
  return list;
}

// `p`, or 'timeout' after `ms`.
function within(p, ms) {
  return Promise.race([p, new Promise((r) => setTimeout(() => r('timeout'), ms))]);
}

// A page that does not exit: whether its main thread still runs tasks and,
// in Chromium, where its JavaScript/Wasm stands (DevTools pause). A blocked
// main thread never runs proxied calls; an idle one lost a callback.
async function reportHang(page) {
  if (typeof page.isClosed !== 'function') return;  // Safari without WebDriver: nothing to ask
  if (page.isClosed()) {
    console.log('hang: the page is closed');
    return;
  }
  const answer = await within(page.evaluate(() => 'answers').catch((e) => 'error ' + String(e).split('\n')[0]), 3000);
  console.log(`hang: main thread ${answer === 'answers' ? 'answers' : 'does not answer (' + answer + ')'}`);
  if (opts.browser !== 'chromium') return;
  let cdp;
  try {
    // Each step bounded: a renderer that hangs may not answer DevTools either.
    cdp = await within(page.context().newCDPSession(page), 3000);
    if (cdp === 'timeout') {
      cdp = undefined;
      console.log('hang: no DevTools session within 3 s');
      return;
    }
    const paused = new Promise((r) => cdp.once('Debugger.paused', r));
    if ((await within(cdp.send('Debugger.enable'), 3000)) === 'timeout' ||
        (await within(cdp.send('Debugger.pause'), 3000)) === 'timeout') {
      console.log('hang: DevTools did not answer within 3 s');
      return;
    }
    const ev = await within(paused, 3000);
    if (ev === 'timeout') {
      console.log('hang: main thread did not pause within 3 s (no JavaScript ran)');
      return;
    }
    console.log(`hang: main thread paused (${ev.reason}):`);
    for (const f of ev.callFrames.slice(0, 40))
      console.log(`  at ${f.functionName || '<anonymous>'} (${(f.url || '').split('/').pop()}:${f.location.lineNumber})`);
    await within(cdp.send('Debugger.resume').catch(() => {}), 2000);
  } catch (e) {
    console.log('hang: no DevTools report: ' + String(e).split('\n')[0]);
  } finally {
    if (cdp) {
      await within(cdp.send('Debugger.disable').catch(() => {}), 2000);
      await within(cdp.detach().catch(() => {}), 2000);
    }
  }
}

// browser.save_backup (P5.3): the page's save export and import in Chromium
// (Playwright; LGP_BROWSER=firefox or webkit picks another engine). Puts saves in the runtime's journaled OPFS layout into
// lgp/save, exports them with the page's button (download), checks the ZIP
// holds only the committed files in the neutral layout, imports it back
// through the page's file input and checks lgp/save; then imports a deflated
// ZIP as OS tools write it, and a damaged one, which must leave the saves
// untouched.
//
//   node save_backup_test.mjs --dir <dir with an lgp_add_web_page() page> --page <page.html>
//
// Exit codes: 0 ok, 1 failed, 77 skipped (no Playwright or browser).
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SKIP = 77;
const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const W = require(path.join(here, '../../platform/web/js/lgp_web.js'));
const opts = {};
for (let i = 2; i < process.argv.length; i += 2) opts[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
if (!opts.dir || !opts.page) { console.error('usage: save_backup_test.mjs --dir <dir> --page <page.html>'); process.exit(2); }

function loadPlaywright() {
  const roots = [];
  if (process.env.LGP_PLAYWRIGHT_DIR) roots.push(process.env.LGP_PLAYWRIGHT_DIR);
  try { roots.push(path.join(execSync('npm root -g', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(), 'playwright')); } catch {}
  for (const r of [...roots, 'playwright']) {
    try { return require(r); } catch {}
  }
  return null;
}

const acc = '010003f003a34000/account/11112222333344445555666677778888';
const OPFS_LAYOUT = {
  [`${acc}/.lgp-journal`]: '1\n',
  [`${acc}/.lgp-0/savedata.bin`]: 'old',
  [`${acc}/.lgp-1/savedata.bin`]: 'committed',
  [`${acc}/.lgp-1/sub/x.bin`]: 'x',
  [`${acc}/.lgp-work/savedata.bin`]: 'uncommitted',
  '010003f003a34000/temporary/t.bin': 't',
};
const NEUTRAL = {
  [`${acc}/savedata.bin`]: 'committed',
  [`${acc}/sub/x.bin`]: 'x',
  '010003f003a34000/temporary/t.bin': 't',
};

function deflatedZip(files) {
  const locals = [], centrals = [];
  let offset = 0;
  for (const [name, text] of Object.entries(files)) {
    const data = Buffer.from(text), comp = zlib.deflateRawSync(data), nm = Buffer.from(name), crc = W.crc32(data);
    const local = Buffer.alloc(30), central = Buffer.alloc(46);
    local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4); local.writeUInt16LE(8, 8);
    local.writeUInt32LE(crc, 14); local.writeUInt32LE(comp.length, 18); local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nm.length, 26);
    central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(20, 4); central.writeUInt16LE(20, 6);
    central.writeUInt16LE(8, 10); central.writeUInt32LE(crc, 16); central.writeUInt32LE(comp.length, 20);
    central.writeUInt32LE(data.length, 24); central.writeUInt16LE(nm.length, 28); central.writeUInt32LE(offset, 42);
    locals.push(local, nm, comp);
    centrals.push(central, nm);
    offset += 30 + nm.length + comp.length;
  }
  const cd = Buffer.concat(centrals), end = Buffer.alloc(22);
  const n = Object.keys(files).length;
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(n, 8); end.writeUInt16LE(n, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}

const root = path.resolve(opts.dir);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((req, res) => {
  const file = path.resolve(root, '.' + decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.writeHead(200, {
    'Content-Type': types[path.extname(file)] || 'application/octet-stream',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cache-Control': 'no-store',
  });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));

let failures = 0;
function check(ok, what) {
  if (!ok) { console.log('FAIL: ' + what); ++failures; }
}
const same = (a, b) => JSON.stringify(Object.entries(a).sort()) === JSON.stringify(Object.entries(b).sort());

let code = 0;
const pw = loadPlaywright();
let browser = null;
if (!pw) {
  console.log('skipped: Playwright not found (set LGP_PLAYWRIGHT_DIR)');
  code = SKIP;
} else {
  try {
    // LGP_BROWSER=firefox|webkit as in browser_test.mjs (safari is not
    // automatable here: the test drives downloads and file inputs).
    const name = process.env.LGP_BROWSER || 'chromium';
    if (!['chromium', 'firefox', 'webkit'].includes(name)) {
      console.log(`skipped: ${name} cannot run this test (Playwright only)`);
      process.exit(SKIP);
    }
    browser = await pw[name].launch({ headless: true });
  } catch (e) {
    console.log('skipped: browser did not start: ' + e.message.split('\n')[0]);
    code = SKIP;
  }
}
if (browser) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lgp-save-backup-'));
  const page = await browser.newPage({ acceptDownloads: true });
  const lines = [];
  page.on('console', (m) => { lines.push(m.text()); console.log('  | ' + m.text()); });
  page.on('pageerror', (e) => { lines.push('pageerror ' + e.message); console.log('  | pageerror ' + e.message); });
  const waitLine = async (re, from) => {
    for (let t = 0; t < 300; ++t) {
      const hit = lines.slice(from).find((l) => re.test(l));
      if (hit) return hit;
      await page.waitForTimeout(100);
    }
    return null;
  };
  // lgp/save as a {path: text} map.
  const readSaves = () => page.evaluate(async () => {
    const out = {};
    for (const f of await window.LgpWeb.readTree(window.LgpWeb.SAVE_DIR)) out[f.path] = new TextDecoder().decode(f.data);
    return out;
  });

  await page.goto(`http://localhost:${server.address().port}/${opts.page}?noconfirm=1`);
  check(await waitLine(/^ready$/, 0), 'page ready');
  await page.evaluate(async (files) => {
    await window.LgpWeb.clearSaves();
    let top = await navigator.storage.getDirectory();
    for (const p of window.LgpWeb.SAVE_DIR) top = await top.getDirectoryHandle(p, { create: true });
    for (const [p, text] of Object.entries(files)) {
      const parts = p.split('/');
      let dir = top;
      for (const d of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(d, { create: true });
      const w = await (await dir.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
      await w.write(text);
      await w.close();
    }
  }, OPFS_LAYOUT);

  // Export through the button: a download with the committed files only.
  const [download] = await Promise.all([page.waitForEvent('download'), page.click('#export-saves')]);
  check(/^lgp-saves-\d{8}-\d{6}\.zip$/.test(download.suggestedFilename()), 'backup file name ' + download.suggestedFilename());
  const zipPath = path.join(tmp, 'backup.zip');
  await download.saveAs(zipPath);
  const exported = {};
  for (const f of await W.unzip(new Uint8Array(fs.readFileSync(zipPath)))) exported[f.path] = new TextDecoder().decode(f.data);
  check(same(exported, NEUTRAL), 'exported ZIP: ' + JSON.stringify(exported));

  // Import it back: lgp/save holds the neutral layout (the runtime takes it over on mount).
  await page.evaluate(() => window.LgpWeb.clearSaves());
  let mark = lines.length;
  await page.locator('#import-saves').setInputFiles(zipPath);
  check(await waitLine(/^saves imported: 3 files/, mark), 'import of the export');
  check(same(await readSaves(), NEUTRAL), 'saves after import: ' + JSON.stringify(await readSaves()));

  // A deflated ZIP of a zipped folder replaces them.
  const osZip = path.join(tmp, 'from-explorer.zip');
  fs.writeFileSync(osZip, deflatedZip({ [`backup/${acc}/savedata.bin`]: 'edited with a tool' }));
  mark = lines.length;
  await page.locator('#import-saves').setInputFiles(osZip);
  check(await waitLine(/^saves imported: 1 files/, mark), 'import of a deflated ZIP');
  const edited = { [`${acc}/savedata.bin`]: 'edited with a tool' };
  check(same(await readSaves(), edited), 'saves after deflated import: ' + JSON.stringify(await readSaves()));

  // A damaged ZIP fails before anything is replaced.
  const damaged = fs.readFileSync(zipPath);
  damaged[30 + damaged.readUInt16LE(26)] ^= 0xFF;  // first byte of the first file's data
  const badZip = path.join(tmp, 'damaged.zip');
  fs.writeFileSync(badZip, damaged);
  mark = lines.length;
  await page.locator('#import-saves').setInputFiles(badZip);
  check(await waitLine(/^import failed: /, mark), 'damaged ZIP refused');
  check(same(await readSaves(), edited), 'saves untouched by the damaged ZIP');
  check(!lines.some((l) => l.startsWith('pageerror')), 'no page errors');

  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
  code = failures === 0 ? 0 : 1;
  console.log(failures === 0 ? 'save backup: ok' : `save backup: ${failures} check(s) failed`);
}
server.close();
process.exit(code);

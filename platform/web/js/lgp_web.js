// Browser front end of lgp programs (P6): picks the user's own game data
// from a local folder, copies it into the Origin Private File System (OPFS)
// and starts the Emscripten program, which reads it through WasmFS at /opfs
// (platform/web/include/lgp/platform/web/opfs.hpp). Nothing is uploaded or
// fetched from a server: the files go from the local disk into this
// browser's private storage for this origin only.
//
// Layout in OPFS (shared with opfs.hpp):
//   lgp/data/exefs/...   modules: main, sdk, subsdk*, rtld, main.npdm (*.nso/*.nro for homebrew)
//   lgp/data/romfs/...   RomFS tree, or lgp/data/romfs.bin
//   lgp/data/systemdata/ system data archives (fonts), if provided
//   lgp/data/pipeline-seed.lgppipes  a pipeline cache from another device or
//                        browser (a .lgppipes file in the picked folder), if
//                        provided; the program takes its records over
//   lgp/save/...         save data, written by the program; each save is a
//                        journaled directory (runtime/hle/src/save_data.hpp):
//                        .lgp-journal names the committed slot .lgp-0 or .lgp-1
//
// Save backups (P5.3) are ZIP files in the neutral layout: per save only the
// committed files, under the save's path (e.g. 0100.../account/<uid>/...), as
// the game sees them. Import replaces lgp/save with them; the runtime takes
// files lying directly in a save directory over on its next mount.
//
// Classic script (no module) so the page works from file-less static
// serving; in Node (tests/web/lgp_web_test.mjs) it exports the pure parts.
(function (root) {
  'use strict';

  const DATA_DIR = ['lgp', 'data'];
  const SAVE_DIR = ['lgp', 'save'];
  // ExeFS module names (Horizon), plus homebrew module files.
  const MODULE_NAME = /^(main|sdk|rtld|subsdk\d+|main\.npdm)$/;
  const MODULE_EXT = /\.(nso|nro)$/i;
  const SECTIONS = ['exefs', 'romfs', 'systemdata'];

  // Same rules as lgp::platform::normalize_path, applied to each component.
  function validPart(p) {
    return p !== '' && p !== '.' && p !== '..' && !/[\\:\0]/.test(p);
  }

  // Path relative to lgp/data for a picked file, or null to leave it out.
  // relPath starts with the picked folder's name (webkitRelativePath style);
  // a bare file name is a file picked on its own. Only known parts of a
  // dump are taken: everything else (the NSP itself, keys, other files in
  // the folder) stays where it is.
  function classify(relPath) {
    const parts = String(relPath).split('/').filter((p) => p !== '');
    if (parts.length === 0 || !parts.every(validPart)) return null;
    for (let i = 0; i < parts.length - 1; ++i) {
      const lower = parts[i].toLowerCase();
      if (SECTIONS.includes(lower)) return [lower, ...parts.slice(i + 1)].join('/');
    }
    const name = parts[parts.length - 1];
    const topLevel = parts.length <= 2;  // directly in the picked folder, or picked alone
    if (topLevel && name.toLowerCase() === 'romfs.bin') return 'romfs.bin';
    if (topLevel && /\.lgppipes$/i.test(name)) return 'pipeline-seed.lgppipes';
    if (topLevel && (MODULE_NAME.test(name) || MODULE_EXT.test(name))) return 'exefs/' + name;
    return null;
  }

  // entries: [{path, file}] -> what to copy and what is left out.
  function planCopy(entries) {
    const plan = { copies: [], ignored: [], bytes: 0, modules: [], romfs: false, romfsBin: false };
    const seen = new Set();
    for (const e of entries) {
      const dest = classify(e.path);
      if (dest === null || seen.has(dest)) {
        plan.ignored.push(e.path);
        continue;
      }
      seen.add(dest);
      const size = e.file ? e.file.size : (e.size || 0);
      plan.copies.push({ dest, file: e.file, size });
      plan.bytes += size;
      if (dest.startsWith('exefs/') && dest.split('/').length === 2) plan.modules.push(dest.slice(6));
      if (dest.startsWith('romfs/')) plan.romfs = true;
      if (dest === 'romfs.bin') plan.romfsBin = true;
    }
    plan.modules.sort();
    plan.copies.sort((a, b) => (a.dest < b.dest ? -1 : a.dest > b.dest ? 1 : 0));
    return plan;
  }

  function formatBytes(n) {
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; ++i; }
    return (i === 0 ? String(n) : n.toFixed(1)) + ' ' + units[i];
  }

  // argv for the program: defaults, ?args=... appended, ?argv=... instead.
  function programArgs(defaults, query) {
    const split = (s) => String(s || '').split(' ').filter(Boolean);
    if (query.has('argv')) return split(query.get('argv'));
    return split(defaults).concat(split(query.get('args')));
  }

  // ---- save backups (P5.3) ----

  const JOURNAL = '.lgp-journal';
  const JOURNAL_TMP = '.lgp-journal.tmp';

  // Slot a journal file names ('0' or '1'), or null.
  function journalSlot(data) {
    if (!data || data.length === 0 || (data[0] !== 0x30 && data[0] !== 0x31)) return null;
    return String.fromCharCode(data[0]);
  }

  // files: [{path, data}] relative to lgp/save, in the OPFS layout ->
  // [{path, data}] in the neutral layout: a journaled save contributes the
  // files of its committed slot under its own path, plus files lying directly
  // in it (the runtime takes those over on mount, so they win). Working copy,
  // spare slot and journal stay out. Files outside journaled saves pass.
  function savesForExport(files) {
    const roots = new Map();  // save path ('' for the top) -> committed slot
    for (const f of files) {
      const parts = f.path.split('/');
      const name = parts.pop();
      const dir = parts.join('/');
      if (name === JOURNAL) { const s = journalSlot(f.data); if (s !== null) roots.set(dir, s); }
    }
    for (const f of files) {  // a temporary journal counts only without the real one
      const parts = f.path.split('/');
      const name = parts.pop();
      const dir = parts.join('/');
      if (name === JOURNAL_TMP && !roots.has(dir)) { const s = journalSlot(f.data); if (s !== null) roots.set(dir, s); }
    }
    const committed = new Map(), loose = new Map();
    for (const f of files) {
      const parts = f.path.split('/');
      let root = null;
      for (let i = parts.length - 1; i >= 0; --i) {
        const dir = parts.slice(0, i).join('/');
        if (roots.has(dir)) { root = { dir, depth: i }; break; }
      }
      if (root === null) { loose.set(f.path, f.data); continue; }
      const first = parts[root.depth];
      const rest = parts.slice(root.depth + 1);
      const prefix = root.dir ? root.dir + '/' : '';
      if (first === '.lgp-' + roots.get(root.dir) && rest.length) committed.set(prefix + rest.join('/'), f.data);
      else if (!first.startsWith('.lgp-')) loose.set(f.path, f.data);
    }
    for (const [p, d] of loose) committed.set(p, d);
    return Array.from(committed, ([path, data]) => ({ path, data }))
      .sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  }

  // entries of a picked backup ZIP -> {files, ignored}: paths checked like
  // normalize_path, a common top folder (someone zipped the folder itself)
  // removed, a raw copy of the OPFS layout mapped to the neutral one.
  function planSaveImport(entries) {
    const ignored = [];
    let files = [];
    for (const e of entries) {
      const parts = String(e.path).replace(/^\.\//, '').split('/').filter((p) => p !== '');
      if (parts.length === 0 || String(e.path).startsWith('/') || !parts.every(validPart)) ignored.push(e.path);
      else files.push({ parts, data: e.data });
    }
    const saveTop = (p) => /^[0-9a-f]{16}$/i.test(p) || p === 'system';
    const top = files.length ? files[0].parts[0] : null;
    if (top !== null && !saveTop(top) && files.every((f) => f.parts.length > 1 && f.parts[0] === top))
      files = files.map((f) => ({ parts: f.parts.slice(1), data: f.data }));
    return { files: savesForExport(files.map((f) => ({ path: f.parts.join('/'), data: f.data }))), ignored };
  }

  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; ++n) {
      let c = n;
      for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(data) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < data.length; ++i) c = CRC_TABLE[(c ^ data[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  // [{path, data}] -> ZIP (stored, UTF-8 names; ZIP64 not needed for saves).
  function zipStore(files, date) {
    const d = date || new Date();
    const dosTime = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
    const dosDate = ((Math.max(d.getFullYear(), 1980) - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
    const enc = new TextEncoder();
    const locals = [], centrals = [];
    let offset = 0;
    for (const f of files) {
      const name = enc.encode(f.path), crc = crc32(f.data), size = f.data.length;
      const local = new DataView(new ArrayBuffer(30));
      local.setUint32(0, 0x04034b50, true); local.setUint16(4, 20, true); local.setUint16(6, 0x800, true);
      local.setUint16(8, 0, true); local.setUint16(10, dosTime, true); local.setUint16(12, dosDate, true);
      local.setUint32(14, crc, true); local.setUint32(18, size, true); local.setUint32(22, size, true);
      local.setUint16(26, name.length, true); local.setUint16(28, 0, true);
      const central = new DataView(new ArrayBuffer(46));
      central.setUint32(0, 0x02014b50, true); central.setUint16(4, 20, true); central.setUint16(6, 20, true);
      central.setUint16(8, 0x800, true); central.setUint16(10, 0, true); central.setUint16(12, dosTime, true);
      central.setUint16(14, dosDate, true); central.setUint32(16, crc, true); central.setUint32(20, size, true);
      central.setUint32(24, size, true); central.setUint16(28, name.length, true);
      central.setUint32(42, offset, true);
      locals.push(new Uint8Array(local.buffer), name, f.data);
      centrals.push(new Uint8Array(central.buffer), name);
      offset += 30 + name.length + size;
    }
    const cdSize = centrals.reduce((n, c) => n + c.length, 0);
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true);
    end.setUint32(12, cdSize, true); end.setUint32(16, offset, true);
    const parts = [...locals, ...centrals, new Uint8Array(end.buffer)];
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  }

  async function inflateRaw(data) {
    const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  // ZIP bytes -> [{path, data}] (files only). Stored and deflated entries,
  // as OS tools write them; throws on anything else or a damaged archive.
  async function unzip(bytes) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let eocd = -1;
    for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 0xFFFF); --i)
      if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error('not a ZIP file');
    const count = v.getUint16(eocd + 10, true);
    let p = v.getUint32(eocd + 16, true);
    const dec = new TextDecoder();
    const out = [];
    for (let n = 0; n < count; ++n) {
      if (p + 46 > bytes.length || v.getUint32(p, true) !== 0x02014b50) throw new Error('damaged ZIP directory');
      const flags = v.getUint16(p + 8, true), method = v.getUint16(p + 10, true), crc = v.getUint32(p + 16, true);
      const csize = v.getUint32(p + 20, true), usize = v.getUint32(p + 24, true);
      const nlen = v.getUint16(p + 28, true), xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true);
      const local = v.getUint32(p + 42, true);
      const name = dec.decode(bytes.subarray(p + 46, p + 46 + nlen));
      p += 46 + nlen + xlen + clen;
      if (name.endsWith('/')) continue;
      if (flags & 1) throw new Error('encrypted ZIP entry: ' + name);
      if (csize === 0xFFFFFFFF || usize === 0xFFFFFFFF) throw new Error('ZIP64 is not supported: ' + name);
      if (local + 30 > bytes.length || v.getUint32(local, true) !== 0x04034b50) throw new Error('damaged ZIP entry: ' + name);
      const start = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
      if (start + csize > bytes.length) throw new Error('truncated ZIP entry: ' + name);
      const raw = bytes.subarray(start, start + csize);
      let data;
      if (method === 0) data = raw.slice();
      else if (method === 8) data = await inflateRaw(raw);
      else throw new Error(`ZIP method ${method} is not supported: ${name}`);
      if (data.length !== usize || crc32(data) !== crc) throw new Error('ZIP entry is damaged (CRC): ' + name);
      out.push({ path: name, data });
    }
    return out;
  }

  function saveBackupName(d) {
    const z = (n) => String(n).padStart(2, '0');
    return `lgp-saves-${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}${z(d.getSeconds())}.zip`;
  }

  // ---- browser only below ----

  function hasOpfs() {
    return typeof navigator !== 'undefined' && !!(navigator.storage && navigator.storage.getDirectory);
  }

  async function dirHandle(parts, create) {
    let dir = await navigator.storage.getDirectory();
    for (const p of parts) dir = await dir.getDirectoryHandle(p, { create });
    return dir;
  }

  // What lgp/data holds now: {files, bytes, modules, romfs, romfsBin}.
  async function inventory() {
    const inv = { files: 0, bytes: 0, modules: [], romfs: false, romfsBin: false };
    let data;
    try { data = await dirHandle(DATA_DIR, false); } catch { return inv; }
    async function walk(dir, path) {
      for await (const [name, h] of dir.entries()) {
        const p = path ? path + '/' + name : name;
        if (h.kind === 'directory') {
          if (p === 'romfs') inv.romfs = true;
          await walk(h, p);
        } else {
          inv.files += 1;
          inv.bytes += (await h.getFile()).size;
          if (path === 'exefs') inv.modules.push(name);
          if (p === 'romfs.bin') inv.romfsBin = true;
        }
      }
    }
    await walk(data, '');
    inv.modules.sort();
    return inv;
  }

  async function removeDir(parts) {
    try {
      const parent = await dirHandle(parts.slice(0, -1), false);
      await parent.removeEntry(parts[parts.length - 1], { recursive: true });
    } catch (e) {
      if (e && e.name !== 'NotFoundError') throw e;
    }
  }
  const clearData = () => removeDir(DATA_DIR);
  const clearSaves = () => removeDir(SAVE_DIR);

  // Files of an <input type=file webkitdirectory multiple>.
  function entriesFromFileList(list) {
    return Array.from(list, (f) => ({ path: f.webkitRelativePath || f.name, file: f }));
  }

  // Runs opfs_copy_worker.js on {clear, copies}. The copy runs in a worker:
  // synchronous OPFS access handles exist only there, and they work in
  // Chrome, Firefox and Safari alike.
  function runCopyWorker(message, onProgress, workerUrl) {
    return new Promise((resolve, reject) => {
      const worker = new Worker(workerUrl || 'opfs_copy_worker.js');
      worker.onmessage = (e) => {
        const m = e.data;
        if (m.type === 'progress') { if (onProgress) onProgress(m); return; }
        worker.terminate();
        if (m.type === 'done') resolve(m);
        else reject(new Error(m.message));
      };
      worker.onerror = (e) => { worker.terminate(); reject(new Error(e.message || 'copy worker failed')); };
      worker.postMessage(message);
    });
  }

  // Replaces lgp/data with the planned files (saves stay).
  function copyToOpfs(plan, onProgress, workerUrl) {
    return runCopyWorker({
      clear: DATA_DIR,
      copies: plan.copies.map((c) => ({ dest: DATA_DIR.join('/') + '/' + c.dest, file: c.file })),
    }, onProgress, workerUrl);
  }

  // Every file below an OPFS directory: [{path, data}], [] if it is missing.
  async function readTree(parts) {
    let top;
    try { top = await dirHandle(parts, false); } catch { return []; }
    const out = [];
    async function walk(dir, path) {
      for await (const [name, h] of dir.entries()) {
        const p = path ? path + '/' + name : name;
        if (h.kind === 'directory') await walk(h, p);
        else out.push({ path: p, data: new Uint8Array(await (await h.getFile()).arrayBuffer()) });
      }
    }
    await walk(top, '');
    return out;
  }

  // The committed saves as a backup ZIP: {zip, files, name}.
  async function exportSaves(date) {
    const files = savesForExport(await readTree(SAVE_DIR));
    const d = date || new Date();
    return { zip: zipStore(files, d), files: files.length, name: saveBackupName(d) };
  }

  // Replaces lgp/save with the saves in a backup ZIP (a File or Blob).
  // Nothing is touched when the archive is damaged or holds no save file.
  async function importSaves(blob, workerUrl) {
    const plan = planSaveImport(await unzip(new Uint8Array(await blob.arrayBuffer())));
    if (plan.files.length === 0) throw new Error('the ZIP file holds no saves');
    await runCopyWorker({
      clear: SAVE_DIR,
      copies: plan.files.map((f) => ({ dest: SAVE_DIR.join('/') + '/' + f.path, file: new Blob([f.data]) })),
    }, null, workerUrl);
    return plan;
  }

  // Free space check before copying several GiB; also asks the browser to
  // keep the data (otherwise it may evict it under storage pressure).
  // Firefox answers persist() only after the user decided in a permission
  // prompt, so it is awaited at most persistWaitMs: persisted is then null
  // (still asking) and the copy goes ahead; onPersisted gets the late answer.
  async function prepareStorage(bytes, { persistWaitMs = 2000, onPersisted } = {}) {
    const result = { persisted: false, enough: true, available: null };
    if (navigator.storage && navigator.storage.persist) {
      const asked = navigator.storage.persist().then((v) => !!v, () => false);
      const pending = Symbol('pending');
      const first = await Promise.race([asked, new Promise((r) => setTimeout(() => r(pending), persistWaitMs))]);
      if (first === pending) {
        result.persisted = null;
        if (onPersisted) asked.then(onPersisted);
      } else {
        result.persisted = first;
      }
    }
    if (navigator.storage && navigator.storage.estimate) {
      try {
        const est = await navigator.storage.estimate();
        const inv = await inventory();  // replaced by the copy, so it counts as free
        result.available = est.quota - est.usage + inv.bytes;
        result.enough = result.available >= bytes;
      } catch {}
    }
    return result;
  }

  // Loads the program script with Module prepared; main() runs right away.
  function startProgram({ script, args, canvas, print, onExit }) {
    root.Module = {
      arguments: args,
      canvas,
      print,
      printErr: print,
      onExit: (code) => { if (onExit) onExit(code); },
      onAbort: (what) => { print('aborted: ' + what); if (onExit) onExit(134); },
    };
    const tag = document.createElement('script');
    tag.src = script;
    tag.onerror = () => { print('cannot load ' + script); if (onExit) onExit(127); };
    document.body.appendChild(tag);
  }

  const api = {
    DATA_DIR, SAVE_DIR, classify, planCopy, formatBytes, programArgs,
    savesForExport, planSaveImport, crc32, zipStore, unzip, saveBackupName,
    hasOpfs, inventory, clearData, clearSaves, entriesFromFileList,
    copyToOpfs, prepareStorage, startProgram, readTree, exportSaves, importSaves,
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.LgpWeb = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);

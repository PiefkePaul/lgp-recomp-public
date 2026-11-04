// Copies picked local files into OPFS (lgp_web.js copyToOpfs). Runs in a
// dedicated worker because synchronous access handles exist only there;
// they write in place, without the temporary swap file createWritable()
// keeps, which matters for a RomFS of several GiB.
//
// In:  {clear: [dir parts], copies: [{dest: 'lgp/data/exefs/main', file: File}]}
// Out: {type: 'progress', files, bytes, total, current}
//      {type: 'done', files, bytes} | {type: 'error', message}
'use strict';

const CHUNK = 8 << 20;
const PROGRESS_EVERY_MS = 250;

async function dirHandle(root, parts, create) {
  let dir = root;
  for (const p of parts) dir = await dir.getDirectoryHandle(p, { create });
  return dir;
}

async function removeDir(root, parts) {
  if (!parts || parts.length === 0) return;
  try {
    const parent = await dirHandle(root, parts.slice(0, -1), false);
    await parent.removeEntry(parts[parts.length - 1], { recursive: true });
  } catch (e) {
    if (e && e.name !== 'NotFoundError') throw e;
  }
}

self.onmessage = async (e) => {
  const { clear, copies } = e.data;
  const total = copies.reduce((n, c) => n + c.file.size, 0);
  let files = 0, bytes = 0, last = 0;
  const progress = (current, force) => {
    const now = Date.now();
    if (!force && now - last < PROGRESS_EVERY_MS) return;
    last = now;
    self.postMessage({ type: 'progress', files, bytes, total, current });
  };
  try {
    const root = await navigator.storage.getDirectory();
    await removeDir(root, clear);
    for (const c of copies) {
      const parts = c.dest.split('/');
      const name = parts.pop();
      const dir = await dirHandle(root, parts, true);
      const handle = await (await dir.getFileHandle(name, { create: true })).createSyncAccessHandle();
      try {
        handle.truncate(0);
        for (let off = 0; off < c.file.size; off += CHUNK) {
          const buf = new Uint8Array(await c.file.slice(off, off + CHUNK).arrayBuffer());
          let written = 0;
          while (written < buf.length) {
            const n = handle.write(buf.subarray(written), { at: off + written });
            if (n <= 0) throw new Error('write failed: ' + c.dest);
            written += n;
          }
          bytes += buf.length;
          progress(c.dest, false);
        }
        if (handle.getSize() !== c.file.size) throw new Error('size mismatch after copy: ' + c.dest);
        handle.flush();
      } finally {
        handle.close();
      }
      files += 1;
      progress(c.dest, false);
    }
    progress('', true);
    self.postMessage({ type: 'done', files, bytes });
  } catch (err) {
    const msg = err && err.name === 'QuotaExceededError'
      ? 'not enough browser storage (quota exceeded)'
      : String((err && err.message) || err);
    self.postMessage({ type: 'error', message: msg });
  }
};

// Test data for the OPFS browser tests (P6.1): a picked "dump" folder with
// the parts the page copies (exefs/, romfs/, systemdata/) and files it must
// leave out (an NSP, a key file, notes). Contents are made up and follow one
// formula, shared with opfs_storage_test.cpp: byte i of the file with
// destination path P is ((i * 0x9E3779B1 + fnv1a32(P)) mod 2^32) >> 24.
//
//   node test_data.mjs <out dir>     -> <out dir>/lgp_test_dump/...
import fs from 'node:fs';
import path from 'node:path';

// Destination path below lgp/data -> size. Keep in sync with kFiles in
// opfs_storage_test.cpp.
export const FILES = {
  'exefs/main': 65536,
  'exefs/main.npdm': 1024,
  'exefs/sdk': 100003,
  'romfs/bin/large.dat': 9 * 1048576 + 7,  // more than one copy chunk (8 MiB)
  'romfs/deep/a/b/c.txt': 100,
  'romfs/empty.bin': 0,
  'systemdata/0100000000000811': 2048,
};
// Files in the picked folder the page must not copy.
export const IGNORED = { 'Game.nsp': 4096, 'prod.keys': 64, 'notes/readme.txt': 32 };

export function fnv1a32(s) {
  let h = 0x811c9dc5;
  for (const c of Buffer.from(s, 'utf8')) h = Math.imul(h ^ c, 0x01000193) >>> 0;
  return h;
}

export function content(dest, size) {
  const h = fnv1a32(dest), out = Buffer.alloc(size);
  for (let i = 0; i < size; ++i) out[i] = ((Math.imul(i, 0x9e3779b1) + h) >>> 0) >>> 24;
  return out;
}

export function writeDump(outDir) {
  const dump = path.join(outDir, 'lgp_test_dump');
  fs.rmSync(dump, { recursive: true, force: true });
  for (const [dest, size] of Object.entries({ ...FILES, ...IGNORED })) {
    const file = path.join(dump, dest);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content(dest, size));
  }
  return dump;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  if (process.argv.length < 3) { console.error('usage: test_data.mjs <out dir>'); process.exit(2); }
  console.log(writeDump(process.argv[2]));
}

// web.page_logic (P6.1): the pure parts of platform/web/js/lgp_web.js, in
// Node. Above all: from a picked folder only the parts of a dump the runtime
// reads are copied, under the OPFS layout opfs.hpp expects; the NSP, keys
// and anything else stay out.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const here = path.dirname(fileURLToPath(import.meta.url));
const W = createRequire(import.meta.url)(path.join(here, '../../platform/web/js/lgp_web.js'));

const cases = [
  // tools/extract layout, picked as the "extracted" folder
  ['extracted/exefs/main', 'exefs/main'],
  ['extracted/exefs/main.npdm', 'exefs/main.npdm'],
  ['extracted/exefs/subsdk1', 'exefs/subsdk1'],
  ['extracted/romfs/bin/field/a.gfpak', 'romfs/bin/field/a.gfpak'],
  ['extracted/systemdata/0100000000000811', 'systemdata/0100000000000811'],
  // the exefs or romfs folder picked itself
  ['exefs/sdk', 'exefs/sdk'],
  ['romfs/x/y.bin', 'romfs/x/y.bin'],
  ['ExeFS/main', 'exefs/main'],
  // first section wins: a romfs subfolder called exefs stays in romfs
  ['dump/romfs/exefs/main', 'romfs/exefs/main'],
  // modules and romfs.bin directly in the picked folder
  ['dump/main', 'exefs/main'],
  ['dump/rtld', 'exefs/rtld'],
  ['dump/basic.nro', 'exefs/basic.nro'],
  ['dump/romfs.bin', 'romfs.bin'],
  ['basic.nso', 'exefs/basic.nso'],
  // a pipeline cache from another device as the seed
  ['dump/pc-webgpu-pipelines.lgppipes', 'pipeline-seed.lgppipes'],
  ['webgpu-pipelines.LGPPIPES', 'pipeline-seed.lgppipes'],
  ['dump/cache/webgpu-pipelines.lgppipes', null],
  // left out
  ['dump/Spiel-bak.nsp', null],
  ['dump/prod.keys', null],
  ['dump/title.keys', null],
  ['dump/nsp/abc.nca', null],
  ['dump/notes/main', null],          // module name, but not in exefs/ or the top level
  ['dump/exefs', null],               // a file named like the section
  ['dump/romfs/../prod.keys', null],  // invalid components
  ['dump/romfs/a:b', null],
  ['dump/romfs/a\\b', null],
  ['', null],
];
for (const [input, want] of cases) assert.equal(W.classify(input), want, input);

const plan = W.planCopy([
  { path: 'd/exefs/sdk', size: 10 },
  { path: 'd/exefs/main', size: 20 },
  { path: 'd/main', size: 99 },  // duplicate destination: the first one counts
  { path: 'd/romfs/a.bin', size: 5 },
  { path: 'd/Game.nsp', size: 1000 },
]);
assert.deepEqual(plan.copies.map((c) => c.dest), ['exefs/main', 'exefs/sdk', 'romfs/a.bin']);
assert.deepEqual(plan.ignored, ['d/main', 'd/Game.nsp']);
assert.equal(plan.bytes, 35);
assert.deepEqual(plan.modules, ['main', 'sdk']);
assert.equal(plan.romfs, true);
assert.equal(plan.romfsBin, false);

const q = (s) => new URLSearchParams(s);
assert.deepEqual(W.programArgs('--exefs /opfs/x  --frames 1', q('')), ['--exefs', '/opfs/x', '--frames', '1']);
assert.deepEqual(W.programArgs('--exefs /opfs/x', q('args=--log%20debug')), ['--exefs', '/opfs/x', '--log', 'debug']);
assert.deepEqual(W.programArgs('--exefs /opfs/x', q('argv=--phase%20import')), ['--phase', 'import']);
assert.equal(W.formatBytes(0), '0 B');
assert.equal(W.formatBytes(3 * 2 ** 30), '3.0 GiB');

// Save backups (P5.3): OPFS layout -> neutral layout, ZIP round trip.
const enc = (t) => new TextEncoder().encode(t);
const txt = (d) => new TextDecoder().decode(d);
const acc = '010003f003a34000/account/11112222333344445555666677778888';
const opfs = [
  { path: `${acc}/.lgp-journal`, data: enc('1\n') },
  { path: `${acc}/.lgp-0/savedata.bin`, data: enc('old') },
  { path: `${acc}/.lgp-1/savedata.bin`, data: enc('committed') },
  { path: `${acc}/.lgp-1/sub/x.bin`, data: enc('x') },
  { path: `${acc}/.lgp-work/savedata.bin`, data: enc('uncommitted') },
  { path: `${acc}/dropped.bin`, data: enc('loose') },                 // not taken over yet
  { path: '010003f003a34000/device/.lgp-journal.tmp', data: enc('0') },  // crash during a switch
  { path: '010003f003a34000/device/.lgp-0/d.bin', data: enc('d') },
  { path: '010003f003a34000/temporary/t.bin', data: enc('t') },        // not journaled
];
const neutral = W.savesForExport(opfs);
assert.deepEqual(neutral.map((f) => [f.path, txt(f.data)]), [
  ['010003f003a34000/account/11112222333344445555666677778888/dropped.bin', 'loose'],
  ['010003f003a34000/account/11112222333344445555666677778888/savedata.bin', 'committed'],
  ['010003f003a34000/account/11112222333344445555666677778888/sub/x.bin', 'x'],
  ['010003f003a34000/device/d.bin', 'd'],
  ['010003f003a34000/temporary/t.bin', 't'],
]);

const zip = W.zipStore(neutral, new Date(2026, 9, 11, 14, 30, 0));
assert.equal(W.crc32(enc('123456789')), 0xCBF43926);
const back = await W.unzip(zip);
assert.deepEqual(back.map((f) => [f.path, txt(f.data)]), neutral.map((f) => [f.path, txt(f.data)]));
// Import of the export, of a raw OPFS copy and of a re-zipped folder give the same files.
assert.deepEqual(W.planSaveImport(back).files.map((f) => f.path), neutral.map((f) => f.path));
assert.deepEqual(W.planSaveImport(opfs).files.map((f) => f.path), neutral.map((f) => f.path));
const nested = W.planSaveImport(neutral.map((f) => ({ path: 'my backup/' + f.path, data: f.data })));
assert.deepEqual(nested.files.map((f) => f.path), neutral.map((f) => f.path));
const bad = W.planSaveImport([
  { path: '../evil', data: enc('') }, { path: '/abs', data: enc('') }, { path: 'a/../../b', data: enc('') },
  { path: 'c:/x', data: enc('') }, { path: 'ok/file', data: enc('y') },
]);
assert.deepEqual(bad.ignored, ['../evil', '/abs', 'a/../../b', 'c:/x']);
assert.deepEqual(bad.files.map((f) => f.path), ['file']);  // "ok/" was the only top folder

// A ZIP as OS tools write it: deflated, sizes and CRC in a data descriptor.
function deflatedZip(name, data) {
  const comp = zlib.deflateRawSync(data), nm = Buffer.from(name), crc = W.crc32(data);
  const local = Buffer.alloc(30), central = Buffer.alloc(46), end = Buffer.alloc(22);
  local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4); local.writeUInt16LE(8, 6); local.writeUInt16LE(8, 8);
  local.writeUInt16LE(nm.length, 26);  // sizes and CRC left 0: they follow in a descriptor (flag 8)
  const desc = Buffer.alloc(16);
  desc.writeUInt32LE(0x08074b50, 0); desc.writeUInt32LE(crc, 4); desc.writeUInt32LE(comp.length, 8); desc.writeUInt32LE(data.length, 12);
  central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(20, 4); central.writeUInt16LE(20, 6); central.writeUInt16LE(8, 8);
  central.writeUInt16LE(8, 10); central.writeUInt32LE(crc, 16); central.writeUInt32LE(comp.length, 20);
  central.writeUInt32LE(data.length, 24); central.writeUInt16LE(nm.length, 28); central.writeUInt32LE(0, 42);
  const body = Buffer.concat([local, nm, comp, desc]);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + nm.length, 12); end.writeUInt32LE(body.length, 16);
  return new Uint8Array(Buffer.concat([body, central, nm, end]));
}
const big = new Uint8Array(100000).map((_, i) => (i * 31) & 0xFF);
const inflated = await W.unzip(deflatedZip(`${acc}/savedata.bin`, big));
assert.equal(inflated.length, 1);
assert.deepEqual(inflated[0].data, big);
const broken = deflatedZip('x.bin', big);
broken[40] ^= 0xFF;  // inside the compressed data
await assert.rejects(W.unzip(broken));
await assert.rejects(W.unzip(enc('not a zip at all')), /not a ZIP/);
assert.equal(W.saveBackupName(new Date(2026, 9, 11, 9, 5, 7)), 'lgp-saves-20261011-090507.zip');

// prepareStorage: a persist() that waits on a permission prompt (Firefox)
// must not hold up the copy; the late answer still reaches onPersisted.
const realNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
let answer;
Object.defineProperty(globalThis, 'navigator', {
  configurable: true, value: { storage: { persist: () => new Promise((r) => { answer = r; }) } },
});
let late = null;
const pendingSpace = await W.prepareStorage(1, { persistWaitMs: 20, onPersisted: (v) => { late = v; } });
assert.equal(pendingSpace.persisted, null);
assert.equal(pendingSpace.enough, true);
answer(true);
await new Promise((r) => setTimeout(r, 0));
assert.equal(late, true);
Object.defineProperty(globalThis, 'navigator', {
  configurable: true, value: { storage: { persist: async () => true } },
});
assert.equal((await W.prepareStorage(1, { persistWaitMs: 1000 })).persisted, true);
if (realNavigator) Object.defineProperty(globalThis, 'navigator', realNavigator);
else delete globalThis.navigator;

console.log(`web.page_logic: ${cases.length} paths, plan, argv and save backups ok`);

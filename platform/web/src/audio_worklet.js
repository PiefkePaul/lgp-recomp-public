// AudioWorkletProcessor "lgp-audio" (P5.2): plays an AudioRing
// (platform/web/include/lgp/platform/web/audio_ring.hpp) straight out of the
// wasm memory. processorOptions: { buffer: the wasm memory's
// SharedArrayBuffer, header: byte offset of the ring }. Embedded into
// web_audio.cpp at build time and loaded from a Blob URL; the Node test
// (tests/web/test_audio_worklet.mjs) runs lgpAudioRender on its own.

// Fills the output channels of one render quantum from the ring; keeps
// silence (and the read position) while paused. Returns false once the ring
// is closed, which ends the processor.
function lgpAudioRender(h, pcm, out) {
  if (Atomics.load(h, 6) !== 0) return false;
  const frames = out[0].length, cap = h[2], ch = h[3];
  const paused = Atomics.load(h, 4) !== 0;
  const r = Atomics.load(h, 1) >>> 0;
  const w = Atomics.load(h, 0) >>> 0;
  const n = paused ? 0 : Math.min((w - r) >>> 0, frames);
  for (let c = 0; c < out.length; ++c) {
    const dst = out[c], src = Math.min(c, ch - 1);
    for (let i = 0; i < n; ++i) dst[i] = pcm[((r + i) & (cap - 1)) * ch + src] / 32768;
    dst.fill(0, n);
  }
  if (!paused && n < frames) Atomics.add(h, 5, frames - n);
  Atomics.store(h, 1, (r + n) | 0);
  return true;
}

if (typeof registerProcessor === 'function') {
  registerProcessor('lgp-audio', class extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const o = options.processorOptions;
      this.h = new Int32Array(o.buffer, o.header, 8);
      this.pcm = new Int16Array(o.buffer, o.header + 32, this.h[2] * this.h[3]);
    }
    process(inputs, outputs) {
      return lgpAudioRender(this.h, this.pcm, outputs[0]);
    }
  });
}

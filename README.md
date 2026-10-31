# lgp-recomp-public

**Status: coming soon, work in progress.** There is no code here yet. This repository will hold a public snapshot
of the project's current state once the remaining game bugs are fixed.

## What it is

A static recompiler and runtime for a Nintendo Switch game:

- the recompiler translates the game's ARM64 code to C++ ahead of time;
- the runtime replaces the console's system services, graphics (NVN on WebGPU), audio, input and video;
- the result runs as a native app on iPhone/iPad, macOS, Windows, Linux and Android (performance varies by device)
  and in the browser; Apple TV (playable) and Android TV/Shield are experimental.

To build the game from your own dump, use the [lgp-build-kit](https://github.com/PiefkePaul/lgp-build-kit), which
wraps this code in a step-by-step assistant.

Extras built in: widescreen, classic wild battles, EXP as in Yellow, random encounters from the grass, balls and
throws in battle, online and local play, motion controls, iCloud saves and more. The full list with an honest status per item is in the
[build kit's README](https://github.com/PiefkePaul/lgp-build-kit#extra-features-and-mods).

## What it is not

- **No game data, no keys and no decryption are included**, nor any C++ generated from game code. Bring your own
  legally obtained dump.
- No ready-made apps are handed out.

## Licence

MIT, see [LICENSE](LICENSE). Third-party code keeps its own licence, as listed in `THIRD_PARTY.md` once the code is
here. This project is not affiliated with or endorsed by Nintendo or any game publisher.

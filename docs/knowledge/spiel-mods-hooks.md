# Game mods via hook doors in recompiled code

Cross-game knowledge from P8.2 (as of 2026-10-20). How a mod changes the behavior of a statically recompiled
game, without binary patches and without game code in the repo. The LGP-specific status of the first mod is in
`docs/analysis/51-p8-wilde-kaempfe.md`.

## Why the recompiler has to play along

In the lifted C++, a `BL` is a direct C++ call (`lgp_fn_<va>(ctx)`), a `B` to a foreign entry a
direct tail call. Only `BLR`/`BR` see the dispatch table (ADR-0004). Replacing an entry in the table
is therefore not enough: most calls of a game function would go past it. A mod needs a place that
**every** call passes through. The codegen has to create it during the lift.

## Hook door (function entry)

`lgp_lift --hook ADDR|SYMBOL` or `--mods <ordner>` (all `hook` lines of the mod files) generates for the function

```cpp
void lgp_fn_8123450_orig(lgp::CpuState* ctx) { /* the lifted body as usual */ }
void lgp_fn_8123450(lgp::CpuState* ctx) { LGP_HOOKED(0x8123450ull, lgp_fn_8123450_orig); }
```

Direct calls, tail calls, recursion and the dispatch table all reach `lgp_fn_8123450`, i.e. the door.
`LGP_HOOKED` (`runtime/core/include/lgp/runtime/guest.hpp`) searches a small sorted table
(`lgp/runtime/hooks.hpp`). Without a hook it is a tail call of the original, with a hook it calls
`fn(ctx, original, user)`. The hook can

- **wrap**: do something before/after and call `original(ctx)`,
- **replace**: not call `original` and itself leave behind what the function's `RET` would have left behind
  (result in `x0`…, `v0`…; AAPCS64, callee-saved registers unchanged),
- **change arguments**: set `ctx->x[…]` and then call `original(ctx)`.

Cost: only functions with a door get the search, all others stay unchanged. An address that was not a
function entry becomes an entry, as with `--function`. setjmp/longjmp cannot be hooked.

The generated file registers its doors before `main()` with `LGP_HOOKABLE(tabelle, n)` (`rt::declare_hookable`).
`rt::set_hook()` rejects an address without a door. Otherwise a mod would silently do nothing if the game was
lifted with a different mod file.

Hooks are installed before guest code runs (like `register_functions`). A mod that is switched on and off
at runtime keeps its hook and decides inside it.

Patch sites at single instructions (a host function instead of an instruction, switchable at runtime) are the
second way, with `LGP_PATCH_ACTIVE`/`LGP_PATCH` (`lgp/runtime/patch.hpp`, PR #350). Doors are for "this
game function differently", patch sites for "this one value or branch differently".

## Mod files: addresses stay private

Which game function plays which role is game data that applies to only one game version. It is in
`<game>/mods/<id>.cfg`, i.e. in the game folder under `$LGP_PRIVATE_DIR` (copied together with the game folder into
locally built app packages). It never goes into the repo. The repo only contains the mechanism and the logic of the mod,
which addresses the roles by its own names. Format (`lgp/mods/mod_file.hpp`):

```
# wild-battles.cfg (private)
hook  encounter_touch 0x08123450   # function entry, gets a door
value species_offset  0x18         # constant the mod needs
patch wide_aspect     0x08234560 …  # instruction patch site, the rest belongs to the mod
```

Addresses are guest VAs as `lgp_lift` assigns them with its `--base` (default `0x08000000` for the first module),
not module offsets. The same files go to the lift (`lgp_lift --mods <game>/mods`) and to the start
(`--mods ID`, reads `<game>/mods`). Whoever changes a role has to lift again. `lgp_lift --mods` turns every
`patch` line into a patch site (only instructions without a branch, otherwise it aborts); at start the mod sets
its host function there (`set_patch`), `stop()` removes hooks and patches.

## Sequence in the boot program

`lgp::mods::start()` runs in `boot()` after all functions are registered (lifted code, host thunks,
NVN HLE) and before the first guest thread. `stop()` runs after the process ends. A mod is off as long as it is not named
(`--mods`, `settings.ini` `mods = …`, menu "Spiel-Mods" (game mods)). If the file is missing, a role is missing or a hook
has no door, this appears as `mods: <id>: not started, …` in the report and the game runs without the mod. An unknown
ID or a broken mod file, on the other hand, is an error.

A mod with a runtime switch (`ModInfo::live`, e.g. `wild-battles`) is installed when its mod file exists even
if it is not named, but switched off (`StartOptions::standby`, in the report
`<id>: ready, switched off`). `mods::set_active(id, on)` toggles it (the menu immediately, without a restart); the mod
reads the atomic flag at the place where it intervenes. Its doors then no longer get `--hook-log`. A
broken file of an unnamed mod is only a notice, not an error. Mods without `live` apply from the next start on.

## Data mods: replacing RomFS files

Some behavior lives in game data, not in code (in LGP for example the spawn tables, Analysis 54). A data mod
(`ModInfo::data`) has no `.cfg` and no doors, only files under `<game>/mods/<id>/romfs/<pfad>`. When
the mod is named, the boot program reads them before registering `fsp-srv` (`mods::romfs_files`), and
`hle::RomFsOverlay` lays them over the RomFS image: the file table is kept as a copy in memory, the entries of the
replaced files point past the end of the image, where the new contents follow. The size may change. New
files in folders the image has are added: an entry at the end of the file table, at the front of its hash bucket
and at the back of the folder's file list. The grown table then lies behind the contents, and the header points
there. So the game's RomFS driver finds them via the hash table like any other file. New folders do
not work: if a file's folder is missing in the image, all replacement files stay out and the game reads its own. This works like LayeredFS on the console, but without a copy of the RomFS, and applies from the
next start on. A mod with doors may bring such files too. The files are derived game data and
stay private.

## Calling guest functions from a hook

`detail::call_guest(ctx, va)` (`runtime/mods/src/mod_impl.hpp`) is a `BL` from the hook. The call goes through the
dispatch table, so a hook at `va` runs as well. The returned `RET` comes back to the C++ caller, `x30` plays
no role. The hook sets the arguments and `sp` itself (scratch below the caller's `sp`, 16-byte
aligned) and afterwards restores `sp` and x19–x30 or the whole state. Nothing with a
destructor may live across the call, because a guest fault aborts with `longjmp`. Virtual methods are called via the vtable in
guest memory (`load(load(obj) + slot)`). This is how `wild-battles` creates the event object, releases references and fades
in.

## Finding roles: call log

`--hook-log N` attaches to every door without a mod hook a log of the first N calls with `x0`–`x3` and the
return address (stderr, `hook log: 0x… call k …`), at the end `hook log: <n> calls`. This is a cheap way to check candidates from
Ghidra: lift with `--mods` on a candidate file, play and look in the log for which function
runs exactly at the event in question. The log contains guest pointers and values from the game and stays local.

Each role name may appear only once in a mod file. Several candidates for the same role therefore get
probe names (`hook probe_a 0x…`): every `hook` line becomes a door during the lift, even if no mod knows the name, and
the log attaches to every door without a mod hook.

What is behind the pointers is shown by `LGP_HOOK_DUMP=VA:AUSDRUCK:LÄNGE[:after],…` (only together with `--hook-log`, at doors without a
mod hook, on the same first N calls, before the original). The expression starts with a register (`x0`–`x30`,
`sp`, `tp` = `tpidr_el0`, the thread's TLS block), followed any number of times by `>OFF` (load the pointer at base+OFF) and `+OFF` (add). `x1>0:0x80` prints 0x80 bytes
of the object the pointer in `x1` points to, `x2>0>0x18+8:16` follows two pointers. Output as `hook dump:` lines
with 16 bytes per line, each with door and call number (lines of two guest threads can mix). With `:after` the dump reads only once the function has returned, but computes with the registers from entry. This shows a result block that the function fills via a pointer parameter. `ret` then stands for `x0` after the call (`ret>0:16:after`). Unreadable addresses are reported, not read. Describe the record in your own words;
offsets and values belong in the private mod file.

A mod's calls can be tried out before its code exists: `LGP_HOOK_ACT=TÜR[#AUSWAHL]:AKTION[:after],…`
(likewise only with `--hook-log`, at doors without a mod hook, but on every call, not only the first N). AUSWAHL is
`K` (only call K), `K-` (from K on) or `K-M`; without it every call applies. Actions, in the written order:
`call:FN(x0=W;x1=W;…)` jumps via BL through the dispatch table into FN, with the door's registers plus the named ones,
and afterwards restores all registers (a halt remains). `set:AUSDRUCK=W[/GRÖSSE]` writes W (1, 2, 4 or 8
bytes, default 4) to the address of the expression. `ret:W` returns W in `x0`; the original then does not run (not with
`:after`). W is a number (`0x…`, decimal, `-N`) or an expression as with `LGP_HOOK_DUMP` on the registers from
entry; an expression is an address, only a final `>0` reads the value there. `ret` in it is the `x0` of the
last `call` before it, after the original that one's `x0`. Each action writes a line `hook act: TÜR call K …`.
Example: `0x…#1-150:ret:0` makes a wait function report "not done" 150 times; `ret` always needs a value, `ret:ret` passes through the `x0` of the last `call`.

## Tests

- `indirect.*_hooks` (`-L indirect`): an own AArch64 program with doors on a function reached via `BL` and one reached via `BLR`.
  Without a hook, with a counting hook and with an equivalent replacement the result is the same as with
  Unicorn; a different replacement changes it.
- `runtime.mods` (`-L mods`): mod files, start, missing role/door, call log, `LGP_HOOK_DUMP` (expressions, output from real guest memory, unreadable pointers), `LGP_HOOK_ACT` (selection, call with restored registers, writing, return without the original), `wild-battles` with host replacement for creator, search, fill function, update and fade-in, runtime switch and readiness (`standby`).
- `boot.sdk_hook_log`, `boot.sdk_mods_*`: the SDK-like module pair with a door on `nnMain`.

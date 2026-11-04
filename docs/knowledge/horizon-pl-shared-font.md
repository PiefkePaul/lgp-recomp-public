# pl:u – system fonts (shared font) in a custom runtime

Games get the system fonts via `pl:u` (`nn::pl`). The fonts do not belong to the game but to the console's firmware
(system data archives). A runtime must read them from the user's firmware dump; they must not be
shipped.

## Archives and files

| Type (`nn::pl::SharedFontType`) | Data ID | File in the archive |
|---|---|---|
| 0 Standard (JP/US/EU) | `0100000000000811` | `nintendo_udsg-r_std_003.bfttf` |
| 1 Chinese simplified | `0100000000000814` | `nintendo_udsg-r_org_zh-cn_003.bfttf` |
| 2 Chinese simplified, extension | `0100000000000814` | `nintendo_udsg-r_ext_zh-cn_003.bfttf` |
| 3 Chinese traditional | `0100000000000813` | `nintendo_udjxh-db_zh-tw_003.bfttf` |
| 4 Korean | `0100000000000812` | `nintendo_udsg-r_ko_003.bfttf` |
| 5 Nintendo extension | `0100000000000810` | `nintendo_ext_003.bfttf` (alongside `nintendo_ext2_003.bfttf`) |

The archives are in the NAND (`bis/system/Contents/registered/*.nca`, in Ryujinx in its own data folder).
In this runtime they live under `<game>/systemdata/<id>/`, like the system archives of `fsp-srv`.

## BFTTF and shared memory

- **BFTTF in the archive:** 8-byte header, then TrueType. All 32-bit words (little-endian) are XORed with
  `0x06186249`; the first word decrypts to `0x18029A7F`, the second to the size.
- **Shared memory (0x1100000 bytes):** all six fonts one after another, each with an 8-byte header (`0x18029A7F ^
  0x49621806`, then the size XOR `0x49621806` as big-endian) followed by the **decrypted** TrueType.
  `GetSharedMemoryAddressOffset` points to the data behind the header. Ryujinx lays it out this way too, and games read it this way.
- Commands of `pl:u`: 0 `RequestLoad`, 1 `GetLoadState` (1 = loaded), 2 `GetFontSize`, 3
  `GetSharedMemoryAddressOffset`, 4 `GetSharedMemoryNativeHandle`, 5 `GetSharedFontInOrderOfPriority`.

## Without the firmware: stand-ins (fonts and the profanity filter)

Friends who build the game should not have to dump their console's firmware. Since 2026-10-28 a game folder without
`systemdata/` works with two stand-ins; with `systemdata/` everything stays as before.

**Font.** `cmake/LgpFreeFont.cmake` downloads M PLUS Rounded 1c Medium (SIL Open Font License 1.1, Google Fonts
repository, pinned by commit and SHA-256, 3.4 MB) at configure time and compiles it unchanged into `lgp_free_font`
(copyright and license travel in the font's name table, which the OFL accepts for bundled copies). `pl:u` serves it
when `/systemdata/0100000000000811/…std_003.bfttf` is missing: types 0–4 (standard, both Chinese, traditional
Chinese, Korean) all point at the one copy; the Nintendo extension (type 5, button icons) comes from
`0100000000000810` or stays empty, as before when only that archive was missing. With the standard font from the
firmware the stand-in is not used and no other firmware font is mixed with it. Switches: `-DLGP_ENABLE_FREE_FONT=OFF`,
offline `-DLGP_FREE_FONT_FILE=<MPLUSRounded1c-Medium.ttf>`. Test `runtime.hle_fs` (`test_pl_stand_in_font`).

Why this font: Nintendo's standard font is a rounded Japanese gothic (Fontworks UD Shin Go Rounded family). M PLUS
Rounded 1c is the closest free match we know of: rounded strokes, Latin, kana and about 4,950 kanji. It has no
Hangul and no simplified/traditional Chinese beyond the Japanese kanji. nxnative does the same in principle (one
stand-in for types 0–4, its own icon font for type 5), but takes the standard font from the build Mac (Arial
Unicode), which other build hosts lack and which may not be passed on.

Checked on the PC with LGP 1.0.0 (2026-10-28, A/B with `LGP_IGNORE_SYSTEMDATA=1`, `--clock virtual`, 10,500 frames
each, ABAB):
- English: all 104 `--frame-grid` points identical (max cell difference 0), frame dumps pixel-identical. LGP does not
  draw with the system font in these scenes (title, intro, character choice, rival, house).
- Korean: almost every Hangul syllable becomes "?" (only "한국어" in the language choice stays, which LGP draws
  from its own font). Simplified Chinese: many characters missing or "?" (14 of 44 compared frames differ, all in
  text boxes). So the game uses the system font for these languages; without the firmware they need a free
  Korean/Chinese font in types 1–4, which this stand-in does not provide yet.
- Not checked: German and the other European languages, Japanese.

**Profanity filter.** LGP mounts NgWord2 (`0100000000000823`) through the SDK's `nn::ngc::ProfanityFilter` after the
intro (analysis 17); without it fsp-srv answers TargetNotFound and the game stops. The archive's file format is not
documented in a source we may use, so instead of building an empty archive the filter itself is replaced:
`lgp_lift` gives the filter's exported methods a hook door (`lgp/recomp/sdk_hooks.hpp`, off with `--no-sdk-hooks`),
and the boot program, when the game folder has neither `systemdata/0100000000000823/` nor `…823.bin`, hooks them
(`runtime/boot/src/ngc_hle.cpp`): `Initialize` succeeds without mounting, `CheckProfanityWords` reports no hit,
`MaskProfanityWordsInText` changes nothing, `GetContentVersion` returns 0, `Finalize` does nothing. Log lines
`boot: no systemdata/0100000000000823 (NgWord2); profanity filter answered on the host (N methods)` and, on the first
call, `ngc: profanity filter without NgWord2 …`. Idea from nxnative's `hle_misc.c` (MIT; read only, no code taken).
Tests `boot.sdk_ngc_host`, `boot.sdk_ngc_firmware` (the SDK-shaped pair exports the same mangled names).

Checked on the PC with LGP 1.0.0 (same runs as above): the lift found 8 of the filter's methods (hook doors 22 → 30),
the boot log said `profanity filter answered on the host (8 methods)`, and the game ran past the point where it used
to stop with 0x7d402 (appearance, rival naming, house) up to frame 10,500.

Limits, honestly:
- A lift made before this change has no doors; the boot then warns (`only 0 of N profanity filter methods hooked`)
  and the game stops after the intro as before. Lift again.
- The names come from the SDK's API; another game may export other overloads. The boot's log line names how many
  methods it found.
- The native start (`--native`) has no hooks; it still needs NgWord2 from the firmware.
- Without a filter, names are never rejected. Offline that only affects the player's own console; names that travel
  over LDN/online are not filtered either (the other side's own filter may still check them).

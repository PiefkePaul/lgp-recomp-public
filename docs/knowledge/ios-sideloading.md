# Installing the iOS app with a sideloader

Status 2026-10-28. Device test passed once: iPad, Impactor 2.7.2, free Apple ID (details below).

## Why

Since about 2026-10-14 the Apple ID login fails in many tools that talk to Apple's private developer
services, xtool among them. With `xtool install` the build tool also needed usbipd in WSL to reach the iPhone.
PokeNX2iOS/nxnative avoid both: they hand out an `.ipa` whose executable carries an ad-hoc signature with the
entitlements, and leave signing and installing to a sideloader the user picks. `build.py` now does the same by
default (`--installer sideload`).

What stays: building for iOS without a Mac still needs WSL/Linux and the SDK from the user's own `Xcode.xip`
([`ios-ohne-mac.md`](ios-ohne-mac.md)). xtool is still used once to unpack that SDK (`xtool sdk install`); that
step needs no Apple ID.

## How it works

1. The build signs only the executable ad hoc with the build's entitlements (`<build>/lgp_game.entitlements`):
   `tools/ios/make_ipa.py`, with `rcodesign` or `ldid` on Linux and `codesign` on a Mac.
2. It packs `Payload/lgp_game.app` into `<work>/ios/lgp_game.ipa`. The game is inside, so the `.ipa` stays with the
   user (about 2.3 GB).
3. The user opens the `.ipa` in a sideloader and signs in there with their Apple ID. The sideloader reads the
   entitlements from the executable's signature, asks Apple for the matching capabilities on the App ID and signs the
   whole bundle with its own certificate and profile.
4. iOS grants an entitlement only when the code signature carries it **and** the profile allows it. The app logs both
   at start (`platform::own_signature`, `lgp/platform/code_signature.hpp`):

   ```
   code signature: increased-memory-limit signed yes, profile yes; extended-virtual-addressing signed no, profile no; available 4012 MiB
   ```

   `available` is `os_proc_available_memory()` at that moment, so it is the second, independent sign: with the
   entitlement it is clearly higher than without (no baseline measured yet).

`python3 tools/ios/make_ipa.py --show <ipa|app|executable>` prints what an `.ipa` carries before it goes to the
sideloader.

### On a Mac

`build.py build --target ios --installer sideload` builds with Xcode **without signing** (`CODE_SIGNING_ALLOWED=NO`,
no team needed), then `make_ipa.py --signer codesign` signs the executable ad hoc with the entitlements. On the Mac M1 (2026-10-28) the build took 1185 s and gave a 2210 MiB `.ipa`; `make_ipa.py --show` listed `increased-memory-limit = True` and no profile, as expected before the sideloader. Without
`--installer` the Mac keeps its usual way: Xcode signs with `--ios-team`, `build.py install` uses devicectl.
The assistant (`lgp-bauen.command`) offers the sideload `.ipa` when Xcode has no Apple team or only a free one;
with a paid team it keeps Xcode's signing. Xcode itself is still needed on the Mac (compiler and SDK), but no
Apple ID has to be signed in to it for the sideload way.

### Defaults

- `--installer auto` (default): `sideload` without a Mac, `xcode` on a Mac. `xtool` keeps the old way
  (`build.py install --target ios` runs `xtool install`).
- With `sideload` the memory entitlements default to `limit` (`increased-memory-limit` only), because most
  sideloading uses a free Apple ID, which cannot get `extended-virtual-addressing` (Mac test 2026-10-25,
  [`ios-ohne-mac.md`](ios-ohne-mac.md)). `--memory-entitlements both` asks for both, for a paid account.
- iCloud saves are off with `sideload`: a sideloader cannot provision the CloudKit container.

## Device test 2026-10-28 (iPad, Impactor 2.7.2, free Apple ID)

- `.ipa` from the Mac without a team (`--installer sideload`, 2210 MiB), installed with Impactor without errors.
- Impactor appended the team id to the bundle id: `<bundle prefix>.game.<team id>`.
- Log at start: `code signature: increased-memory-limit signed yes, profile yes; extended-virtual-addressing signed no,
  profile no; available 7945 MiB`. So Impactor kept the entitlement and Apple's profile for the free team allows it.
- The app loaded save slot 1, built pipelines and ran cleanly on screen up to the game's controller selection
  (the tester); not played further. No `--time-stats`, so no footprint numbers.
- Two other apps missing afterwards had been deleted by hand (the tester); Impactor removed nothing and asked for no
  certificate reset in this test.

## Device test 2026-10-29 (iPad, Impactor for Windows, free Apple ID, `.ipa` from WSL)

- `.ipa` built in WSL with preset `ios-cross` (`--installer sideload`, 14.3 min, 2.2 GB); `make_ipa.py --show`:
  `increased-memory-limit = True`, no `extended-virtual-addressing`, no profile (the sideloader adds it).
- Installed with Impactor for Windows over USB without errors; the app started and ran up to the game's controller
  selection (the tester). So the ios-cross build runs on a device at all.
- Log at start: `code signature: increased-memory-limit signed yes, profile yes; extended-virtual-addressing signed no,
  profile no; available 7934 MiB`. So Impactor for Windows keeps the entitlement as well.

## Warning: Impactor may reset the certificate

From Impactor's source (claration/impactor at b13d995, `crates/plume_core/src/utils/certificate.rs`): when Apple
refuses a new development certificate because the account has too many (result code 7460, common with free
accounts), Impactor asks once ("Impactor needs to reset your certificate. This breaks existing SideStore and
AltStore installs.") and on yes **revokes one of the account's existing development certificates**. Every app signed
with a revoked certificate stops launching, including apps Xcode or xtool signed with the same free Apple ID. The
source has no code that uninstalls apps from the device; whether iOS then removes such apps or only refuses to start
them is not checked by us.

So before the first Impactor install: back up the saves of every app signed with the same Apple ID (Files app,
`Documents/`), and answer the reset question with no if other sideloaded apps matter. A paid team's apps are
signed with that team's certificate and are not touched by a free team's reset (suspicion, from how the
certificates are scoped; not checked).

## Which sideloader

| Sideloader | Runs on | Keeps `increased-memory-limit`? |
|---|---|---|
| Impactor (v2.7.2 of 2026-10-24; moved from khcrysalis/Impactor to claration/impactor) | Windows, macOS, Linux | **yes**, iPad test 2026-10-28 with a free Apple ID (Mac) and 2026-10-29 (Windows, `.ipa` from WSL) |
| SideStore | on the device (needs a computer once) | reported to; not checked by us |
| AltStore | Windows, macOS | reported to from 2.2 on; not checked by us |
| Sideloadly | Windows, macOS | **drops it** (ns2ios-shell); not checked by us |

Only the Impactor row is checked on our devices; the others are reports.

With a free Apple ID: the signature lasts 7 days, at most 3 apps at a time, and the sideloader may change the bundle
id. The saves sit in the app's `Documents/` and survive a re-install over the same app, not a delete.

## Open

- A longer run after a sideload (field, battles, footprint with `--time-stats`).
- SideStore and AltStore.
- Whether a 2.3 GB `.ipa` goes through each sideloader (large `.ipa` files are a known source of failures; a slim
  `.ipa` without the game is idea 4 in the research note of 2026-10-28).

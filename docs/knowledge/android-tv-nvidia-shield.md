# Android TV and NVIDIA Shield as a target platform

Game-independent knowledge (status 2026-10-14, P7). Background: [`android-ndk-build.md`](android-ndk-build.md),
[`speicherprobe-mobil.md`](speicherprobe-mobil.md), [ADR-0008](../decisions/0008-native-apps-auf-allen-geraeten.md).
What this means for LGP (memory requirements, recommendation) is in
`26-nvidia-shield-und-wgpu-repos.md`.

Nothing here was measured on a Shield. Manufacturer specs and press reports are marked as such,
conclusions drawn from them as suspicion.

## Models

| Model | SoC | RAM | Storage | Android userspace | For an arm64 app |
|---|---|---|---|---|---|
| Shield TV 2015 (P2571) and Pro 2015 | Tegra X1 | 3 GB | 16 GB / 500 GB | 64 bit (suspicion, Dolphin runs there) | possible |
| Shield TV 2017 and Pro 2017 | Tegra X1 | 3 GB | 16 GB / 500 GB | 64 bit (suspicion, as 2015) | possible |
| Shield TV 2019 ("tube") | Tegra X1+ | 2 GB | 8 GB + microSD | **32 bit** (press reports) | **not possible** |
| Shield TV Pro 2019 | Tegra X1+ | 3 GB | 16 GB, 2× USB 3.0 | 64 bit | possible |

Sources: [Droid Life, Shield TV 2019 review](https://www.droid-life.com/2019/10/28/nvidia-shield-tv-2019-review/)
(RAM, storage, USB), [Android Central](https://www.androidcentral.com/nvidia-shield-pro-vs-standard) (games with a
3 GB requirement only run on the Pro), [PiunikaWeb](https://piunikaweb.com/2019/10/31/nvidia-shield-tv-2019-reportedly-supports-only-32-bit-apps/)
and [Mighty Gadget](https://mightygadget.co.uk/is-the-tubular-nvidia-shield-tv-2019-32-bit-or-64-bit-can-64-bit-apps-run-on-it/)
(tube: 64-bit CPU, but 32-bit Android, which is why Dolphin does not run there). Check on the device:
`adb shell getprop ro.product.cpu.abilist` must contain `arm64-v8a`.

According to NVIDIA, all models received updates up to Android TV 11 (Shield Experience 9); this has not been verified.

**Consequence for any Switch recompiler:** the tube is out. A Switch game needs a 64-bit host
(guest address space, 64-bit registers in native variables), and 2 GB is not enough anyway. Only the
3 GB models make sense, and among them the Pro 2019 (X1+, according to NVIDIA about 25 % faster than the X1).

## CPU and GPU compared to the Switch

The Tegra X1 is the same chip as in the Switch (Cortex-A57 cores, Maxwell GPU GM20B with 256 shader units).
The X1+ corresponds to the later Switch chip (Mariko). The Shield clocks both higher than the Switch: the Switch's CPU
runs at 1.02 GHz in games, the Shield's at about 2 GHz (manufacturer spec, not verified).

For recompiled code that means: same core type, about twice the clock. Whether that is enough depends on how much
slower the recompiled code including HLE and renderer is than the original. An A57 is considerably weaker than the
cores of current phones (Cortex-A76 class and newer). Suspicion: a game that is already tight on a Snapdragon 855
will run slower on the Shield.

## Vulkan

- NVIDIA has shipped Vulkan 1.0 since Android 6, Vulkan 1.1 since Android 8 (Shield Experience 7)
  ([NVIDIA, Vulkan on Android](https://developer.nvidia.com/vulkan-android)). Whether SE 9 on Android 11 also reports 1.2
  has not been checked. wgpu needs Vulkan 1.0 with a few extensions; that is enough.
- **Texture compression (suspicion, high probability, check on the device):** Maxwell supports BC1–BC7 in hardware (like
  the desktop Maxwells) and ASTC LDR (Tegra and the Switch use ASTC). Then wgpu reports `texture-compression-bc` and
  `texture-compression-astc`, and a WebGPU renderer needs neither a BCn nor an ASTC CPU decoder. That
  is the most important difference from Adreno and Mali, which cannot do BC. Evidence on the device: `vulkaninfo`
  (fields `textureCompressionBC`, `textureCompressionASTC_LDR`) or wgpu's adapter feature list.
- Adreno driver bugs (such as a dynamic index into a uniform `vec4`) do not automatically apply to NVIDIA.
  Workarounds that cost nothing can still be left on.
- wgpu 30 fixes frame time spikes on NVIDIA drivers outside Windows (waiting on the fence of
  `vkAcquireNextImage`, [wgpu#9486](https://github.com/gfx-rs/wgpu/pull/9486)). Whether the Shield driver is affected
  has not been checked. wgpu-native has not adopted wgpu 30 yet, see
  [`wgpu-native-upstream-stand.md`](wgpu-native-upstream-stand.md).

## Output resolution

On a 4K TV the Shield outputs 3840 × 2160. An SDL fullscreen surface then has that size: each
swapchain image costs 32 MiB (RGBA8), and upscaling to 4K costs GPU time. A game at 720p/1080p
should limit the surface size to 1920 × 1080 (`SurfaceHolder.setFixedSize` on the Java side or
`ANativeWindow_setBuffersGeometry`); the Shield's hardware compositor scales the rest without GPU cost
(suspicion, default behavior of Android SurfaceViews).

## Input

- **Shield Controller** (2017 Bluetooth LE, 2015 Wi-Fi Direct): appears as a normal Android gamepad
  (`InputDevice` with `SOURCE_GAMEPAD`). SDL3 reads it via `SDLControllerManager`; Xbox layout (A at the bottom).
  With a mapping by button label (`SDL_GetGamepadButtonLabel`) you press A for A.
- **Remote:** D-pad, select, Back, Home. Not a gamepad; it arrives as keyboard events
  (DPAD keycodes). Unsuitable for playing, usable for operating menus.
- **Back button:** Android TV expects Back to go up one level and, at the top level, to leave the app.
  Without `SDL_HINT_ANDROID_TRAP_BACK_BUTTON`, SDL3 passes the button on to `onBackPressed()`, i.e. to finishing the
  activity. Whether the Shield controller's Back button takes this route or arrives as `SDL_GAMEPAD_BUTTON_BACK`
  has not been checked. In the bad case, pressing Back quits the game.
- No touchscreen, no gyro on the controller. Games with motion controls need a button mapping.

## Leanback launcher (app appears on the home screen)

An app installed via adb only appears in the launcher on Android TV if the activity is registered for Leanback;
otherwise it can only be started via Settings → Apps or a "Sideload Launcher". Required according to the
Android TV guidelines:

```xml
<uses-feature android:name="android.software.leanback" android:required="false" />
<uses-feature android:name="android.hardware.touchscreen" android:required="false" />
<application android:banner="@drawable/banner" android:isGame="true" …>
  <activity …>
    <intent-filter>
      <action android:name="android.intent.action.MAIN" />
      <category android:name="android.intent.category.LAUNCHER" />
      <category android:name="android.intent.category.LEANBACK_LAUNCHER" />
    </intent-filter>
  </activity>
</application>
```

- `android:banner`: image 320 × 180 px (xhdpi). An APK build without resources (only `aapt2 link` with a manifest) needs
  an `aapt2 compile` step for the PNG file for this.
- `android:isGame="true"` sorts the app into the games row.
- An activity with both categories runs on phone and TV; a separate TV APK is not necessary.

## Video

The Tegra X1 decodes VP9 and H.264 in hardware (manufacturer spec). Which output format the
NVIDIA decoder delivers with `MediaCodec` and ByteBuffer output has not been checked; vendor formats (tiled)
are possible. Vendor-neutral is output into an `AImageReader` with `AIMAGE_FORMAT_YUV_420_888`
(the NDK has no `getOutputImage`; the route goes via the reader's surface).

## Memory

On a 3 GB device, Android TV 11 itself occupies a large part of the memory (launcher, Play services,
media services). How much an app in the foreground gets has not been measured; suspicion: 1.3–1.8 GB. The GPU shares
the main memory with the CPU. This can be measured as on the phone with `tools/memprobe/` as a
shell program via adb (procedure [`speicherprobe-mobil.md`](speicherprobe-mobil.md)); the Shield supports
network debugging (developer options), a USB cable is not needed.

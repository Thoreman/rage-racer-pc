# Rage Racer Quest VR — working notes

Status and how-to for the Meta Quest 3 standalone VR build. Last updated
2026-11-12 (APK 0.5.1-hud).

## Where things live

| What | Where |
|---|---|
| Game repo branch | `quest-flat` |
| psyz submodule branch | `external/psyz` → `quest-xr` (OpenXR backend) |
| SDL submodule | `external/psyz/external/SDL` pinned to upstream main `787394e6e` (needed for `SDL_openxr.h`; not in any 3.4.x release) |
| XR platform code | `external/psyz/psyz/src/platform/sdl3_xr.c` (+ hooks in `sdl3_gpu.c`, `sdl3_common.h`) |
| Stereo/HUD renderer path | `src/port/modern/modern_renderer.c` → `ModernPresentXrStereo()` |
| Eye camera, MSAA, reversed-Z | `src/port/modern/modern_native_gpu.c` (`ModernNativeApplyEye`, `ModernNativeGpuDrawStereo`, `ModernNativeGpuEyeMsaaReady`) |
| Android app | `android/` (Gradle, manifest, `RageRacerActivity`) |
| Android startup paths | `src/port/android_bootstrap.c` |

Nothing has been pushed; commits are local only.

## Build & deploy (Git Bash on the Windows PC)

```sh
# native library (NDK r27c, arm64)
android/build-native.sh            # or: ninja -C build/android rage-racer
cp build/android/libmain.so android/app/jniLibs/arm64-v8a/
# APK
cd android && JAVA_HOME="/c/Program Files/Eclipse Adoptium/jdk-17.0.21.9-hotspot" ./gradlew.bat assembleDebug
# install (Windows-style path + MSYS_NO_PATHCONV, or adb mangles it)
MSYS_NO_PATHCONV=1 adb install -r "C:/path/to/rage-racer-pc/android/app/build/outputs/apk/debug/app-debug.apk"
```

- Bump `versionCode`/`versionName` in `android/app/build.gradle` each build.
- Wireless adb: `adb connect <headset-ip>:5555` — drops whenever the headset
  sleeps; after a reboot, plug in once and run `adb tcpip 5555`.
- Windows desktop build still works: `cmake --build build/release --config Release`
  (ClangCL toolset; pass `-- -nodeReuse:false`).

### Release build

```sh
cd android && ./gradlew assembleRelease -PpublicIcon
# -> app/build/outputs/apk/release/app-release.apk
```

Signed from `android/keystore.properties` (git-ignored: `storeFile`,
`storePassword`, `keyAlias`, `keyPassword`); without that file the release APK
is left unsigned. `-PpublicIcon` ignores `app/local-icon/` so the bundled
vector icon is used. A release APK cannot be installed over a debug one (the
signatures differ): uninstalling first deletes the app's files, including the
disc image and saves, so back those up.

### Shaders
SPIR-V headers are rebuilt with the NDK's `glslc` + `xxd -i` (the repo's
`build_spirv.sh` wants glslangValidator/spirv-cross, not installed). XR-only
variants are compiled with defines: `-DXR_COVERAGE -DXR_SUPERSAMPLE`
(opaque eye materials), `-DXR_SUPERSAMPLE` (transparent), `-DCOVERAGE_ALPHA`
and `-DCRACK_DEBUG` (crack fill). **MSL headers were not regenerated** — run
`build_spirv.sh` on a machine with the tools before any macOS release.

## On the headset

- Disc image: `/sdcard/Android/data/org.rageracer.quest/files/disc/` (CUE +
  BIN tracks). Let the app create that folder first or it can't read it.
- Logs: `.../files/rage-racer/rage-racer.log`; `adb logcat -s SDL/APP VrApi`
  (`VrApi FPS=` lines give frame rate, stale frames, GPU%).
- Screenshots: Meta button + trigger → `/sdcard/Oculus/Screenshots/`.
- Optional `.../files/rage-port.ini`, `[xr]` section:

| key | default | meaning |
|---|---|---|
| `eye_scale` | 1.0 | fraction of runtime-recommended eye resolution (1.2 dropped to ~20 fps) |
| `refresh_rate` | 72 | Hz; 72 holds solid with ~30% GPU headroom |
| `world_scale` | 25 | game units per metre of head movement |
| `msaa` | false | MSAA on eyes (off by default: ~40% of the GPU in heavy views) |
| `msaa_samples` | 2 | 2 or 4 samples per pixel (4 costs ~10-15% more GPU) |
| `max_spot_lights` | 6 | nearest car lamps lighting the scene (~0.2 ms each) |
| `race_speed` | 100 | race speed percent, 50-150; 100 = speedometer true at `world_scale` (0.42x console on NTSC, 0.51x PAL; pause menu SPEED row, saved in race-speed) |
| `catchup` | true | logic keeps console time while frames are slow (psyz absolute VBlank pacing) |
| `hud` | true | floating HUD panel in the in-car view |
| `hud_width` / `hud_distance` / `hud_drop` | 2.2 / 1.3 / 0.0 | HUD panel metres (gauges must sit ~40° out) |
| `full_visibility` | false | draw all track cells; leaves far sections sliced by the draw distance floating in the sky, so off since 0.5.2 (the game's masks depend on position, not heading) |
| `terrain_facing` | true | keep terrain behind the game camera plane (back-face test by position only) |
| `radial_fog` | true | fog by distance, so it does not change as the head turns |
| `hud_fades` | false | draw full-screen fades/flashes on the HUD panel (turns it into a solid white/black board) |
| `stereo` | true | stereo in-car view |
| `crack_debug` | false | paint filled crack pixels magenta |

Headset tuner / optimiser apps set `debug.oculus.*` properties (refresh
rate, CPU/GPU level, texture size) that override the app, and some keep
doing so while "off". Check `adb shell getprop | grep debug.oculus` before
profiling: a forced GPU level of 0 makes the game crawl, 7 overclocks it.
Without one, a Quest 3 grants this app GPU level 5 (manifest CPU-for-GPU
trade).

## What works

- VR app launch; menus/FMV/chase cam on a floating screen.
- Touch controllers → SDL virtual gamepad (triggers throttle/brake, left stick
  steer, A/B/Y = Cross/Circle/Triangle, menu = Start).
- Grips are the gear paddles while driving (left down, right up;
  `xr.grip_shift = false` restores L1/R1). X is not sent to the game: holding
  it briefly recentres the seat. Either thumbstick click shows/hides the HUD.
- Touch rumble from the game's force feedback (pause-menu toggle, saved in
  `files/rage-racer/force-feedback`), lifted with a square-root response.
- Behind-car view stays in stereo (`xr.stereo_chase = false` for flat).
- Rear-view mirror also shown in Custom Race (retail: Grand Prix only).
- Stereo in-car racing with head tracking; track visible all round.
- Image quality: 4x MSAA, crack fill (PS1 T-junction holes), reversed-Z
  (distant decal fighting), far-texture footprint supersampling (moire on
  stands / finish line — "much better, still noticeable").
- HUD panel incl. working rear-view mirror.

## Open items / ideas

1. Black rectangle in the sky on the start straight — probably the start-line
   video screen; native renderer draws it black.
2. Residual far moire: options are 2x MSAA + higher eye_scale, or cheaper
   supersampling taps (cap at 3x3) to afford 80/90 Hz.
3. Loading screen / flat quad shimmer — compositor resampling; layer
   supersampling flag is on; could add a mipmapped quad swapchain.
4. Chase/replay cameras are flat-screen only (stereo only in CAMERA_VIEW_CAR).
5. Licence: upstream repo has no licence — do not distribute builds without
   the author's permission.
6. Commit/push strategy for psyz + SDL submodules (currently local-only
   commits; a fresh clone can't resolve them).

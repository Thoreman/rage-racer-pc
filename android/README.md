# Rage Racer on Meta Quest

A standalone VR build of the Rage Racer PC port: you sit in the car, look
around with your head, and drive with the Touch controllers. Menus, videos and
loading screens appear on a floating screen.

Developer notes (build commands, internals, open items) are in
[QUEST-VR-NOTES.md](QUEST-VR-NOTES.md).

## What you need

- A Quest 3, Quest 3S, Quest 2 or Quest Pro with developer mode on.
  Only the Quest 3 has been tested; see *Performance* below.
- Your own Rage Racer disc image: a CUE file with its BIN tracks, or a CHD.
  No game data is included in the app.
- A PC with `adb` to install the app and copy the disc image.

## Install

1. Install the APK: `adb install -r app-debug.apk`
2. Copy your disc image onto the headset: the CUE and all its BIN tracks in
   one folder, or a single CHD with "Rage Racer" in its name. The headset's
   `Download` folder is a good place.
3. Start **Rage Racer (Quest)** from *Unknown Sources* in the headset
   library. With no disc set up it opens a flat setup panel with two choices:
   - **Search this headset for it** asks once for "all files access", finds
     the image wherever it is and uses it from there. Keep the files where
     they are.
   - **Choose the folder myself** opens a folder picker and copies the image
     into the game's own folder. The originals can then be deleted.
4. Press **Start Rage Racer**. The choice is remembered; the panel only
   comes back if the disc image goes missing.

Copying the image straight into
`/sdcard/Android/data/org.rageracer.quest/files/disc/` (for example with
`adb push`) still works and skips the panel.

## Controls

The same list is shown under the pause menu in the game.

| Control | Action |
|---|---|
| Right trigger | Accelerate |
| Left trigger | Brake |
| Left stick | Steer; move through menus |
| Left grip / right grip | Gear down / gear up (manual cars) |
| Y | Change view (in-car or behind the car) |
| Click either thumbstick | Show or hide the HUD |
| Hold X | Recentre your seat on where your head is now |
| Menu button (left controller) | Pause; Start in menus |
| Hold A | Look behind while racing (from your seat in the in-car view, from behind the car outside); confirm in menus |
| B | PlayStation Circle |

Notes:

- The HUD always comes back while the game is paused, during the countdown
  and at the end of a race, because the pause menu is drawn on it.
- The last row of the pause menu switches controller vibration on or off.
  The setting is remembered.
- The row above it, **SPEED**, sets how fast races run, from 50% to 150%
  (left/right, or press to step). At 100% the car passes the scenery as fast
  as the speedometer says: the console's own pace looks about twice that in
  life-size VR. Menus and movies always run at normal speed. The setting is
  remembered.
- The rear-view mirror appears in the in-car view in Grand Prix and Custom
  Race.
- **Custom Race** on the title menu offers every course, class and car
  without unlocking them. Its course list has the four courses followed by
  the same four in reverse.

## Performance

The app picks settings for the headset it is running on:

| Headset | Refresh rate | Eye resolution | Status |
|---|---|---|---|
| Quest 3 | 72 Hz | 100% | tested |
| Quest 3S | 72 Hz | 100% | untested (same processor as Quest 3) |
| Quest 2 | 72 Hz | 70% | untested estimate |
| Quest Pro | 72 Hz | 75% | untested estimate |

The game itself always runs at the right speed; these settings only decide
how smoothly the picture follows your head. A steady 72 Hz looks smoother
than an uneven higher rate. Anti-aliasing (MSAA) is off by default, which
keeps heavy scenes near 72 Hz on a stock Quest 3; `msaa = true` smooths
edges at a large GPU cost (best with a performance tuner or a lower
`eye_scale`), and `msaa_samples = 4` smooths them further.

If the picture stutters or edges wobble as you turn your head, lower the
settings. Create `rage-port.ini` in
`/sdcard/Android/data/org.rageracer.quest/files/` containing, for example:

```ini
[xr]
refresh_rate = 72
eye_scale = 0.8
```

The same file can adjust the picture in the headset: `saturation`, `contrast`
and `gamma` each default to `1.0`; for a richer image try
`saturation = 1.25` and `contrast = 1.08`.

Anything written there overrides the automatic profile. `profile = quest2`
(or `quest3`, `quest3s`, `questpro`) forces a whole profile. The full list
of settings is in the developer notes.

## Troubleshooting

- **The setup panel says no disc image was found**: a CUE needs every BIN
  file it lists in the same folder, and the disc must be Rage Racer (Europe,
  USA or Japan). The log is at
  `/sdcard/Android/data/org.rageracer.quest/files/rage-racer/rage-racer.log`.
- **No vibration**: check the last row of the pause menu reads `VIBRATE ON`.
- **Seat is off-centre or facing the wrong way**: sit how you want to drive
  and hold X.

## How this port was made

This VR port is a **Claude Code project**. Claude Code is Anthropic's AI coding
agent, and it wrote the VR code: the OpenXR runtime, the stereo renderer, the
controller mapping, the disc setup panel and these documents. A person set the
goals, made the design calls and tested every build in a Quest 3. Most bugs
were found that way and then fixed by Claude.

It is built on [rage-racer-pc](https://github.com/khasinski/rage-racer-pc) by
Chris Hasiński, the native PC port of the game, and his PSY-Z compatibility
layer. All the hard work of bringing the game to modern hardware is his. This
fork is published with his permission
([issue 85](https://github.com/khasinski/rage-racer-pc/issues/85)). Please
report VR-specific bugs to this fork, not upstream.

## Legal

This is an unofficial, fan-made, non-commercial project. It is not affiliated
with, endorsed by or sponsored by Bandai Namco Entertainment or Sony. Rage
Racer and all related names, artwork and game content belong to their
respective owners.

The app contains no game data. You must supply your own legally obtained disc
image; please do not ask for or share links to one. See [LICENSE.md](../LICENSE.md) for the status of the source code.

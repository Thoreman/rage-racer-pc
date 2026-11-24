# Rage Racer on Meta Quest — changelog

## 0.6.5 release candidate 1

- FPS counter option on the GRAPHICS page; replaces the time-remaining
  display when on.
- Quest 3 and 3S defaults are now the settings tuned in the headset: the
  HIGH preset uses 2X anti-aliasing and 4 car lamps with lamp lighting,
  lamp glows and shadows off, and COLOUR starts VIVID with SHARPEN HIGH.
  Settings you have already changed on the GRAPHICS page are kept.
- Fixed a crackle at the race countdown: the race music plus the engine
  sounds went over the maximum volume and were cut off harshly. The mix
  is now limited smoothly instead (`[audio] limiter = false` in
  `rage-port.ini` restores the old behaviour).

## 0.6.4 beta

- New **GRAPHICS** page, opened from a GRAPHICS button on the title menu
  (after OPTION; Options is unchanged): presets (LOW, MEDIUM, HIGH,
  ULTRA) plus FOVEATION, ANTI-ALIAS, RESOLUTION, TEXTURE DETAIL, CAR
  LAMPS, LAMP LIGHTING, LAMP GLOWS, SHADOWS, MIRROR, SHARPEN, COLOUR and
  REFRESH rows. The page scrolls (eight rows visible) and leaving it
  returns to the title screen. Resolution and refresh rate apply after a
  restart, the rest from the next race. Choices are saved. Until you
  choose, the preset matches your headset (HIGH on Quest 3 and 3S, MEDIUM
  on Quest Pro, LOW on Quest 2). `rage-port.ini` still works for any
  setting you have not changed on the page; the page's own graphics file
  only holds the rows you touched.
- Presets now also set CAR LAMPS (2/4/6/8), MIRROR and TEXTURE DETAIL.
  Quest 2 and Quest Pro defaults use 2 and 4 car lamps. New `rage-port.ini`
  keys `texture_detail` (standard/high), `mirror` (off/half/full) and
  `sharpen` (0 to 1); `max_spot_lights` follows the preset unless set.
  Quest 3 defaults are unchanged.
- Race speed retuned: 100% is now 30% faster than in 0.6.2 and 0.6.3,
  which play-testing found too slow (about 0.55x console speed with a USA
  disc). The SPEED setting starts again at 100%; the range is still
  50-150%.

## 0.6.3 beta

- Foveated rendering: the outer edges of each eye are drawn at a lower
  resolution, where the headset's lenses blur them anyway. It saves about
  14% of the GPU time in the heaviest part of the track, which lifts the
  frame rate there by around 7 fps. `foveation = off` in `rage-port.ini`
  turns it off.

## 0.6.2 beta

- Race speed is now calibrated to the size of the VR world. At console
  speed the car crossed the scenery about twice as fast as its
  speedometer showed, so races felt far too quick. 100% on the SPEED row
  now means the speedometer matches what you see (about 0.42x console
  speed with a USA disc, 0.51x with a European one). The range is 50-150%,
  and a speed saved by 0.6.0 is reset to 100%.
- Smoother frame rate on Quest 3: the app gives up two CPU levels (it
  barely uses the CPU) so the GPU can run one level faster. The heaviest
  course averages about 64 fps instead of 55-64, and lighter stretches
  hold 72.
- If performance is unexpectedly poor, check for headset tuner or
  optimiser apps: some keep forcing the CPU and GPU to their lowest
  levels even after being closed.

## 0.6.0 beta (first public build)

The first build meant for other people. Tested on a Quest 3 only.

**Playing in VR**

- Runs natively on the headset: no PC, no streaming.
- Stereo 3D with head tracking from the driver's seat and from behind the
  car; replays stay in VR too.
- Menus, videos and loading screens on a floating screen.
- Speed, lap times and the rest of the HUD on a floating panel, with a
  working rear-view mirror in Grand Prix and Custom Race.
- Custom Race on the title menu opens every course, class and car.

**Controls**

- Triggers accelerate and brake; the left stick steers.
- Grips shift gear: left down, right up.
- Hold A to look behind: from your seat in the in-car view, from behind
  the car outside.
- Y changes view; click either thumbstick to show or hide the HUD.
- Hold X to recentre your seat.
- Controller vibration for collisions, slides and road surface, with an
  on/off switch on the pause menu.
- The pause menu lists the controls.
- Race speed setting on the pause menu, 50-100% of console speed, 60% by
  default for comfort in VR. The game keeps exact console timing even when
  the headset drops frames.

**Picture**

- 72 Hz on Quest 3 at full eye resolution; Quest 3S, Quest 2 and Quest Pro
  have their own settings (the last three are untested estimates).
- Asks the headset for its higher GPU level (Quest 3/3S trade a CPU level
  for it), so no performance tuner is needed.
- Anti-aliasing is off by default to keep heavy scenes near 72 Hz; it can be
  switched on (2x or 4x) in the settings file. The hairline gaps between PS1
  polygons are filled.
- Road markings lie on the road and trackside banners no longer cut into
  the buildings behind them, as they did once seen in real 3D.
- Fog and car lights behave correctly as you look around.
- Optional colour settings (saturation, contrast, gamma).

**Setup**

- No game data is included. On first start a setup panel finds your own
  Rage Racer disc image on the headset, or lets you pick its folder.

**Credits**

- Written with Claude Code, Anthropic's AI coding agent; directed and
  play-tested by a person on a Quest 3.
- Built on Chris Hasiński's rage-racer-pc and PSY-Z, with his permission.

**Known limits**

- Scenery such as waterfalls and trackside figures is flat, as in the
  original game.
- Distant detail can shimmer.
- Quest 2, Quest 3S and Quest Pro settings have not been tried on real
  hardware.

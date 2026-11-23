# Rage Racer on Meta Quest — changelog

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

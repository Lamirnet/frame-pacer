# frame-pacer · Dynamic FPS

An adaptive FPS limiter and performance HUD for Steam games on Linux. This
fork of [endjynn/frame-pacer](https://github.com/endjynn/frame-pacer) adds
dynamic frame pacing for Vulkan, Proton, GLX and EGL games.

Set a minimum and maximum FPS for a game. The limiter lowers the cap when
frames repeatedly arrive late and raises it in small, then progressively
larger steps when frame times stabilize. The HUD shows the current cap.

<img width="2172" height="724" alt="Адаптивный лимит FPS_ нагрузка и восстановление" src="https://github.com/user-attachments/assets/38d8983f-54d1-4aab-96bf-50d5e5a3c6d3" />

## Install

Use the standard (non-Flatpak) Steam client on an x86_64 Linux system with
systemd. Install the [source-build requirements](docs/reference/environment.md),
including the 32-bit dependencies. Fully exit Steam and all games, then build
the fork:

```sh
git clone --branch dynamic-fps https://github.com/Lamirnet/frame-pacer.git
cd frame-pacer
make -j4 install PREFIX="$HOME/.local"
```

`make install` runs the project's installer and installs the 64-bit and 32-bit
libraries under `~/.local`. No `sudo` is needed for this step.

To update an existing checkout of this fork:

```sh
cd ~/frame-pacer
git switch dynamic-fps && git pull --ff-only && make -j4 install PREFIX="$HOME/.local"
```

## Quick start · Deadlock at 40–80 FPS

Create the configuration directory and open the configuration file:

```sh
mkdir -p ~/.config/frame-pacer
chmod 700 ~/.config/frame-pacer
nano ~/.config/frame-pacer/frame-pacer.conf
```

Put global settings before the first game section. This example enables the
HUD globally and the adaptive limiter specifically for Deadlock:

```ini
hud = on

[Deadlock]
executable = "deadlock.exe"
dynamic_fps = on
min_fps_limit = 40
max_fps_limit = 80
target_gpu_load = 85
step_down_fps = 5
step_up_fps = 1
```

Save the file, fully exit Steam, then run:

```sh
chmod 600 ~/.config/frame-pacer/frame-pacer.conf
ENABLE_FRAME_PACER=1 steam
```

Launch Deadlock. With this profile, the cap starts at **80 FPS**. Watch the
limit shown in the HUD to see when the controller adjusts it. Actual FPS can
fall below 40 if the game takes longer than 25 ms to produce a frame.

## How the limiter reacts

| Situation | Behavior with the example profile |
| --- | --- |
| Sustained load | About every 300 ms, once at least five samples are available, the controller estimates a sustainable cap using the 80th percentile of time spent outside the limiter's own wait. |
| Repeated late frames | At a steady cap, two late arrivals in the last three frames can trigger an immediate 5 FPS reduction. One isolated late frame does not trigger this rule. |
| Stable recovery | After two measurement windows with headroom, the cap can climb from 40 through **41, 43, 47, 55, 63, 71, 79 and 80 FPS**. Each increase stays within the estimated sustainable rate. |
| Failed increase | Two consecutive late arrivals during the first window after an increase restore the preceding cap. The controller waits at least 600 ms before trying to climb again, starting with a 1 FPS step. |

Despite its name, `target_gpu_load` is a **frame-time budget target**. A value
of `85` tells the controller to aim for 85% of the frame interval spent
outside its own wait. That time can include CPU work and driver waits.
Hardware GPU utilization is a separate measurement.

For a global adaptive cap, move the dynamic settings above the first game
section. For a fixed cap in one game, set `fps_limit = 60` in its section
and omit `dynamic_fps = on` from that section.

## Native OpenGL games

For a native GLX or EGL game, add this to that game's Steam **Launch Options**:

```text
LD_PRELOAD="$HOME/.local/lib/frame-pacer/\${LIB}/libframe_pacer_gl_shim.so" %command%
```

Apply this option only to the individual OpenGL game. Vulkan and Proton games
use the Vulkan layer when frame-pacer is enabled.

## Documentation

- [Dynamic FPS settings and defaults](docs/configuration.md#dynamic-fps-limit)
- [Steam integration](docs/steam-integration.md)
- [Frame-spike diagnostics and troubleshooting](docs/troubleshooting.md)
- [Installation paths and removal](docs/installation.md)
- [Technical details](docs/technical-details.md)
- [Contributing](CONTRIBUTING.md)

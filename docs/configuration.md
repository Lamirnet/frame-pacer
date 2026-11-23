# Configuration

Frame-pacer reads:

```text
~/.config/frame-pacer/frame-pacer.conf
```

If `XDG_CONFIG_HOME` is set, it reads
`$XDG_CONFIG_HOME/frame-pacer/frame-pacer.conf` instead.

## Complete example

```ini
global_fps_limit = 60
hud = on

[Example game]
executable = "ExampleGame.exe"
fps_limit = 45
thread_cpu_limit = 50%

[Uncapped game]
executable = "AnotherGame.exe"
fps_limit = off
```

Games without a matching section use the global 60 FPS limit. `ExampleGame.exe`
uses 45 FPS and the optional CPU thread limit. `AnotherGame.exe` remains
uncapped.

## Options at a glance

| Option | Where | Accepted values | If omitted |
| --- | --- | --- | --- |
| `global_fps_limit` | Before any game section | `1`–`999`, or `off` | No global FPS limit |
| `hud` | Before any game section | `on` or `off` | HUD on |
| `executable` | Inside a game section | Quoted executable filename, 1–511 bytes | Required |
| `fps_limit` | Inside a game section | `1`–`999`, or `off` | Optional in a section with dynamic settings |
| `thread_cpu_limit` | Inside a game section | `1%`–`100%`, or `off` | Off |

Use lowercase `on` and `off`. An `executable` must be a filename such as
`"Game.exe"`, not a path. It cannot contain `/`, `\`, or another quote. Each game section requires an executable and at least one fixed or dynamic FPS setting.

## Basic configuration

```ini
global_fps_limit = 60
hud = on
```

`global_fps_limit` applies to every game using frame-pacer unless a matching
per-game section overrides it. Use a value from `1` through `999`, or use
`off` to leave games uncapped by default:

```ini
global_fps_limit = off
hud = on
```

`global_fps_limit` is optional. Omitting it has the same uncapped behavior as
`off`. Per-game rules continue to work in either case.

`hud` is optional. Use `on` to show the overlay or `off` to hide it. Hiding the
HUD does not disable frame pacing.

## Per-game limits

```ini
global_fps_limit = 60
hud = on

[Tactical game]
executable = "TacticalGame.exe"
fps_limit = 45

[Native Linux game]
executable = "native-game"
fps_limit = 90

[Uncapped game]
executable = "uncapped-game"
fps_limit = off
```

For fixed limits, use the following rules:

- The section name, such as `[Tactical game]`, is only a label.
- `executable` is the filename of the process that renders the game, without
  its directory.
- Native Linux executable names match exactly.
- Windows `.exe` names match without regard to letter case.
- `fps_limit` accepts a number from `1` through `999`, or `off`.
- A matching section always overrides `global_fps_limit`; `fps_limit = off`
  explicitly leaves that game uncapped even when the global limit is numeric.
- Games without a matching section use `global_fps_limit`, or remain uncapped
  when it is `off`.

For Proton games, frame-pacer identifies the mapped Windows game executable
even when Wine clears or retains a launcher's command line. If a launcher hands
off to a different game executable, a rule for that selected game takes
priority. A launcher rule can still act as a collection-wide fallback through
the same-user parent process chain.

## Dynamic FPS limit

Enable globally before the first section or add these keys to a game section
with an `executable`. Game sections inherit individual dynamic settings.
A per-game `fps_limit` selects fixed/off mode unless that same section sets
`dynamic_fps = on`. With dynamic mode on, `max_fps_limit` is the starting cap.

```ini
dynamic_fps = on
min_fps_limit = 45
max_fps_limit = 120
target_gpu_load = 85
step_down_fps = 5
step_up_fps = 2
dynamic_window_ms = 300
```

The numbers above are the defaults. Dynamic pacing is off by default. Limits
and steps accept 1–999, minimum must not exceed maximum, and the target
accepts 1–100. The HUD limit shows the current cap.

The controller estimates the share of a frame budget spent outside its own
wait. This includes driver waits and other delays, so `target_gpu_load` is a
frame-budget target rather than a hardware utilization reading. Once per configured window (at least five samples) it estimates a sustainable cap from
the 80th percentile of time outside the wait. With an 80% target and a 10 ms
P80, the estimate is 80 FPS. A single slow frame cannot set the percentile.

Set `dynamic_window_ms` to any whole number of milliseconds from 1 to
4294967295, globally or in a game section. For example, use 125 or 150 for
faster updates. Windows containing more than 320 frames use a bounded sample
from the whole window to estimate P80. Short windows still require five frames.

Alternatively, set `dynamic_window_ms = auto`. Normal measurement windows are
300 ms. A steady-state reduction triggered by two late frames out of three
starts two overload-only windows. Each lasts eight frame intervals at the
current cap, clamped to 125–300 ms. The next reduction can occur after that
shorter interval. Another late-frame reduction restarts the two windows.
Recovery requires two full normal windows with headroom after this phase.
The 600 ms retry delay after a failed rise still applies.

Recovery begins after two windows with headroom. To start a climb the first
estimate needs at least 3 FPS and 5% spare capacity. The controller starts at
`step_up_fps`, doubles the step after each successful increase, and limits it
to 8 FPS (or the configured starting step if larger). Each increase is bounded
by the estimate and `max_fps_limit`. With a starting step of 1 and enough
headroom, a 40 FPS cap climbs through 41, 43, 47, 55, 63, 71, 79 and 80.

Two consecutive late arrivals during the first window after a rise restore the
preceding cap. Further rises wait at least 600 ms and restart at the base step.
At a steady cap, two late arrivals in the last three frames lower the cap by
one `step_down_fps` immediately. A single late arrival does not trigger this
rule. After a decrease, steady-state reductions wait at least one configured window. A failed
rise still uses the preceding-cap rollback. Sustained overload reduces the cap towards the new estimate by up to
`step_down_fps` per window, except when rolling back a failed rise. Gaps longer than
two seconds start a fresh window at the maximum cap.

## Optional CPU thread limit

Some games have one or more CPU-heavy threads that cause unnecessary boost,
power use, or heat. Add `thread_cpu_limit` to that game's section to limit each
game thread independently:

```ini
[CPU-heavy game]
executable = "CpuHeavyGame.exe"
fps_limit = 60
thread_cpu_limit = 50%
```

Accepted values are `1%` through `100%`, or `off`. Start with `off`; enable a
limit only after normal FPS pacing works.

The percentage is relative to one logical CPU core. A value of `50%` allows
each game thread to use roughly half of one core over time. It is not a limit
on the whole game, total CPU use, power, or temperature. Several threads can
each use their own allowance.

When the limit is active, the HUD adds a `THR` row. The second value shows the
confirmed configured limit. `N/A` means frame-pacer has not confirmed that the
limit is active.

## Live changes

Frame-pacer checks the file about once per second. Valid changes take effect
without restarting the game or Steam.

The file must:

- Be a regular file, not a symbolic link.
- Be owned by your user.
- Have no group or other permissions; `chmod 600` is recommended.
- Be no larger than 1 MiB.
- Contain only valid, complete settings.

If the file is missing, insecure, or invalid, frame-pacer applies no FPS limit
and disables the CPU thread limit. Both `global_fps_limit` and per-game
`fps_limit` accept `off`.

The 1 MiB limit supports thousands of per-game rules while keeping memory use
bounded if the file is accidentally replaced with something else.

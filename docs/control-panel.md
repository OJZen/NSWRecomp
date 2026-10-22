# macOS control panel and input

Open the panel with the window's title-bar button or **⌘,**. Closing the panel
leaves the runtime active; closing the main window closes both. Use the return
button or click the main window to restore input focus.

The sound toggle applies immediately and is restored at the next launch; it
initially defaults to enabled. Muting preserves mixing and the audio clock.
An explicit `DM_AUDIO=0` launch does not create an output device, disables the
toggle and leaves the saved preference unchanged.

Target-rate choices are **30, 60, 90 and 120 FPS**, with a saved default of 30.
Actual presentation depends on workload and the display. Video retains its own
timing path. `--fps` or `TOTK_TARGET_FPS` overrides only the current launch.
Selecting a rate is not evidence of achieving it or of correct timing at that rate.

Compilation defaults to at most eight jobs. `--jobs 1` selects serial compilation;
`--jobs N` or `TOTK_BUILD_JOBS=N` accepts 1–32, subject to the resource guard.
See [status](status.md) before changing the validated resource budget.

## Metrics

| Metric | Definition |
| --- | --- |
| Presented FPS | Intervals between actual Metal drawable presentation timestamps |
| Submitted FPS | Wall-clock intervals between guest frame submissions |
| Frame interval P95 | Nearest-rank 95th percentile of up to 120 recent presentation intervals |
| Process CPU usage | Process user + system CPU time divided by wall time; 100% is one core |
| GPU busy estimate | Union of completed graphics/upload GPU intervals over the recent two-second window |
| Process CPU per frame | CPU time across all threads divided by submitted frames; not frame latency |
| Submission CPU per frame | Graphics translation/submission thread CPU time, excluding waits and presentation |
| GPU per frame | Duration sum of completed commands associated with the frame; overlaps may be counted |
| Render resolution | Backend-identified render dimensions, or unknown when unavailable |
| Output resolution | Current CAMetalLayer drawable dimensions |
| Physical memory | Process `phys_footprint`, distinct from RSS |
| Draw calls per frame | Increment of the backend's cumulative draw counter per frame |

Frame-based metrics use up to 120 samples; GPU/presentation feedback can lag by
up to eight submitted frames. The panel refreshes once per second and shows an
unknown value when data is absent or submission stops. The GPU interval history
is bounded; insufficient history must not produce a misleadingly low estimate.

GPU timings come from completed-command callbacks. They do not represent whole
machine utilization or shader-unit occupancy, and the displayed metrics cannot
simply be subtracted to derive wait time. See [Apple GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime?language=objc).

## Input mapping

Keyboard input is confined to the native window. WASD and IJKL map to the two
sticks; arrow keys map to directional navigation, X/Z/R/F to A/B/X/Y,
Enter to + and Tab to −. Focus loss releases active inputs.

The first extended controller recognized by macOS GameController is used, with
hot-plug support. Pair Bluetooth or connect a supported USB controller through
macOS first.

| Controller input | Guest input |
| --- | --- |
| A / B / X / Y | Mapped by letter, not by Switch button position |
| Left / right stick | Corresponding sticks, with a 15% radial dead zone and normalized remaining range |
| L3 / R3 | Corresponding stick presses |
| LB / RB, LT / RT | L / R, ZL / ZR; trigger pressed thresholds produce digital inputs |
| Menu / View | + / − |
| D-pad | Corresponding directional inputs |

Keyboard and controller buttons are combined. Keyboard input takes precedence
for a stick it actively drives. Disconnecting clears controller state; losing
application focus clears guest input. Refocusing restores current controller
state. The panel can display raw controller state without sending it to the guest.

This uses [GameController](https://developer.apple.com/documentation/gamecontroller/gccontroller),
not global event injection. Rumble, motion sensors, custom bindings and multiplayer
are not implemented here. Synthetic snapshots, dead-zone tests, focus checks and
audio callbacks do not establish physical controller or audible-output acceptance.

# This fork

Doug's fork of Zion Brock's Orb OS (`upstream` = Ziplock78/orb-os). It adds screens
and leaves upstream's own code as close to untouched as it can, so upstream's frequent
releases keep merging cleanly. Upstream's CLAUDE.md is Zion's; its launch plan,
publishing steps and Orb Studio rules are about his product, not this fork.

## What it adds

| app | files | talks to |
|---|---|---|
| Forecast | `forecast_view.*`, `weather_client_native.cpp` | Open-Meteo (the device's existing weather fetch) |
| Music | `music_view.*`, `music_client.*` | orb-ponderer: `spotify.*` |
| Photos | `photo_view.*`, `photos_decode.*`, `photos_tjpgd.*` | orb-ponderer `photo.*`, or `/photos` on the SD card |
| Facts | `facts_view.*`, `facts_client.*`, `facts_layout.*` | orb-ponderer: `fact` |
| Plasma | `plasma_view.*`, `plasma_engine.*`, `plasma_render.*` | nothing; a port of Doug's plasma project |

They sit between Flight Tracker and News. `orb_extras.cpp` registers all of them for
both the device and the simulator; each has an `APP_*_ENABLED` switch in `config.h`.

**orb-ponderer** (github.com/DougGreenhough/orb-ponderer) is the relay on
spiritdemon.net that does what this board can't: HTTPS, OAuth, image resizing.
`ponderer.{h,cpp}` is its client and a
small scheduler: each app registers a `ponderer::Module` whose `netStep` runs on the
network task (a thread in the simulator) and whose `uiApply` runs on the LVGL loop.

The board can't do TLS, so the client encrypts every request and answer itself
(XChaCha20-Poly1305, per-device keys, replay protection; "Transport" in orb-ponderer's
`docs/DEVICE-API.md`) using Monocypher, vendored unmodified in `lib/monocypher`.

The relay's key goes in `src/ponderer_secrets.h` (gitignored):

```c
#define PONDERER_KEY "op2_..."   // from the orb-ponderer setup page
```

## The knob

Every turn goes to the current app (`input_router` → `app_shell::turnCurrent`); the app
switcher is the **rock** gesture, not a turn. `setCaptured` no longer changes routing.
So in this fork: Plasma turn = Power, Music turn = volume, Photos turn = next/previous,
Facts turn = next fact; a press opens each app's own options where it has any.

## Simulator

```bash
pio run -e native && .pio/build/native/program
ORB_PONDERER_URL=http://127.0.0.1:8790/ ORB_PONDERER_KEY=op2_... .pio/build/native/program
```

- `--appshot <App> <prefix>` opens one app and follows `SIM_KEYS`: `p` press, `>`/`<`
  turn, `w` wait 250 ms, `n` one network tick, `c` capture.
- `--forecastshot <prefix>` renders the Forecast screen under made-up weather.
- `python3 tools/orb-to-sim.py <file.orb>` loads an Orb Studio theme into the simulator.

## Known issues

- None of these screens is themeable from Orb Studio (its source isn't public), so they
  don't have the plate/glass/theme-font controls `docs/adding-a-screen.md` lists.
- PNGdec in the **simulator** can corrupt some PNGs: its bundled zlib's unaligned
  4-byte copy path is live on 64-bit hosts. It also overruns its line buffer for PNGs
  wider than `PNG_MAX_BUFFERED_PIXELS / 6` (1,360 px RGB). Photos refuses those.
- Nothing here has run on a real Orb yet. Plasma's frame rate on the S3 is an estimate
  (15-22 fps); it logs `[plasma] fps` every 5 s so it can be measured.

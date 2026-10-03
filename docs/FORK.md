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
| Globe | `globe_view.*`, `globe_render.*`, `globe_texture.*` (baked by `tools/bake_globe_texture.py`) | orb-ponderer: `globe.cities` (the homepage's city list) |

They sit between Flight Tracker and News.

**Screen cycle** (`orb_cycle.*`): steps through chosen apps every N seconds and pauses on
any knob input until the Orb has been idle for M seconds. Chosen per Orb on the
orb-ponderer setup page (fetched every 5 min, cached across reboots); defaults
`ORB_CYCLE_*` in `config.h`; in the simulator `ORB_CYCLE="secs=8;resume=20;apps=Clock,Facts"`. `orb_extras.cpp` registers all of them for
both the device and the simulator; each has an `APP_*_ENABLED` switch in `config.h`.

**Themes over WiFi** (`orb_themes.*`): the relay's setup page says which uploaded themes
this Orb holds and which to wear; the Orb checks every ten minutes, fetches what differs,
removes what it installed that is no longer listed, and restarts into the chosen one.
Upstream's `theme_pull` (Zion's account server) is untouched, and themes this module did
not install are never removed. `tools/pack-orb.py <folder>` makes a `.orb` from a theme
folder; theme PNGs must be 8-bit RGBA or the firmware draws them black. Simulator:
`ORB_THEMES_FIRST_MS=300` brings the first check forward.

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
Facts turn = next fact, Globe turn = spin; a press opens each app's own options where it has any.

## Simulator

```bash
pio run -e native && .pio/build/native/program
ORB_PONDERER_URL=http://127.0.0.1:8790/ ORB_PONDERER_KEY=op2_... .pio/build/native/program
```

- `--appshot <App> <prefix>` opens one app and follows `SIM_KEYS`: `p` press, `>`/`<`
  turn, `w` wait 250 ms, `n` one network tick, `c` capture.
- `--forecastshot <prefix>` renders the Forecast screen under made-up weather.
- `ORB_CLOCK_FACE=aviator|imperial|digital|office` shows a built-in clock face. Without an
  Orb Studio theme the default (Zion's baked custom design) is bare hands on black; the web
  demo sets `aviator`. (`imperial` draws nothing without its theme either.)
- `python3 tools/orb-to-sim.py <file.orb>` loads an Orb Studio theme into the simulator.

## Browser build

`tools/build_web.sh [outdir]` (default `build/web/`; needs `brew install emscripten`) compiles
the simulator with Emscripten into `orb.js` + `orb.wasm` (+ `orb.data` when this checkout has a
`sim/sdcard` to preload), reading the source list and `-D` flags from `[env:native]` so the two
cannot drift. It is the orb-ponderer demo page's Orb (`demo/sim/`). What differs from the
desktop is `#ifdef __EMSCRIPTEN__` in `sim_main.cpp`, `net_fetch.cpp`, `native_http.cpp` and
`ponderer.cpp`:

- just the 466x466 screen in the page's `<canvas id="canvas">`; no bezel, no capture modes, no
  pointer input (the Orb has no touch screen);
- frames run on the page's `requestAnimationFrame`; the network runs as a second loop that
  suspends in `fetch()` with Asyncify (the device's network task, cooperatively);
- `ponderer::get()` asks the page's own `api.php?fn=<fn>&<extra>` for the plain payload: no key,
  no transport encryption (the demo is behind the site's login instead);
- third-party fetches go straight from the browser, so they need CORS: Open-Meteo works, the
  News gateway does not (News stays on "Getting the headlines...").

The page drives it with `Module._orb_knob(delta, pressed)`, `_orb_rock()`, `_orb_app_count()`,
`_orb_app_name(i)`, `_orb_select_app(i)`; `Module.onOrbReady()` is called once the app list
exists. `Module.orbEnv = { ORBLAT: "51.5", ... }` sets what the desktop reads from the
environment; `Module.orbFps = n` swaps requestAnimationFrame for an n-per-second timer.

## Known issues

- A theme may rename upstream's screens (Clock -> "Time"). The screen cycle stores screens
  by name, so after such a theme arrives the cycle's ticks need setting again.
- None of these screens is themeable from Orb Studio (its source isn't public), so they
  don't have the plate/glass/theme-font controls `docs/adding-a-screen.md` lists.
- PNGdec's bundled zlib has a bug in its 64-bit fast-copy path that corrupted about half of
  all PNGs in the **simulator** (never the device, which is 32-bit). The simulator build
  now switches that path off with `-DARDUINO_ARCH_RP2040` (see platformio.ini). PNGdec
  also can't take PNGs wider than `PNG_MAX_BUFFERED_PIXELS / 6` (1,360 px RGB) on either
  target; Photos refuses those rather than overrun.
- Nothing here has run on a real Orb yet. Plasma's frame rate on the S3 is an estimate
  (15-22 fps); it logs `[plasma] fps` every 5 s so it can be measured.

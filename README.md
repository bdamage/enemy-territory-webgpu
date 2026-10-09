# Enemy Territory · WebGPU

Wolfenstein: Enemy Territory (id Software's GPL release of ET 2.60b) ported to
the browser: the original C engine and game code compiled to WebAssembly with
Emscripten, rendering through a new WebGPU backend.

The game code is unchanged apart from portability fixes. You get the real
menus, maps, game logic, HUD and the software sound mixer, all running locally
in a browser tab against a loopback listen server.

## Quick start

Requirements: the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 6.0.12, which needs Python ≥ 3.10) and a browser with WebGPU
(Chrome/Edge 113+, Safari 26+, Firefox with WebGPU enabled).

```sh
source ~/emsdk/emsdk_env.sh     # EMSDK_PYTHON=/path/to/python3.12 if needed
tools/fetch_data.sh             # downloads pak0-2.pk3 (~228 MB) into data/etmain
make -j8                        # builds dist/et.js + dist/et.wasm
make serve                      # http://localhost:8080
```

Pass engine arguments with `?args=`, e.g.
`http://localhost:8080/?args=%2Bdevmap%20oasis` starts a local game on Oasis.

On first launch ET asks you to create a profile. Settings and profiles live
in IndexedDB, so they persist between visits. The paks are kept in the
browser's Cache Storage after the first download.

Controls are the stock ET binds. Click the game to capture the mouse. `Esc`
releases it and opens the menu, `` ` `` opens the console, and `Alt+Enter`
toggles fullscreen (with keyboard lock, so `Esc` reaches the game).

## How it works

```
src/            id Software's GPL source (engine, game, cgame, ui, botlib)
web/            the browser platform layer and the WebGPU renderer backend
tools/          build helpers, data fetcher, dev server
```

**Game modules.** On desktop, `qagame`, `cgame` and `ui` are shared libraries
loaded with `dlopen`. Here they are linked into the same wasm binary.
They share sources (`q_shared.c`, `bg_*.c`), so each module is compiled twice.
The first pass emits bitcode, and `tools/gen_prefix.sh` collects its global
symbols. The second pass recompiles with a generated header that renames them
(`cgame__CG_Init`, …). `web/web_modules.c` hands out the entry points through
variadic adapters, because WebAssembly traps on call-signature mismatches and
the engine calls `vmMain` through a variadic pointer.

**Renderer.** The ET renderer (`src/renderer`) is left as-is and talks to the
GPU only through `qgl*` calls. `web/qgl_wgpu.h` routes them to `web/wgl_*.c`,
a fixed-function GL 1.x emulation on WebGPU:

- Draws (`glDrawElements`, `glBegin/glEnd`) are converted to one interleaved
  vertex format and appended to per-frame vertex/index/uniform streams, which
  are uploaded with one `writeBuffer` each just before submit.
- Fixed-function state maps onto cached render pipelines (blend, depth,
  stencil, cull, color mask, polygon offset, topology), a 256-byte uniform
  slot (matrices, texenv, alpha test, GL fog, clip plane), per-texture bind
  groups, and viewport/scissor/stencil-reference state on the pass.
- Render passes open lazily, so full-screen `glClear`s become load ops. Partial
  (scissored) clears draw a quad.
- The scene renders to an offscreen target that is blitted to the canvas
  through a 256-entry gamma ramp. This replaces the hardware gamma ramp that
  `r_gamma` and overbright lighting depend on.

**Platform.** `web/sys_web.c` provides the `Sys_*` layer: MEMFS for the paks,
IDBFS for `fs_homepath`, one `Com_Frame` per `requestAnimationFrame`, and errors
surfaced to the page. `web_input.c` handles keyboard (mapped by physical key)
and pointer-locked mouse. `web_snd.c` drains the engine's DMA ring buffer from
Web Audio. `web_net.c` supports loopback only.

## Status

Working: the menus, profiles, local listen-server games on the stock maps,
weapons, effects, HUD, limbo panel and portrait views, sound, settings
persistence and `vid_restart` on window resize.

Not yet:

- **Online play.** Browsers can't send UDP, so joining real servers needs a
  WebSocket/WebRTC relay behind `Sys_SendPacket`/`Sys_GetPacket`.
- **Screenshots** (`glReadPixels`), because WebGPU readback is asynchronous.
- **Loading screen updates.** The browser only presents frames between
  `Com_Frame` calls, so the screen holds still while a map loads (a second or
  two).
- **Bots.** ET 2.60 shipped without usable multiplayer bots (Omni-bot was a
  separate add-on), so local games are just you.

## Licensing

The engine and game source is GPLv3 with id Software's additional terms (see
`COPYING.txt`). The game data (`pak*.pk3`) is **not** GPL. It is freeware under
the original ET EULA, is downloaded separately by `tools/fetch_data.sh`, and is
never committed (`data/` is gitignored).

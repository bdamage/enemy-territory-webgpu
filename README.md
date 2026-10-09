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
tools/fetch_data.sh             # downloads pak0-2 + mp_bin.pk3 (~230 MB) into data/etmain
make -j8                        # builds dist/et.js + dist/et.wasm
make serve                      # game + UDP relay on http://localhost:8080 (needs Node 18+)
```

Pass engine arguments with `?args=`, e.g.
`http://localhost:8080/?args=%2Bdevmap%20oasis` starts a local game on Oasis.

On first launch ET asks you to create a profile. Settings and profiles live
in IndexedDB, so they persist between visits. The paks are kept in the
browser's Cache Storage after the first download.

Controls are the stock ET binds. Click the game to capture the mouse. `Esc`
releases it and opens the menu, `` ` `` opens the console, and `Alt+Enter`
toggles fullscreen (with keyboard lock, so `Esc` reaches the game).

## Online play

Browsers can't send UDP, so `tools/server.mjs` (no dependencies) runs a
WebSocket relay next to the static files. Each browser tab gets its own UDP
socket on the relay machine, which makes it an ordinary ET client to game
servers. Through the relay you can:

- **Browse servers.** *Play Online* queries ET: Legacy's master
  (`etmaster.etlegacy.com`), which also lists vanilla 2.60b servers. id's
  master is long gone.
- **Join servers** running the standard game (`etmain`) or server-side-only
  mods, from the browser or with `?args=%2Bconnect%20host:port`. Missing maps
  are downloaded from the server's HTTP download host (proxied by the relay's
  `/dl` endpoint because those hosts don't send CORS headers), or in-band
  when that fails.
- **Host from the browser.** `?args=%2Bset%20net_port%2027960%20%2Bmap%20oasis`
  binds UDP port 27960 on the relay machine, so other browsers or regular ET
  2.60b clients can connect to `<relay host>:27960`.

**Mods:** most public servers run mods (Legacy, Nitmod, Jaymod, Silent, …)
that ship their own client code as native `.so`/`.dll` files. A browser
can't load those, so the client stops with an explanation after downloading
the mod. Some ET: Legacy servers also turn away any client but ET: Legacy.

**Deploying the relay.** Run it on a machine that can send and receive UDP:

```sh
node tools/server.mjs --host 0.0.0.0 --port 8080
```

| Flag | Default | Effect |
|---|---|---|
| `--ports A-B` | `27000-28000` | UDP ports browsers may contact or bind. Replies to whoever contacted the socket are always allowed |
| `--no-private` | off | refuse private/loopback destinations (use for public deployments) |
| `--no-static` | off | relay and `/dl` only, with the game served elsewhere |

Each connection is limited to 400 datagrams/s, and `/dl` only proxies `.pk3`
files. Serve the page over HTTPS behind a reverse proxy that forwards WebSocket
upgrades on `/relay`, and the client switches to `wss://` automatically. Point
a page at a different relay with `?relay=wss://host/relay`, or turn networking
off with `?relay=off`.

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
Web Audio. `web_net.c` tunnels UDP through the relay; hostnames resolve on the
relay, with the engine seeing placeholder addresses in 240.0.0.0/8.
`web_dl.c` replaces the curl downloader with `fetch()`.

## Status

Working: the menus, profiles, local listen-server games on the stock maps,
weapons, effects, HUD, limbo panel and portrait views, sound, settings
persistence, `vid_restart` on window resize, the server browser, joining
`etmain` servers, map downloads, and hosting from a tab.

Not yet:

- **Mod servers.** See above. Compiling the open-source ET: Legacy mod's
  client code into the build would open up the largest group of servers.
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

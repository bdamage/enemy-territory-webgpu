# Enemy Territory · WebGPU

Wolfenstein: Enemy Territory in the browser, in two builds that share one
WebGPU renderer backend and one browser platform layer:

| Build | Source | URL | Joins |
|---|---|---|---|
| **ET: Legacy 2.86.1** | `etl/` ([etlegacy/etlegacy](https://github.com/etlegacy/etlegacy) v2.86.1) | `/legacy/` | servers running the **legacy** mod, v2.84 – 2.86 |
| ET 2.60b ("classic") | `src/` (id Software's GPL release) | `/` | servers running plain `etmain` |

Both builds compile the original C engine and game code to WebAssembly with
Emscripten. Rendering goes through a fixed-function GL emulation on WebGPU,
and UDP networking runs through a small WebSocket relay. The game code is
unchanged apart from portability fixes, so you get the real menus, maps, HUD,
game logic and sound mixer.

## Quick start

Requirements: the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 6.0.12, which needs Python ≥ 3.10), Node 18+, and a browser with
WebGPU (Chrome/Edge 113+, Safari 26+, Firefox with WebGPU enabled).

```sh
source ~/emsdk/emsdk_env.sh     # EMSDK_PYTHON=/path/to/python3.12 if needed
tools/fetch_data.sh             # game data (~265 MB) into data/
make -j8                        # both builds (make legacy / make classic for one)
make serve                      # http://localhost:8080/legacy/ and http://localhost:8080/
```

Pass engine arguments with `?args=`. For example,
`/legacy/?args=%2Bconnect%20host:port` joins a server, and
`?args=%2Bdevmap%20oasis` starts a local game.

On first launch you create a profile. Settings, profiles, downloaded maps and
ET: Legacy's etkey (your player identity on servers) live in IndexedDB, so
they persist between visits. The base paks stay in the browser's Cache Storage
after the first download.

Controls are the stock binds. Click the game to capture the mouse. `Esc`
releases it and opens the menu, `` ` `` opens the console, and `Alt+Enter`
toggles fullscreen (with keyboard lock, so `Esc` reaches the game).

## Online play

Browsers can't send UDP, so `tools/server.mjs` (no dependencies) runs a
WebSocket relay next to the static files. Each browser tab gets its own UDP
socket on the relay machine, which makes it an ordinary client to game
servers. Through the relay you can:

- **Browse servers.** *Play Online* queries ET: Legacy's master
  (`etmaster.etlegacy.com`).
- **Join servers.** Missing maps come from the server's HTTP download host,
  proxied by the relay's `/dl` endpoint because those hosts don't send CORS
  headers, or in-band from the server when no HTTP host is set.
- **Get the right Legacy mod pak quickly.** Servers often run an older
  release than 2.86.1, and pure servers then require their exact
  `legacy_vX.pk3` (~34 MB). Downloaded in-band that takes over ten minutes,
  so the relay fetches the official release from etlegacy.com once,
  extracts the pk3 and caches it in `data/etlmod-cache/`. The client
  downloads it over HTTP in seconds.
- **Host from the browser.** `?args=%2Bset%20net_port%2027960%20%2Bmap%20oasis`
  binds UDP port 27960 on the relay machine, so other browsers or desktop
  clients can connect to `<relay host>:27960`.

**What can be joined.** Mods ship their client code (`cgame`/`ui`) as native
`.so`/`.dll` files, which a browser can't load. Each build can only run the
client code compiled into it:

- **ET: Legacy build:** servers running the `legacy` mod. Its 2.86.1 client
  code works with v2.84 – 2.86 servers. v2.83 servers changed the network
  protocol, so the game loads but misbehaves. About 85 of the ~150 Legacy
  servers on the master list are v2.84+.
- **Classic build:** servers running plain `etmain` (a handful) or
  server-side-only mods.
- **Other mods** (Nitmod, Jaymod, Silent, NoQuarter, …) stop with an
  explanation after the mod pak downloads.

Servers can still turn a client away for their own reasons: ping limits,
passwords, full slots, or (some Legacy servers) anything but the desktop
ET: Legacy client.

**Deploying the relay.** Run it on a machine that can send and receive UDP:

```sh
node tools/server.mjs --host 0.0.0.0 --port 8080
```

| Flag | Default | Effect |
|---|---|---|
| `--ports A-B` | `1024-65535` | UDP ports browsers may contact or bind. Replies to whoever contacted the socket are always allowed |
| `--no-private` | off | refuse private/loopback destinations (use for public deployments) |
| `--no-static` | off | relay and `/dl` only, with the game served elsewhere |

Each connection is limited to 400 datagrams/s, and `/dl` only proxies `.pk3`
files. Serve the page over HTTPS behind a reverse proxy that forwards WebSocket
upgrades on `/relay`, and the client switches to `wss://` automatically. Point
a page at a different relay with `?relay=wss://host/relay`, or turn networking
off with `?relay=off`.

## How it works

```
etl/            ET: Legacy v2.86.1 (src/, plus web/ glue and its Makefile)
src/            id Software's ET 2.60b GPL source
web/            shared: WebGPU GL emulation, browser JS helpers, page, relay client
tools/          build helpers, data fetcher, relay + dev server
```

**Game modules.** On desktop, `qagame`, `cgame` and `ui` are shared libraries
loaded with `dlopen`. Here they are linked into the same wasm binary. They
share sources (`q_shared.c`, `bg_*.c`), so each module is compiled twice. The
first pass emits bitcode, and `tools/gen_prefix.sh` collects its global
symbols. The second pass recompiles with a generated header that renames them
(`cgame__CG_Init`, …). The platform layer hands out the entry points through
fixed-signature adapters, because WebAssembly traps on call-signature
mismatches.

For pure servers, the ET: Legacy build reads the mod pk3's Linux x86_64
`cgame`/`ui` binaries once (without running them). That references the pak
exactly as a native client does, so the server's checksum test passes.

**Renderer.** Both renderers are left as-is: ET 2.60b's (`src/renderer`, via
`qgl*` calls routed by `web/qgl_wgpu.h`) and ET: Legacy's vanilla renderer
(`etl/src/renderer`, via a generated GLEW stand-in, `etl/web/GL/glew.h`). Both
talk to `web/wgl_*.c`, a fixed-function GL 1.x emulation on WebGPU:

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
- ET: Legacy's optional GL extras (FBOs, GLSL gamma and multisampling) report
  "unsupported", and its renderer falls back to the paths above.

**Platform.** Each build has a small `Sys_*` layer (`web/sys_web.c`,
`etl/web/etl_sys.c`): MEMFS for the paks, IDBFS for the home directory, one
`Com_Frame` per `requestAnimationFrame`, and errors surfaced to the page.
Input is mapped by physical key, with the mouse in pointer lock. Sound drains
the engine's DMA ring buffer from Web Audio. Networking goes through the relay,
with hostnames resolved there behind placeholder addresses in 240.0.0.0/8.
Downloads use `fetch()`. The JavaScript halves are shared in `web/web_js.c`.

## Status

Working in both builds: menus, profiles, local listen-server games, weapons,
effects, HUD, limbo panel, sound, settings persistence, `vid_restart` on window
resize, the server browser, joining servers, map downloads, and hosting from a
tab. The ET: Legacy build adds the legacy mod's HUD and features, TrueType fonts
(FreeType) and its translations.

Not yet:

- **More Legacy versions.** Compiling v2.83's client code alongside 2.86.1
  and picking by the server's mod pak would cover the remaining Legacy
  servers.
- **Screenshots** (`glReadPixels`), because WebGPU readback is asynchronous.
- **Loading screen updates.** The browser only presents frames between
  `Com_Frame` calls, so the screen holds still while a map loads (a second or
  two).
- **Bots.** Omni-bot is a separate native library.

Fixes made to upstream code along the way, which apply outside the browser
too:

- ET 2.60b: the `ReviveEntity` prototype, `CL_ParseDownload` desyncing on a
  stray download block.
- ET: Legacy: the `trap_FS_Write` prototype in cgame builds, and the client
  dropping when it joins a server whose 32-bit `serverTime` has wrapped
  (uptime over about 25 days).

## Licensing

The engine and game source is GPLv3 with id Software's additional terms (see
`COPYING.txt` and `etl/COPYING.txt`). The ET 2.60b game data (`pak*.pk3`) is
**not** GPL. It is freeware under the original ET EULA, is downloaded
separately by `tools/fetch_data.sh`, and is never committed (`data/` is
gitignored).

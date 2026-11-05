# Settings and frame times

Every setting here changes while the game runs: on Windows in the panel in the game (**Insert**)
and in the setup window's **NR controls**, on Linux with `nr-panel`, `nr-ctl` or `nr-toggle`
([DEVS.md](DEVS.md#the-three-tools)). The two tables below are generated from
`src/layer/nr_knobs.py`, the table the tools read, so they cannot drift from the programs.

## Frame times

<!-- rates:begin -->

Measured on this project's Arc 140V under Linux, through the socket on 2026-10-18 by `python3 src/bench/live_rates.py`, on network A — the whole round trip a game waits for, median of nine frames, not graph time alone:

| swapchain | render scale | ms | fps |
| --- | ---: | ---: | ---: |
| 512x288 | 0.35 | 25 | 40.7 |
| 512x288 | 0.50 | 25 | 39.8 |
| 640x360 | 0.35 | 26 | 38.8 |
| 640x360 | 0.50 | 26 | 38.8 |
| 854x480 | 0.50 | 31 | 32.5 |
| 1024x768 | 0.55 | 50 | 20.0 |
| 1920x1080 | 0.55 | 111 | 9.0 |

Medians of six runs of `live_rates.py --warmup 3`, with 1.3 GiB in zram and 9.7 GiB available; a row's runs spread up to 6 %, and the tool's default warm-up of 25 frames reads the same. On 2026-09-23, with 5.5 GiB in zram and the kernel's memory-pressure figures rising, 1920x1080 ran anywhere from 322 to 463 ms: if that row is much slower for you, look at swap before anything else.

That is the daemon's own cost with nothing else on the GPU. A game adds its own frame to it: **Tekken 7** ran at **30 fps at 800x450** with the render scale at 0.35, and 27 at 0.6, in a live session on 2026-09-27 — against 10.5 fps at 640x360 on 2026-09-16 (`notes/phase59`).

<!-- rates:end -->

**Set the game small.** The cost follows the extent, and a stack of full-frame passes runs
at the *output* resolution regardless of the render scale, so the swapchain size matters
as much as the scale does. A game at 512x288 with the compositor stretching to the panel
is the fastest arrangement there is.

**On a small window, a low render scale costs no more than the frame it fills.** The network's
frame is never smaller than 320 a side, so a scale whose frame would be smaller is raised,
free, to fill the cheapest frame it lands on — 640x360 runs as 0.5 for anything up to 0.5, and
800x450 as 0.4. The daemon's log line says `runs as` when it happens.

### By GPU

<!-- players:begin -->

The network's own time a frame, in ms, from the setup window's speed test: the latest report for each GPU and network, from players and from this project's Arc 140V (`notes/players-20261103.md`). Since 0.0.2 the Windows release keeps a game's frames on the GPU, and each frame waits for this time plus a few ms of NR's passes around the network before the game draws its next. The test's round trip, which the setup window also shows, still goes through the CPU and reads higher. The fixed part is what a frame costs whatever its size, fitted over fields up to 0.45 megapixels, the sizes games run at.

| GPU | network | 640x360, 0.4 | 1280x720, 0.4 | 1920x1080, 0.4 | 1920x1080, 1.0 | fixed + per megapixel, ms | report |
| --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| Arc B580 | A | 8 | 9 | 18 | 88 | 4 + 39 | [#17](https://github.com/Uzbekunknown/dlss-nr-on-intel/issues/17), 0.0.1.3, 2026-10-18 |
| Arc A770 | A, inferred | 32 | 35 | 55 | 229 | 22 + 91 | [#23](https://github.com/Uzbekunknown/dlss-nr-on-intel/issues/23), 0.0.2 or 0.0.3, 2026-10-31 |
| Arc A770 | B | 16 | 19 | 29 | 120 | 11 + 51 | [#25](https://github.com/Uzbekunknown/dlss-nr-on-intel/issues/25), 0.0.3.1, 2026-11-01 |
| Arc 140V | A | 24-27 | 32-34 | 60-64 | 381-404 | 11 + 144 | this project's laptop, Windows, 2026-10-18 |
| Arc 140V | B | 12 | 16 | 31 | 187-202 | 4 + 76 | this project's laptop, Windows, 2026-10-18 |

<!-- players:end -->

## The network: A or B

`nvngx_dlssnr.dll` 310.9.16 and later carries two networks. **A** is the one NR has run from the
start; **B** is NVIDIA's smaller one, about twice as fast here and a little lighter in what it
changes. On Windows setup takes B out of such a DLL and runs it, and the panel in the game
switches between the two as you play; on Linux `NR_PRESET=B` picks B
([DEVS.md](DEVS.md#build)). The frame times at the top of this page are network A's, and the
table by GPU names each row's network.

## The settings

Every one of these is post-network or an extent choice: none invalidates the weights, and
all of them move between frames. Only `profile` costs a forward pass.

<!-- knobs:begin -->

### `render_scale` — the fraction of each side the network runs on

`0.05` to `1`, step `0.05`, default `1`

The only knob that changes the frame rate. The network draws its detail on a frame this much smaller; the detail is then scaled up and laid over the game's full-resolution frame, so the game's own pixels are never resampled. Lower is faster and draws coarser detail, at about the same strength. The network's frame is never smaller than 320 pixels a side (`min_extent`), so on a small window a low scale is raised, at no cost, to fill the cheapest frame it lands on: 640x360 runs as 0.5 for anything up to 0.5, 800x450 as 0.4 for anything up to 0.4, and a lower scale is never the slower one. For play, 0.35-0.6 is the useful range. For screenshots, 1.0 hands the network the game's raw pixels, jagged edges and all, and it turns a little of them into pixel-level grain; 0.9 smooths them first and draws the same strength of effect. Cost on an Arc 140V: about 130 ms per megapixel of network frame, plus 10-20 ms that grows with the game's own resolution.

### `min_extent` — the smallest side the network's frame is padded to

`128` to `320`, step `64`, default `320`

The network's frame is padded, by mirroring the picture, to at least this many pixels on a side. 320 is what NVIDIA's own driver does; the network itself runs down to 128. At small live sizes most of a 320 frame is padding, so a lower floor is much faster — on an Arc 140V, 512x288 at scale 0.35 takes 25 ms a frame at 320 and 14 at 128 — and draws a somewhat different picture, since the network no longer sees a mirrored copy of the scene around it. Neither is wrong; compare them in a game. It changes nothing once the scaled frame is larger than this anyway.

### `profile` — which way to trade skin texture against highlights, and its colour grade

`standard` / `natural` / `cinematic` / `hogwarts`

The style the network is asked for, and the colour grade that comes with it. The profiles are a trade, not a quality ladder: what one adds to skin and surface texture it takes from highlights. `standard` is the default and adds the most texture; `natural` and `cinematic` keep more of the highlights, and on very bright scenes `cinematic` can smooth fine detail rather than add it. After the network, as NVIDIA grades them, `natural` comes out a little darker, flatter and less saturated and `cinematic` less saturated. `hogwarts` is Hogwarts Legacy's own tuning, as NVIDIA and the game's developers set it for DLSS 5: `standard`'s style with half its tone change and 0.84 of its detail, and faces and skin milder still, at 0.27, which the network finds by itself. The profile is an input to the network, so it takes effect on the next frame the network draws; the knobs below act after it.

### `intensity` — how far to go towards the model's picture, or past it

`0` to `2`, step `0.05`, default `1`

Blends the model's picture with the game's own. 1 is the model's picture, 0 is the game's frame untouched, and values between are part of the way. Above 1 the blend goes past the model and exaggerates what it changed, which can look overdone. Free to change: nothing is re-run.

### `detail_strength` — how strongly to apply the fine detail the pass adds

`0` to `2`, step `0.05`, default `1`

The change the pass makes is split into fine detail — pores, hair, grain — and broad tone. This scales the fine part: 0 keeps only the tonal change, above 1 sharpens further, and past about 2 it over-sharpens. Away from 1 it costs one blur over the whole frame.

### `colour_strength` — how strongly to apply the pass's change of tone and colour

`0` to `2`, step `0.05`, default `1`

The broad half of the same split: how much of the pass's change in tone and colour is applied. It does not add colour back. The pass tends to calm bright, saturated areas, and this scales that change — so above 1 colours can look more washed out, not richer, and 0 keeps the game's own tone with the detail on top.

### `temporal` — how much of the previous frame to carry over

`0` to `1`, step `0.05`, default `1`

Each frame, the previous result is fed back to the network, which decides per pixel how much of it to keep; that is what keeps the picture from shimmering. This scales how much it keeps. 0 draws every frame on its own and forgets the stored frame, so turning it back on cannot bring back an old one. Costs a few per cent of the frame time.

### `hold` — how firmly to keep areas the game did not change

`0` to `1`, step `0.05`, default `1`

Where the game hands back exactly the same pixel as last frame, the previous result is still right for it, so it is kept at least this firmly. This is what stops still areas — backgrounds, a waiting character — from shimmering while something else moves. It cannot leave a trail: the first frame in which the game changes a pixel lets go of it.

### `release` — change, in levels of 255, that drops the previous frame where something moved

`0` to `64`, step `1`, default `24`

The other side of `hold`. Where the game's own pixel changed by this many levels of 255 or more, something moved there, and the previous result is dropped for that pixel; smaller changes keep a share that falls with the change. It is what prevents trails behind moving objects, since the layer has no motion vectors to follow them with. 16-24 is the useful range: lower drops more and the picture starts to shimmer over moving things, 0 turns it off and trails come back.

### `cut_limit` — how big a change between frames counts as a new scene

`0` to `1`, step `0.01`, default `0.15`

The average change between two frames above which the scene is taken to have cut — a camera cut, a menu, a replay — and the previous frame is dropped entirely instead of pixel by pixel. Lower cuts more readily; 1 never cuts.

<!-- knobs:end -->

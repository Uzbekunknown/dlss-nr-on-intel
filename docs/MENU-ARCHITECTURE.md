# The menu layer, as an architecture

**Scope:** the parts of `src/layer/` that concern the in-game settings menu
**Reader:** whoever is changing this layer, and whoever has to judge whether a change is safe
**Relation to its predecessor:** `docs/ARCHITECTURE.md` is about the **network itself** — graph,
weights, numerics — and does not touch this layer. `docs/WINDOWS-PORT.md` is the port's history in
time order. This is the **first time this subsystem has been described as an architecture**.

---

## 1. Why this layer exists, and why it is shaped the way it is

### 1.1 The constraints came before the design

A Vulkan layer **lives inside somebody else's process**. It has no window of its own, no message
loop, and no `WndProc` to subclass. That one constraint produced almost every structure below:

| Wanted | How a normal program does it | How a layer must do it |
|---|---|---|
| Know whether it is in front | `WM_SETFOCUS` / `WM_KILLFOCUS` | compare `GetForegroundWindow()` against this process's windows (`nr_focus.c`) |
| Read keys | window messages | poll `GetAsyncKeyState` every frame (`nr_menu.c:273`) |
| Know where its mouse is | `WM_MOUSEMOVE` | the system cursor position, converted by hand (`nr_menu_ui.cpp`) |
| Draw UI | create a window | draw onto the game's swapchain image with a compute pass (`nr_menu.h:1-9`) |
| Use ImGui | give ImGui a `GLFWwindow` | wire up ImGui's Vulkan backend oneself, with no platform layer |

**The reference project, OptiScaler, solved the first two by subclassing**
(`menu/input/input_system_messages.cpp`). This layer declines to subclass, because patching the
game's window procedure is likelier to break the game than reading system state is.

### 1.2 Three structural facts that follow

1. **Polling per frame, not events.** All input is "ask what the current state is". This is why
   every key needs its own edge latch (`m->key_down[]`, `menu_ui_*_was_down`) — with no event
   there is no such thing as "was pressed", so each key manufactures one.

2. **"Is it in front" must be gated explicitly, and separately at every read site.** With no
   single input-event entry point there is no single place to filter. **That is the structural
   cause of review defect 2:** `nr_layer.c` gained the gate, `nr_menu.c` did not, and neither knew
   about the other.

3. **A system-wide read, once ungated, affects the whole desktop** and not just the game. An `L`
   typed in a chat window changes the game's settings; Home in an editor turns the effect off in
   the game behind it.

---

## 2. Layers and responsibilities

```
┌─────────────────────────────────────────────────────────────┐
│ nr_layer.c  (3667 lines)                                     │
│   The Vulkan layer itself: hooks vkQueuePresentKHR, grabs    │
│   the frame, sends it to the daemon, composites, and decides │
│   when the menu should be drawn.                             │
│   89 call sites into the menu — the largest consumer, and    │
│   the tightest coupling                                      │
└───────────────┬─────────────────────────────────────────────┘
                 │ direct C calls (no dispatch layer)
     ┌───────────┴───────────┬──────────────────┬───────────────┐
     ▼                       ▼                  ▼               ▼
┌────────────┐    ┌──────────────────┐  ┌──────────┐  ┌─────────────┐
│ nr_menu_ui │    │ nr_menu_bridge   │  │ nr_menu  │  │ nr_raw_mouse│
│  (C++)     │    │ (C, 1708 lines)  │  │ (C,1724) │  │ (C, 180)    │
│ ImGui panel│    │ knob table +     │  │ hand-    │  │ raw input   │
│ 1454 lines │    │ file I/O +       │  │ drawn    │  │             │
│            │    │ pacing           │  │ menu +   │  │             │
│            │    │                  │  │ keys     │  │             │
└─────┬──────┘    └────────┬─────────┘  └────┬─────┘  └─────────────┘
      │                    │                 │
      │              ┌─────┴──────┐    ┌─────┴──────┐
      │              ▼            ▼    ▼            ▼
      │      ┌────────────┐  ┌──────────┐  ┌────────────┐
      │      │nr_menu_    │  │ nr_file  │  │  nr_focus  │
      │      │strings.c   │  │ (atomic  │  │ (32 lines) │
      │      │(generated) │  │  write)  │  └────────────┘
      │      └────────────┘  └──────────┘
      ▼
┌────────────────┐
│ third_party/   │
│ imgui 1.93.0   │  (vendored, byte-identical to upstream)
└────────────────┘
```

### 2.1 Two UIs, one set of data

The **hand-drawn path** (`nr_menu.c`) and the **ImGui path** (`nr_menu_ui.cpp`) coexist. This is
not legacy — it is a **deliberate fallback path**:

- `nr_menu_create` asks the physical device whether this swapchain format can be a storage image,
  and returns NULL when it cannot (`nr_menu.h:28-31`)
- `nrex_create` is the ImGui one, and it too returns NULL on failure
- `nr_layer.c` treats both as "NULL means no menu"

`nr_menu_set_key_reading(menu, 0)` (`nr_layer.c:2223`) turns the hand-drawn path's key reading off
once ImGui is up, so the two paths do not each act on the same keypress. The very existence of that
mechanism is the cost of running two paths, and it is the other cause of defect 2: key reading is
spread across `nr_layer.c`, `nr_menu.c` and `nr_menu_ui.cpp`, and each place must remember to gate
on its own.

### 2.2 `nr_menu_bridge` is the only authority on the data

It owns:

- the knob table (11 knobs, mirroring `nr_knobs.py`)
- reading and writing `work/nr_settings.json` (the only writer)
- the **live preview** `nr_settings.json.live` (with a PID, from which the daemon tells whether its
  author is still alive)
- the three pacing values (kept in `nr-env.txt`, see 2.3)
- persistence of the language and the effect switch

**"Save is the only thing that writes the settings file"** is this layer's central rule. Every
other path writes only the preview. That rule was once broken by Reset calling `save_settings` —
one keypress destroyed every saved value — and after the fix it is written into both the comments
and the tests.

### 2.3 Two settings, two files, two readers

| | file | reader | when it takes effect |
|---|---|---|---|
| image-quality knobs | `work/nr_settings.json` | daemon (reads every frame) | next frame |
| frame pacing / GPU path | `dlss-nr\nr-env.txt` beside the game | **the layer** (reads once at start-up) | next launch |

The split is not arbitrary. The layer reads the environment at `vkCreateInstance` and never again;
the daemon is a resident process that watches the file every frame. So "next launch" is a physical
fact for the former, not a design choice.

**The ownership rule (settled in round three):**

- `nr-env.txt` belongs to the **panel** — it is the only writer that can see what the layer is
  actually running
- the panel writes `windows-profile.json` too, and the profile is a **projection**
- the window reads the profile, so the two agree

The breaking and restoring of that rule is what review defect 6 was.

---

## 3. The ABI boundary: the only C++/C seam

`nr_menu_ui.h` is the **only** interface that crosses C++/C. It must obey two things.

### 3.1 `extern "C"`

`nr_menu_ui.h` has it (`#ifdef __cplusplus extern "C" {`). **`nr_menu_strings.h` once did not** —
it is generated, and the generator forgot. The consequence: a C++ caller looked for the mangled
name `?nr_menu_text@@YAPEBDHH@Z`, and the failure surfaced only when a C++ test first called it.

**The lesson:** a generated header and a hand-written one must be treated as equally bound by the
ABI contract.

### 3.2 Calling convention (only fatal on x86)

```c
// nr_menu_ui.cpp:122 — as it was
typedef PFN_vkVoidFunction (*nrex_gpa_fn)(void *device, const char *name);
```

On x64 every calling convention is the same and this code is correct. On x86
`vkGetDeviceProcAddr` is `__stdcall` while this declares it `__cdecl`: **every resolved function
pointer leaves the stack unbalanced by 8 bytes**. Accumulated, that is a `c0000005` jump to a wild
address.

The correct spelling is `VKAPI_PTR` (Vulkan's calling-convention macro, `__stdcall` on 32-bit).

**Why this only showed on 32-bit, and why the 32-bit build does exist:**
`tools/build_win.bat:257` compiles `nr_menu_ui.cpp` with `%VCX86%` (the x86 vcvars), and
`build_win.bat:261` links it into `nr_layer32.dll` with `/MACHINE:X86`. So the 32-bit DLL
**ships with the full menu**, and this ABI error goes out with it.

### 3.3 Handle width (x86's second trap)

Vulkan's non-dispatchable handles (`VkImage`, `VkImageView`, `VkBuffer`, …):

- `VK_USE_64_BIT_PTR_DEFINES` == 1 → real pointers (x64)
- == 0 → `uint64_t` (32-bit)

The comment at `nr_menu_ui.cpp:260-270` already knew this ("pointers only when... 32-bit games")
and used `VK_NULL_HANDLE` correctly. But `nrex_image` / `nrex_image_view`'s **return values** were
still `void *`, which truncates a 64-bit handle on 32-bit.

That is why the crash survived the author's first suspicion being fixed — two independent defects.

### 3.4 What is worth keeping at this boundary

- `VK_NO_PROTOTYPES` plus runtime resolution: the layer **cannot** link its own Vulkan library, and
  must ride on the one the game already loaded. That is what decides `nrex_vk`'s existence.
- `nrex_gpa_fn` passing through `ctx[0]`/`ctx[1]` (`void *`): safe on x64, and safe on x86 where a
  function pointer is also 32 bits. **This part is fine.**

---

## 4. The input subsystem's three paths

This is where defects concentrated, so it is worth drawing on its own.

```
                   ┌──────────────────────────────┐
                   │  system-wide state (no events) │
                   │  GetForegroundWindow          │
                   │  GetAsyncKeyState             │
                   │  GetCursorPos                 │
                   └───────────┬──────────────────┘
                               │
         ┌─────────────────────┼──────────────────────┐
         ▼                     ▼                      ▼
┌───────────────┐   ┌──────────────────┐   ┌──────────────────┐
│ nr_layer.c    │   │ nr_menu.c        │   │ nr_menu_ui.cpp   │
│ menu_read_keys│   │ nr_menu_update   │   │ menu_ui_keys     │
│ (L2831+)      │   │ (L891+)          │   │ (L1260+)         │
│               │   │                  │   │                  │
│ INS  ✅gated  │   │ INS  ✅read_keys │   │ all keys ⚠️ none │
│ L    ✅gated  │   │ Home ❌ no gate  │   │ but its caller   │
│ Home ✅gated  │   │ S/R  ❌ open only│   │ checks foreground│
│ S/R  ✅gated  │   │ arrows ✅read_keys│ │                  │
└───────────────┘   └──────────────────┘   └──────────────────┘
         │                     │                      │
         └─────────────────────┴──────────────────────┘
                               │
                     ┌─────────┴─────────┐
                     │ result: three     │
                     │ behaviours        │
                     │ front open/closed/│
                     │ background        │
                     └───────────────────┘
```

### 4.1 Where the gate has to go

The right place for the gate is the **read site**, not the consumer, because:

- the consumer (`nr_menu_inject_*`) does not know where the key came from
- the read site is the only place that knows "this is system-wide state"

**But `nr_menu.c` cannot ask which window is the game's.** It has no `HWND`, only an
`nr_menu*`. So the correct answer is:

**give `nr_menu` a way to ask about focus** (a function pointer, or an hwnd), injected by
`nr_layer.c` at `nr_menu_create`. Then `nr_menu.c` can gate at the read site without having to
guess.

### 4.2 Three hard rules

1. **Home works with the panel open AND closed.** That is a product requirement, not a compromise.
   The gate looks only at "is the game in front", never at "is the panel open".
2. **S and R work only while the panel is open** — the panel being open is itself the declaration
   "I am changing settings".
3. **L needs the panel open AND the game in front** — a language change writes a file, so it is a
   settings action.

### 4.3 Keys still reach the game

**Unavoidable.** `GetAsyncKeyState` only reads; it does not take the event out of the system queue.
The game's own message loop receives the key as usual.

The available suppression, and its cost:

- `RegisterHotKey`: globally exclusive, breaks the game's own bindings, and needs a window
- a low-level keyboard hook (`WH_KEYBOARD_LL`): would work, but installing a global hook inside
  somebody else's process is the last thing such a layer should do — it affects the machine's
  entire input and the blast radius of a mistake is uncontrollable

This layer chooses **not to suppress**, and says so on the panel. That is a deliberate boundary,
not an omission.

---

## 5. Data flow: one keypress to one file write

Take "move intensity from 1 to 1.5 in the panel, then press S":

```
1. nr_layer.c menu_read_keys         GetAsyncKeyState sees the arrow
2. nr_menu_inject_step / nrex_*      handed to the UI
3. nr_layer.c:3203-3221              reads the changed row back from nrex
4. nr_menu_bridge_set(i, 1.5)        ──┐
                                       │ bridging layer
5. format_row()                        │ memory + the shown string only
6. write_live_settings()               │ writes nr_settings.json.live (with PID)
                                       ──┘
7. daemon reads .live each frame, sees the PID alive → applies 1.5 → the picture changes
                                       (nr_settings.json has not changed yet)

8. the user presses S
9. nr_menu_take_save_request
10. nr_menu_bridge_save              ──┐
                                       │ the only path that writes the settings file
11. save_settings()                    │ rewrites nr_settings.json
12. preview_confirmed = 1              │ the preview is "confirmed"
13. write_live_settings()              │ written once more, both sides agree
                                       ──┘
```

**The key invariants:**

- steps 4-6 must never touch `nr_settings.json`
- step 11 is the only write
- `preview_confirmed` is the switch for "can this preview be applied on the next start"
  (set to 0 after Reset, 1 after Save/Revert)

### 5.1 Pacing goes another way

```
nr_menu_bridge_set_pacing(what, value)
  ├── conflict resolution (the GPU path requires async off + every frame)
  ├── write_pacing()          → nr-env.txt (read by the layer at start-up)
  └── write_profile_pacing()  → work/windows-profile.json (read by the window)
```

**Note that `NR_GPU_ONLY` is always written 0**: it is a strict mode, not another name for the GPU
path. Writing 1 makes the effect refuse the CPU fallback when the shared path is unavailable, so
the effect disappears entirely — which is the shape of issue #17. That comment is at
`nr_menu_bridge.c:495-504` and must be read before changing it.

### 5.2 The three pacing values, linked

| operation | side effect | why |
|---|---|---|
| async on | GPU path off | the path requires synchronous |
| GPU path on | async off + cadence=1 | same |
| cadence>1 | GPU path off | same |

These rules live **in exactly one place** (`nr_menu_bridge.c:1518-1545`) and both UIs go through
it. That is the right shape and worth keeping.

---

## 6. Localisation: one generator, two consumers

```
dist-tools/windows-wizard.ps1  (the setup window's 4-language table, 178 keys)
             │
             │ tools/gen_menu_strings.py
             ▼
    ┌────────────────────┬────────────────────┐
    ▼                    ▼                    ▼
nr_menu_strings.c   nr_menu_strings.h   nr_menu_strings.py
(for the in-game    (declarations,      (for the Python
 menu)               incl. extern "C")   tools)
```

**Why generated rather than hand-written:** this project has already been wrong twice from "one
fact, three copies" (the `render_scale` default disagreeing in three places; `Profile.neutral`
present in one UI and already deleted in the other).

### 6.1 Keys only the menu has go in `OWN`

The setup window has no overlay, so keys like `compare` and `menu key` do not exist there. Those
are hand-written in four languages in `gen_menu_strings.py`'s `OWN` dictionary.

**The pacing keys added in round three** (`async`, `async_hint`, `gpu_image`, `gpu_image_hint`,
`every`, `every_hint`, `every_frame`, `every_nth`, `gpu_blocked_*`) are here too — they were once
hardcoded in `nr_layer.c` as `zh ? ... : ...`, which is **a two-way choice made over a four-way
enum**, so Russian and Spanish fell into the English branch.

**The lesson:** any `== NR_LANG_ZH ? ... : ...` is the shape of a defect. A four-language enum
takes a table, never a ternary.

### 6.2 Plural forms are formatting, not concatenation

`NR_STR_EVERY_NTH` uses a `{}` placeholder (like `"every {} frames"`), and `nr_layer.c`
substitutes it. A language with a different word order then needs no C change.

**But Russian needs three forms:** 1 кадр / 2-4 кадра / 5+ кадров. There was one `every_nth` key
and a 24-byte buffer that was too small ("каждые 2 кадров" is 27 bytes). That was review defect
8's two halves:

- **the buffer:** `every_names_store[4][24]` → at least 32
- **the grammar:** pick the form by the number, do not just substitute

### 6.3 What a check can and cannot cover

| check | catches | does not catch |
|---|---|---|
| key exists (the self-test's 178) | a missing translation | stale content |
| not equal to English | straight copying of English | **a translation left behind by an English revision** |
| `{}` preserved | broken formatting | whether the word order is right |

**The round-three case, for real:** the setup window's `GpuImagePathTip` Chinese read "skip the
effect if it is not satisfied, the game shows the original frame", while en/ru/es read "take the
ordinary path, the saved report says why". The Chinese described the behaviour from **before**
issue #17 was fixed. It is genuinely different from English (so the check cannot fail it), but
the content is wrong. This kind of thing is found only by reading the four languages side by side;
there is no automatic way.

**Conclusion: localisation checks can only guarantee "nothing is missing", not "it says the right
thing".** The latter needs a human, and needs re-reading after every English revision.

---

## 7. Build: one source tree, five build systems

| build | target | status |
|---|---|---|
| MSVC (`build_win.bat`) | `nr_layer.dll` / `nr_layer32.dll` + 7 fixtures | ✅ works, links the menu sources |
| MinGW (CMake on Windows) | the same + tests | ❌ 3 tests, undefined refs |
| Linux (Makefile) | `libnr_layer.so` + tests | ❌ menu sources not built |
| Linux (CMake) | the same | ❌ same as the Makefile |
| cross (-m32) | `nr_layer32` | ⚠️ links maybe, crashes certainly (defect 1) |

### 7.1 The root cause

Five tests use **`#include "nr_layer.c"`** rather than linking the library:

`test_device_teardown.c:25`, `test_exchange.c:8`, `test_gpu_only.c:22`, `test_settled.c:6`,
`test_transfer_resources.c:3`

They therefore inherit every one of `nr_layer.c`'s menu symbols, and the build systems hand them
one `.c` file. **`test_gpu_present` already uses `add_dependencies(... nr_layer ...)`, so linking
the library is the established pattern; these five simply did not follow it.**

### 7.2 What the one build that works does right

`build_win.bat:150-168` explicitly compiles the five ImGui sources + `nr_menu_ui.cpp` +
`nr_menu.c`/`bridge`/`font`/`strings`/`strings_lookup`/`file`/`focus`/`raw_mouse` into objects and
links them into `nr_layer.dll`. **That is the list the others have to copy.**

### 7.3 What the platform gating is really like (not what the comment says)

`nr_layer.c`'s comment claims the menu headers "bring their own no-op arm". The reality is
**mixed**:

| file | no-op arm? |
|---|---|
| `nr_focus.h` | ✅ `static inline` (L45-46) |
| `nr_raw_mouse.c` | ✅ `#else` stub (L174-179) |
| `nr_menu_font.c` | ✅ `#else` stub `glyph_bitmap` (L106-115) |
| `nr_menu.c` | ✅ the key functions have `#else` (L282-297) |
| **`nr_menu_ui.cpp`** | ❌ **`menu_ui_keys` entirely ungated** (L1316-1339) |
| **`nr_menu.h`** | ❌ declares unconditionally (but a declaration compiles) |

So the premise "Linux can compile it" **does not hold for `nr_menu_ui.cpp`**. That is the work
defect 5's fix had to do first: gate that file.

---

## 8. The mistakes this architecture invites

Ordered by how often they actually happened:

1. **a system-wide read with no gate** (defect 2; also the original "dragging a progress bar in
   another window moved a slider")
2. **one fact in several copies** (`render_scale`'s default, `Profile.neutral`)
3. **a two-way choice over a four-language enum** (the pacing rows before defect 8)
4. **"I changed one, so I changed all"** (`nr_layer.c` gated, `nr_menu.c` not)
5. **asserting the implementation instead of the behaviour** ("Reset does not touch the file" was
   green because Reset never ran)
6. **a build system out of step with the source** (defects 4 and 5 this time)
7. **a comment describing something that does not exist** ("bring their own no-op arm")

### 8.1 What this is for

Adding a new system-wide read? Ask:

- does it have a gate?
- where does the gate's idea of "the game's window" come from?
- what is it off Windows?

Adding a new string? Ask:

- is it `zh ? ... : ...`?
- are all four languages written?
- is the buffer big enough?

Adding a new build target? Ask:

- does it compile `nr_layer.c`? Then does it need the menu sources?
- is it `#include "nr_layer.c"` or a link against the library?

---

## 9. Open questions

| question | consequence | who decides |
|---|---|---|
| the panel's "Network" box duplicates the `weights` knob (added in `9a74998`) | two controls, one setting, two values | author + owner |
| the exact assertion for pacing's two-way write | how defect 6's test should change | implement, then confirm with the author |
| whether Linux needs the menu at all (or a permanent no-op) | the depth of defect 5 | the author (who said "stub the menu out off Windows") |
| whether the panel can say "the game still receives these keys" | defect 7's wording and placement | decided while implementing |

---

## 10. In one sentence

**This layer is a privileged observer with no window, no message loop and no events.** All its
input is a poll of system state, all its drawing borrows somebody else's swapchain, and all its
configuration is written into two files another process reads. Its robustness comes not from
clever design but from **every read site remembering to gate, every piece of data having exactly
one authority, and every platform being handled explicitly** — drop any one of the three and the
symptom shows up somewhere completely unrelated.

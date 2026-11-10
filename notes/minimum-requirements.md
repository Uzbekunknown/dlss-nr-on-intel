# The real minimum requirements (2026-11-10)

An audit of what this system actually needs, which line of code enforces it,
and what happens when it is missing. Measured on an Arc B580 (driver 101.8864),
Windows, with a complete vkcube install.

**One sentence:** the only hard gate is on the daemon side - an Intel Arc whose
driver exposes cooperative matrix and can compute float16 8x16x16 into float32 at
subgroup scope. Everything else is either spec-mandated and universal, or
degrades gracefully.

## 1. The network (daemon) - the only hard gate

| Requirement | Enforced at | When missing |
| --- | --- | --- |
| XMX matrix engines (Intel Arc) | `src/gpu/libxmx.c` `device_lacks()` | No fallback, no CPU path |
| `VK_KHR_cooperative_matrix` | same, names the extension | same |
| `VK_KHR_workgroup_memory_explicit_layout` | same | same |
| fp16 8x16x16 -> fp32, subgroup scope | `libxmx.c:687-696`, per-shape compare | an unable device is still tried (`XMX_PICK_NONE_ABLE`), then pipeline creation fails |
| Vulkan 1.3 instance | `libxmx.c:784` (libxmx makes its own) | always satisfied |
| 1.2/1.3 driver properties, subgroup control | `libxmx.c:921,941` | degrades: pin32 warns, denorm falls back |

Measured (`notes/players-20261103.md`, eleven cards):

- **B570/B580/Pro B70**: offer 8x16x16 natively - the playable tier (B580 ~50 fps at 1080p, scale 0.5)
- **All eleven A-series** (A380/A750/A770): offer only 8x8x16, yet compute NR's shape
  correctly - it runs (A770 ~40 fps at 720p, scale 0.4). README calls it experimental.
- **Intel graphics without XMX**: no.
- Non-Intel: the panel does not check the vendor, but the shape gate excludes NVIDIA/AMD.

## 2. The capture layer (in a game) - almost no gate

| Requirement | Enforced at | When missing |
| --- | --- | --- |
| Any Vulkan version the game declares | `nr_layer.c:840`, defaults to 1.0 | no requirement |
| Swapchain accepts TRANSFER_SRC/DST | `nr_layer.c:1093-1103` | degrades: unpatched swapchain, capture off, game runs |
| HOST_VISIBLE memory (staging) | `nr_layer.c:1612` | capture fails, game runs |
| Extensions / features / version / vendor | none (PR #41 removed the last) | - |

## 3. The panel - after PR #41

| Requirement | Enforced at | When missing |
| --- | --- | --- |
| Surface caps AND format support STORAGE | `nr_layer.c:1083-1085` (`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778`) | degrades: no panel, game runs |
| Format in the five compiled variants | `nr_menu.c` `menu_formats` | same (BGRA8/RGBA8 unorm+srgb, A2B10G10R10) |
| HOST_VISIBLE\|HOST_COHERENT memory | `nr_menu.c` `find_memory_type` | same (every real device has it) |
| A graphics queue family (ImGui draws) | `nr_menu_ui_vk.cpp` | same |
| Windows (font via `GetGlyphOutlineW`, `GetAsyncKeyState`) | `nr_menu_font.c` | no panel off Windows; `nr-panel` covers Linux |
| `NR_LAYER_TRIGGER` configured | `nr_panel.c:1165` | degrades: panel works, Home and the checkbox read "?" and NR stays on |

**The panel's shader requirements, measured** (`spirv-dis`): `menu.spv` is SPIR-V 1.0,
declaring only `OpCapability Shader`, zero extensions. ImGui's embedded vert/frag are
SPIR-V 1.0 too. A Vulkan 1.0 core device suffices.

**The 10-bit build** declares `StorageImageExtendedFormats`, which the registry enables
at `VK_VERSION_1_0` with no feature (`vk.xml`).

**Deliberately absent**: a vendor check (vendorID appears only on the GPU transport
path), any `VkPhysicalDevice*Features` query, any device extension requirement. The one
remaining `vkGetPhysicalDeviceFeatures2` (`nr_layer.c:943`) belongs to the GPU patch,
not the panel.

## 4. The SPIR-V version rule - the one that was misunderstood

`menu.spv` must be built at `--target-env vulkan1.0`, and that is a **requirement, not a
preference**.

Device-level functionality added by a core version may be used only where **both** the
device's `VkPhysicalDeviceProperties::apiVersion` **and** the application's
`VkApplicationInfo::apiVersion` allow it. The SPIR-V version a module may use is part of
that rule. Building the panel at `vulkan1.3` produces a SPIR-V 1.6 module, which is
invalid under Vulkan 1.0 semantics:

    VUID-VkShaderModuleCreateInfo-pCode-08737
    Invalid SPIR-V binary version 1.6 for target environment SPIR-V 1.0
    (under Vulkan 1.0 semantics).

Intel's driver ran such a module anyway, which is why the panel appeared to work in games
declaring 1.0 and 1.1. A driver accepting a module does not make the module valid. The
version test in `menu_device_reason` was therefore **correct for the module it guarded**;
what made it unnecessary was fixing the build, not the test.

Raising the target back to `vulkan1.3` would reinstate that requirement with nothing left
to catch it, because `menu_device_reason` answers NULL now.

## 5. Transport and DirectX paths - all degrade

- Same-device GPU transport needs Vulkan 1.1 instance + Intel vendor +
  `VK_KHR_external_memory_win32` + BDA; any one missing falls back to a CPU copy
  (observed in the vkcube log).
- D3D8-11 goes through DXVK (installed by setup); D3D12 through the game's own XeSS or
  OptiScaler. Compatibility requirements, not hardware ones.
- cv2/PIL are imported only for optical flow or a render scale other than 1
  (`nr_frame.py:46`) - optional.

## 6. Software and resources, measured

| Item | Value |
| --- | --- |
| Python | 3.11.9 + numpy (the only hard dependency) |
| Daemon memory | ~1.4-1.7 GB private |
| Disk | weights 216 MB (B) / 278 MB (A), plus the release |
| Drivers verified | 101.8864 (this machine), 101.9150 (player reports) |
| Elevation / anti-cheat | an elevated game does not get the layer; anti-cheat may refuse it |

## Open

- The A-series runs on the driver's emulation of the shape; no vendor contract keeps
  that stable across driver updates.
- No compiled variant for surfaces outside the five formats (R10G10B10A2, HDR16 float).
- **No fixture pins `menu.spv`'s capability set.** If someone raises the target or adds a
  1.2+ feature to the shader, nothing fails the build. A `spirv-dis` assertion that the
  module declares only `Shader` would close this.
- Linux panel path unverified; `make test` in both memory modes outstanding.
- The render-scale slider's mouse drag has not been scripted.

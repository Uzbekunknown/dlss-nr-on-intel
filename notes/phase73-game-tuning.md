# Phase 73 — Hogwarts Legacy's own tuning of NR, read out of the game

2026-11-06. Hogwarts Legacy's DLSS 5 update reached the owner's Epic copy that night. The
owner then asked for NR to offer what NVIDIA and the game's developers chose, beside our
standard.

## What the update brought

- `nvngx_dlssnr.dll` 310.9.16.0 in `Engine\Plugins\Runtime\Nvidia\StreamlineCore\Binaries\ThirdParty\Win64`:
  **byte for byte the 310.9.16 that preset B came from** (SHA-256 `2bc19bc0…789f`, the same
  `WEIGHTS_HT` and `WEIGHTS_CC_SMALL_FP8`). There is no new network. Hogwarts Legacy owners now have the
  DLL in their game.
- Streamline 2.14.16 with `sl.dlss_nr.dll`, DLSS SR, RR and FG 310.9.16, XeSS 2.0.2.53 to
  2.0.2.68, XeFG 1.3.1.78. FSR 3.1 is unchanged.
- A new game option, `NeuralRendering=(Mode=Off,ToneIntensityPercentage=50,
  DetailIntensityPercentage=84,CharacterIntensityPercentage=27)`, in `GameUserSettings.ini` and
  `SavedUserOptions.sav`.

## From the game's percentages to the network's inputs

Read with `objdump` and a cross-reference scan of the executable. It is protected: code and
data share `.data1` and `.arch`, and there is no `.text`. Addresses are those of build 2153883.

1. **The game** (`Phoenix`): `GetDefaultNeuralRenderingInfoForUpscaler` (0x14337ce40) fills
   `{Mode, 50, 84, 27}` for every upscaler. The mode is On by default only for the DLSS modes,
   and only where NVIDIA's support query passes. Four call sites (0x142e52270, 0x142e56bb5,
   0x142e5a55x, 0x142e5c86x) apply it:
   `cvtdq2ps xmm0, percent; mulss xmm0, [0x146a97ca0]` (0.01), then the plug-in's
   `SetDLSSNRToneStrength`, `SetDLSSNRStructuralStrength` and `SetDLSSNRSkinStructureStrength`,
   with `SetDLSSNRMode` before them. The game's native code calls no setter for the intensity,
   the style or the mask mode. Only the plug-in's Blueprint thunks reach those.
2. **NVIDIA's Unreal plug-in** (`StreamlineDLSSNR`, linked into the executable) registers
   `r.Streamline.DLSSNR.Intensity`, `.ToneStrength`, `.StructuralStrength` and
   `.Mask.SkinStructureStrength`. Each is a float whose default is `[0x146a0e6dc]` = **1.0**.
   `.Mask.Mode` (0 off, 1 the stencil mask, 2 the automatic mask) and `.Style` (0-2) are ints,
   default 0. The intensity's help reads "range [0..1]", and its setter clamps to that. The
   tone, structure and skin setters print the float into the cvar unchanged. The skin cvar's
   help: "applies to skin regions defined by the auto mask mode".
3. **Streamline** (`sl.dlss_nr.dll`): `dlss_nrBackendNGX.cpp` sets the NGX parameters straight
   from the options, with no arithmetic. `movss xmm2, [opts+0xf8]` goes to `DLSSNR.Intensity`,
   `+0xfc` to `.LocalToneStrength`, `+0x100` to `.LocalStructureStrength`, `+0x114` to
   `.SkinStructureStrength`; `+0x110` is the byte for `.UseAutoMask`, `+0x11c` `.MaskBlendMode`,
   `+0x108` `.Style`, `+0x122` `.UICorrection`. It also accepts only the eUltraQuality,
   eMaxQuality and eDLAA performance modes.

So the network gets **tone 0.50, structure 0.84 and skin 0.27, at intensity 1 and style 0**. Our
`standard` is tone 1 and structure 1 with the mask off, which is the plug-in's defaults. The
shipped panel phase30 compared with defaults to structure 1.5 and skin 2.0 with the mask on. The
game is milder than both: half the tone change, 0.84 of the detail, and a quarter on skin.

**Not established: the mask's mode.** The skin strength is read only with the automatic mask
on (phase72), and no code of the game's sets the mode. It would be in the game's packed config,
or in a Blueprint. A Character slider would do nothing with the mask off, so the profile turns it
on, with the automatic-mask lane following the local structure. The packed config would settle
it, or a frame from an RTX 50 with the Character slider moved.

## The profile

`nr_frame.PROFILES["hogwarts"]`: style 0, tone 0.5, structure 0.84, `AutomaticMask(0.27, -1)`.
`feature_lanes` gives channels 10-14 as MLX-DLSS's builder writes them. With the mask on:
style, tone, structure 1, skin 0.27, the rest 0.84, each in half. With it off: the three scalars
and -1, -1. The native pass (`nr_image.features`) took the five lanes already. The GPU image
path's push block grew two floats for channels 13 and 14 (`image_preprocess.comp`, 104 bytes),
so the profile runs on the GPU path as the others do. The panel, the setup window, the Linux
config and `nr_knobs` list it. The network finds the skin by itself: phase30's difference map
for the skin strength is a face. No extra pass is run.

## Tools

The scan was done with scratch scripts that were not kept, each a few lines of numpy: strings
in a region, RIP-relative `lea`, `mov` and `movss` targets, `call` and `jmp` rel32 targets, and
absolute pointers. `objdump -d -M intel --start-address --stop-address -j .data1` read the
ranges they found.

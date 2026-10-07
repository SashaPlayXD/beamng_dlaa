<img width="1280" height="719" alt="photo_2026-10-07_18-50-01" src="https://github.com/user-attachments/assets/a0067155-3f3d-4d40-8164-fad5a881a604" />
<img width="1280" height="719" alt="photo_2026-10-07_18-49-55" src="https://github.com/user-attachments/assets/9ba89b96-af92-4487-9df2-faf439da4ddd" />
[README.md](https://github.com/user-attachments/files/33164307/README.md)
# bng_dlss — NVIDIA DLAA for BeamNG.drive

> **About the authorship of this code.**
> All source code in this repository (C++ add-on, Lua mod, build script) was **written by Claude, an AI model by Anthropic**, in a long chat session.
> The repository owner directed the project, ran every experiment in-game, collected logs and measurements, and tested each version.
> The owner is not a C++/graphics programmer, so please file technical questions as **Issues** — answers may be prepared with AI help as well.

An unofficial add-on that adds **real NVIDIA DLAA** (DLSS at native resolution, used as anti-aliasing) to BeamNG.drive.
After installation, **DLAA (NVIDIA)** appears in `Options → Graphics → Anti-aliasing` next to FXAA and SMAA.

- Genuine NVIDIA NGX / DLSS SDK, no patched DLLs.
- Runs on the official ReShade 6.8.0 (with add-on support).
- Sub-pixel camera jitter, synchronized automatically (no manual calibration).
- Cost on an RTX 3070 Laptop at 2560×1440: about 2–3 ms per frame with preset K.

**This is not DLSS upscaling.** There are no Quality / Balanced / Performance modes: BeamNG cannot render the scene below the window resolution, so only native-resolution DLAA is possible with this approach. See *Limitations*.

---

## Requirements

| | |
|---|---|
| Game | BeamNG.drive **0.39.4.0.20972**, **DirectX 12** renderer |
| GPU | NVIDIA RTX (20 series or newer) |
| ReShade | **6.8.0 with full add-on support** (official build from reshade.me) |
| DLSS runtime | `nvngx_dlss.dll` from the official [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`lib/Windows_x86_64/rel/`) |

The add-on is tied to this game version. After a game update the frame layout may change and the add-on may stop working (see *Troubleshooting*).

## Installation

1. Install **ReShade 6.8.0 (add-on support)** for `BeamNG.drive\Bin64\BeamNG.drive.x64.exe`, API **DirectX 10/11/12**. Effect packages are not needed.
2. Copy into `BeamNG.drive\Bin64\`:
   - `bng_dlss.addon64` (from Releases, or build it yourself);
   - `nvngx_dlss.dll` from the NVIDIA DLSS SDK.
3. Copy the folder `mod/bng_dlss` to
   `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods\unpacked\bng_dlss`
   and make sure the mod is enabled in the game's mod manager.
4. Start the game with **DirectX 12**, open `Options → Graphics`, enable **Anti-aliasing** and choose **DLAA (NVIDIA)**.

Diagnostics panel: press **Home** (ReShade) → **Add-ons** → **BeamNG DLSS**. It shows whether NGX is available, whether depth and motion vectors were found, and the jitter synchronization status.

### Uninstall

Delete `bng_dlss.addon64` from `Bin64` **and disable/remove the `bng_dlss` mod**. If you remove only the add-on, the Lua part may keep jittering the camera, which looks like fine shaking.

## Important: NVIDIA App "DLSS Override"

If `NVIDIA App → Graphics → DLSS Override – Model presets` forces a Super Resolution preset globally, the driver replaces the model requested by the add-on.
On RTX 30 the newest presets (L/M) are **5–15× more expensive** than K: in testing, DLAA cost went from ~3 ms to ~16 ms.
Set Super Resolution to **"Use the 3D application setting"**, then choose the preset in the add-on panel (K is the default).

## How it works

Short version of what was found in the engine and how the add-on uses it:

1. **Inputs found in BeamNG's D3D12 frame**
   - Scene color: `R16G16B16A16_FLOAT` (HDR, before tonemapping).
   - Depth: `D24_UNORM_S8_UINT`, **reversed-Z** (cleared to 0).
   - Motion vectors: `R16G16_FLOAT`, in UV units, *previous minus current* — the convention DLSS expects; written for static geometry by a compute pass and for vehicles (including soft-body deformation) by the material shader.
2. **Hook point.** In the post-processing command list the HDR scene color is copied several times (ping-pong). Before the **3rd** RGBA16F→RGBA16F copy the scene is complete (including transparents) and bloom, motion blur and tonemapping have not run yet. The add-on runs DLSS there and writes the result back over the scene color.
3. **D3D12 state.** NGX changes descriptor heaps, root signatures and the pipeline state on the game's command list. The add-on hooks those four `ID3D12GraphicsCommandList` methods, snapshots the game's state before DLSS and restores it afterwards.
4. **Jitter.** BeamNG's Lua sandbox forbids FFI, so Lua and C++ cannot talk directly.
   - The Lua mod shifts the projection every frame with `GameViewportCtrl:setFrustumCameraCenterOffset` using a Halton(2,3) sequence of 8 phases, converted to pixels from the current FOV.
   - The engine writes this jitter into the motion vectors, so the add-on reads back a row of motion vectors of static ground from the GPU and fits the Lua phase automatically (typical error < 0.01 px). DLSS is created with `MVJittered`.
   - Positive vertical offsets break vehicle culling in the engine, so vertical jitter only goes upward (−1…0 px); DLSS receives the values centred to ±0.5 px.
5. **Game menu.** The Lua mod adds `DLAA (NVIDIA)` to the anti-aliasing option list, turns SMAA/FXAA off while it is selected, and exchanges two tiny files with the add-on in the user `settings` folder: `bng_dlss_request.txt` (menu → add-on) and `bng_dlss_state.txt` (add-on → Lua: "jitter wanted").

## Limitations

- **No upscaling modes.** BeamNG has no desktop render-resolution scale; implementing it would require faking the window size, replacing the swapchain, recompositing the UI and remapping mouse input.
- **Locked to 0.39.4.0.20972.** The hook point relies on the current frame layout.
- **Paused physics (J):** the engine writes zero motion for parts of the player vehicle, so the vehicle may shimmer slightly while paused. Not an issue while driving.
- The first frames after loading a map or switching DLAA on look grainy while DLSS accumulates history.
- Only tested on one machine (RTX 3070 Laptop, Windows 11, 2560×1440).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| No `DLAA (NVIDIA)` in the menu | Mod `bng_dlss` disabled, or the game is not on D3D12 |
| Image shakes finely | DLAA off but jitter on (add-on missing or failed) — check the ReShade panel |
| DLAA very slow | NVIDIA App DLSS Override forcing preset L/M (see above) |
| Artifacts after a game update | Frame layout changed; the hook point needs to be re-found |

## Building from source

Requirements: Visual Studio 2022/2026 with *Desktop development with C++*, plus:

```powershell
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git reshade68
git clone https://github.com/NVIDIA/DLSS.git
# ImGui exactly as pinned by ReShade 6.8.0:
git init imgui; cd imgui
git remote add origin https://github.com/ocornut/imgui.git
git fetch --depth 1 origin 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c
git checkout FETCH_HEAD
```

Edit the paths at the top of `build.bat` and run it. The result is `build\bng_dlss.addon64`.

## Third-party components and licenses

- **NVIDIA DLSS SDK** — `nvngx_dlss.dll` and NGX headers/libraries are NVIDIA's and are covered by NVIDIA's license; they are **not** included in this repository.
- **ReShade** (BSD 3-Clause) — headers are used at build time and not included.
- **Dear ImGui** (MIT) — header used at build time and not included.
- Code in this repository: MIT License (see `LICENSE`).

Not affiliated with BeamNG GmbH or NVIDIA. Use at your own risk; do not use in multiplayer if a server forbids client modifications.


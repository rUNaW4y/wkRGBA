# wkRGBA

**wkRGBA brings true-colour terrain to Worms Armageddon.**

Worms Armageddon normally renders colour maps through an indexed palette. A colour map can use no more than 112 terrain colours because the remaining palette entries are reserved by the game for worms, weapons, effects and interface elements. Photographs, illustrations and other detailed images must therefore be reduced to that small palette before they can be used as terrain. This produces visible banding, colour loss and dithering, particularly on gradients and high-resolution artwork.

wkRGBA removes that visual restriction. It keeps a native indexed representation for Worms Armageddon's terrain logic and collision system, while preserving and rendering the original 32-bit RGBA image through the Direct3D 9 Shader renderer. The game continues to control destruction, collision, worms, objects, effects and the interface; wkRGBA replaces the visible terrain pixels with their original colours. This allows maps containing thousands or millions of colours to be played without reducing their artwork to the native 112-colour palette.

## Features

- Loads 32-bit RGBA PNG maps from `User\SavedLevels`.
- Preserves the original RGB and alpha channels without colour quantisation or dithering.
- Uses the PNG alpha channel to define the initial terrain collision mask.
- Keeps opaque black pixels as solid terrain.
- Follows the game camera and updates destroyed terrain during play.
- Supports the Direct3D 9 Shader renderer used by Worms Armageddon 3.8.1.
- Integrates with the native map editor, borders, automatic worm placement and generated holes.
- Includes multiplayer map transport and replay metadata support.
- Leaves native indexed maps unchanged.

Pixels with alpha values of 128 or greater are treated as solid terrain. Pixels below that threshold are treated as empty space. Partially transparent solid pixels retain their original alpha for rendering.

## Requirements

- Worms Armageddon 3.8.1
- Direct3D 9 Shader selected as the game's graphics renderer
- WormKit module loading enabled
- Windows with Direct3D 9 support

For multiplayer games, every participant should install the same wkRGBA build. Mixed lobbies containing players without the module are not currently considered reliable for RGBA maps.

## Installation

Place `wkRGBA.dll` in the main Worms Armageddon directory, next to `WA.exe`. Start the game normally and select an RGBA PNG from `User\SavedLevels`.

The module creates a `wkRGBA` directory beside the game executable for its runtime files and logs.

## Current limitations

wkRGBA is usable, but it still has areas that need further development and testing:

- **Multiplayer synchronisation:** all players should use the same module build. Map changes, reconnects and mixed lobbies can still expose timing or synchronisation problems, including desynchronisation when host and client do not activate the same map state.
- **Large map transfers:** high-resolution Rope Race and Big Rope Race maps can contain tens of megabytes of compressed artwork. Their transfer and verification can take considerably longer than the native map transfer. The host's loading indicator may report a client as ready before the complete RGBA payload has been processed.
- **Editor preview consistency:** generated holes, their monochrome outlines and the final cropped area do not always appear immediately in the editor preview. Reloading or regenerating the map may produce a different preview even when the selected settings have not changed.
- **Border state:** combinations involving only the top border require more testing. The temporary clearance used for automatic worm placement must be removed whenever a real top border is present, without cropping or shifting the original artwork.
- **First-frame artefacts:** on some maps, interface elements such as the wind bar or team name can briefly leave an incorrect terrain-coloured halo during the first load.
- **Native terrain effects:** destruction is synchronised with the RGBA artwork, but every native colour transformation, such as burning or darkening terrain, may not yet have an exact RGBA equivalent.
- **Saving from the editor:** wkRGBA protects the original RGBA file from being overwritten by the native indexed editor representation. Saving an edited map under a new name currently produces the game's native representation rather than a lossless edited RGBA image.
- **Replays:** RGBA replay data is implemented, but replay creation, seeking and playback still need broader testing across long games and large maps.
- **Renderer compatibility:** the integrated renderer targets Direct3D 9 Shader. Other Worms Armageddon renderers are not supported by this build.

These limitations are useful starting points for contributors. The most valuable improvements are a deterministic map-state snapshot shared by the editor, host and clients; transfer progress tied to complete RGBA verification; consistent preview generation; and expanded automated coverage of border and hole combinations.

## Building

The project requires Windows, CMake 3.21 or later, and Visual Studio with the **Desktop development with C++** workload and a Windows SDK.

Worms Armageddon is a 32-bit application, so wkRGBA must be built for Win32/x86. The supplied script configures the correct platform, builds the module and runs the test suite:

```powershell
.\build.ps1 -Configuration Release
```

The resulting files are written to `build\Release`.

No external libraries need to be downloaded. PNG decoding uses Windows Imaging Component, rendering uses Direct3D 9, and transfer integrity uses the Windows cryptography API.

## Tests

The `tests` directory contains four automated test programs:

- `rgba_tests` checks RGBA PNG decoding, alpha collision rules and Direct3D rendering.
- `transfer_tests` checks map manifests, chunk transfer, integrity validation, stale data and corrupted packets.
- `loader_tests` checks native proxy generation, file interception, borders, holes, automatic-placement clearance and canonical terrain data.
- `replay_tests` checks RGBA replay payload creation, validation and recovery.

The tests are part of the default build and are executed by `build.ps1`. A Direct3D 9 device must be available for the rendering tests.

## Project layout

- `src/` — module, renderer, loader, networking and replay source code
- `tests/` — automated regression tests
- `CMakeLists.txt` — CMake build configuration
- `build.ps1` — Win32 build and test script
- `wkRGBA.dll` — current stable prebuilt module


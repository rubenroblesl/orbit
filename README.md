# Orbit

Orbit runs **Super Mario Galaxy** natively on Macs with Apple Silicon. It is the macOS port of
[Nebula](https://github.com/keepvaibin/nebula) by keepvaibin, which translates the game's
PowerPC machine code into C++ ahead of time. Orbit compiles that code on your Mac from your own
copy of the game and runs it with a Metal renderer and macOS audio and input.

Orbit contains no Nintendo code, game files or disc images. You need your own copy of the game.

[Download](../../releases/latest) · [Controls](docs/KEYBOARD_CONTROLS.md) ·
[Nebula's documentation](docs/NEBULA-README.md) · [Third-party notices](THIRD-PARTY-NOTICES.md)

Not affiliated with or endorsed by Nintendo.

## Project status

Orbit is a work in progress, like Nebula. The game is playable from the start on an M-series
Mac at 60 FPS. Some lighting effects differ from the original and bugs remain.

These features are implemented but have not been tested with the physical controllers yet:
real Wii Remotes over Bluetooth, the Switch Pro Controller and the Switch 2 Pro Controller.

## Features

- A Mac app with a setup assistant. It checks your copy of the game, installs Apple's command
  line tools if they are missing, compiles the game and applies recommended settings.
- Native macOS full screen in its own Space. The green title bar button or Control-Command-F
  switches between full screen and a window.
- The picture adapts to any display shape, including MacBook displays taller than 16:9, without
  stretched menus.
- Community HD textures and HD HUD included in the release, with preloading to avoid stutter.
  The texture loader reads packs in Dolphin's custom texture format.
- Keyboard and mouse, Xbox and PlayStation controllers, Switch Pro controllers and real
  Wii Remotes. Buttons can be remapped in the in-game menu (F1).
- Settings for resolution, aspect ratio, anti-aliasing, anisotropic filtering, draw distance,
  audio volumes and controls, in the app and in the in-game menu.
- Box art downloaded from GameTDB on your Mac, as Dolphin does.

The app's interface is in English or Spanish, following the language of the system.

## Requirements

- A Mac with Apple Silicon and macOS 14 or later.
- Super Mario Galaxy, USA (RMGE01), revision 0, as a WBFS, ISO, RVZ, WIA, CISO or GCZ image or
  an extracted folder.
- Apple's command line tools. The setup assistant installs them.
- 12 GB of free disk space during installation and about 3.5 GB afterwards.

## Install

1. Download `Orbit.dmg` from the [latest release](../../releases/latest) and open it.
2. Drag Orbit to the Applications folder and open it from there.
3. Orbit is not signed with an Apple developer certificate, so macOS blocks the first launch.
   Open System Settings, go to Privacy & Security and select **Open Anyway** next to the Orbit
   message. You only do this once.
4. Follow the setup assistant. Installation takes between 3 and 15 minutes, depending on the Mac.

The game is installed in `~/Library/Application Support/Orbit`. Saves are kept in
`~/Library/Application Support/SuperMarioGalaxy/nand` and are not removed when you reinstall or
repair the game.

## How it works

1. **Recompiler** (`crates/`, Rust, from Nebula). Reads the game disc, checks that it is the
   supported release and emits C++ for every function in the game executable, the HOME Menu
   module and the audio DSP program.
2. **Runtime** (`runtime/`, C++, from Nebula). Provides what the Wii hardware provided: memory,
   the GX graphics processor, audio, input, disc and NAND storage and video timing.
3. **macOS platform layer** (`runtime/platform/macos`). Implements the Windows APIs the runtime
   uses on top of AppKit, Core Audio, Game Controller and IOKit, and Direct3D 12 on top of
   Metal. HLSL shaders are compiled with glslang and translated to Metal with SPIRV-Cross.
4. **App** (`macos/`, Swift). The launcher, setup assistant and settings, and
   `macos/package.sh`, which builds `Orbit.app` and `Orbit.dmg`.

## Building from source

You need Xcode, Rust (`rustup`), CMake, Ninja and Microsoft's DirectX headers:

```sh
brew install cmake ninja directx-headers
cmake -S . -B ../build/runtime -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../build/runtime
ctest --test-dir ../build/runtime
macos/package.sh --textures <folder with the RMG texture pack>
```

`package.sh` builds the recompiler and the launcher, downloads the official CMake and Ninja
releases with pinned checksums, assembles `Orbit.app` and creates `Orbit.dmg` in `../dist`.
Omit `--textures` to build an app without the texture packs.

## Credits

Orbit exists thanks to these projects and people:

- **Nebula** by keepvaibin, <https://github.com/keepvaibin/nebula>. Orbit is a fork of Nebula.
  The recompiler, the runtime and the game support are Nebula's work.
- The projects Nebula builds on: Aurora, Dusklight, WiiCompiled, the Petari decompilation
  project, Berkeley SoftFloat, the Dolphin project's free DSP ROM, SMAA, AMD FidelityFX and the
  `nod` disc library. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- **HD textures** by CDAMJC and Razius, <https://forums.dolphin-emu.org/User-cdamjc>.
- **HD HUD** by Super Hackio and SY24, <https://www.youtube.com/watch?v=P8lkzqYP7eg>.
- **glslang, SPIRV-Tools and SPIRV-Cross** by The Khronos Group and contributors, for shader
  translation.
- **Dolphin**, whose custom texture format and GameTDB cover download Orbit follows.
- **GameTDB**, <https://www.gametdb.com>, for the box art.

The texture pack credits are also shipped in `Orbit.app/Contents/Resources/Licenses`.

## AI-assisted project

Like Nebula, the macOS port was built with AI coding agents (Anthropic's Claude), directed,
reviewed and tested by the project's author. The game's own logic is the original game code,
translated instruction by instruction by Nebula's recompiler.

## License

Orbit is licensed under the GNU General Public License v3.0 only ([LICENSE](LICENSE)), the same
license as Nebula. Third-party code and its licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and [docs/PROVENANCE.md](docs/PROVENANCE.md).
The HD texture packs belong to their authors and are not covered by this license.

"Super Mario Galaxy", "Wii" and "Nintendo" are trademarks of Nintendo.

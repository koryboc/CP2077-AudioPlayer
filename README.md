# AudioPlayer

Native **RED4ext** plugin for **Cyberpunk 2077** that exposes a minimal, FMOD-backed audio playback API to **REDscript** (and therefore to **CET**).

Scope is intentionally narrow: 2D streaming playback across a fixed pool of channels, nothing else. No REDscript UI, no 3D audio, no listener model.

Works with the latest **RED4ext**. All native functions are static members of the `AudioPlayer` class and therefore callable from REDscript and CET.

---

## Features

### 1) Channel pool
64 playback channels (ids `0..63`) plus a dedicated preview channel. Every call takes an explicit `channelID`; passing `-1` targets **all** channels at once (for `Stop` and `SetVolume`).

Playback is fully asynchronous:
- `Play` opens the file with `FMOD_NONBLOCKING` and returns immediately.
- The internal update loop promotes the pending load to a real channel once FMOD reports `FMOD_OPENSTATE_READY`.
- If a slot is already busy, its previous sound is stopped and released before the new load starts.

### 2) Song length probe
Opens the file as a blocking stream, reads its length in milliseconds, releases it. Used to decide whether a file is a valid, decodable audio source before committing to playback.

```julia
public native class AudioPlayer extends IScriptable {
    public static native func GetSongLength(path: String) -> Int32
}
```
- Returns the duration in **milliseconds**, or `0` if the file cannot be opened or decoded.
- Supported formats: `.mp3`, `.mp2`, `.flac`, `.ogg`, `.wav`, `.wax`, `.wma`, `.opus`, `.aiff`, `.aif`, `.aifc` (anything FMOD Core understands).

### 3) Playback control

```julia
    public static native func Play(channelID: Int32, path: String, startPos: Int32, volume: Float, fade: Float)
    public static native func Stop(channelID: Int32)
    public static native func SetVolume(channelID: Int32, volume: Float)
}
```

Semantics:
- **Play**:
  - `channelID`: target slot in `[0, 63]`, or `-1` for the preview channel.
  - `path`: absolute or CET-relative file path.
  - `startPos`: start offset in **milliseconds** (`0` = from start).
  - `volume`: linear gain, `1.0` = 100%.
  - `fade`: fade-in duration in **seconds** before reaching the target volume; `0` disables the fade.
- **Stop**:
  - Stops and releases the channel. Idempotent; calling it on an idle slot is a no-op.
  - `channelID = -1` stops **all** channels (including any pending async loads).
- **SetVolume**:
  - Sets linear gain on the channel. `channelID = -1` applies the volume to every channel.
  - Safe to call on an idle channel; has no effect.

REDscript examples:
```julia
let ms: Int32 = AudioPlayer.GetSongLength("plugins\\cyber_engine_tweaks\\mods\\Synthrace\\music\\#Default\\track.mp3");
AudioPlayer.Play(0, "plugins\\...\\track.mp3", 0, 0.7, 0.5);
AudioPlayer.SetVolume(-1, 0.0); // mute all
AudioPlayer.Stop(-1);           // stop all
```

CET (Lua) example:
```lua
local path = "plugins\\cyber_engine_tweaks\\mods\\MyMod\\track.mp3"
local length = AudioPlayer.GetSongLength(path)
if length > 0 then
    AudioPlayer.Play(-1, path, 0, 0.7, 0.5)
end
```

---

## Requirements

### For users
- [Cyberpunk 2077](https://www.gog.com/de/game/cyberpunk_2077) (version 2.31)
- [RED4ext](https://github.com/wopss/RED4ext) (loader)
- [FMOD Core runtime](https://www.fmod.com/download) — `fmod.dll` is shipped next to the plugin DLL

### For developers (to build from source)
- [CMake ≥ 3.20](https://cmake.org/cmake/help/v3.20/release/3.20.html)
- [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK) (bundled as submodule)
- [FMOD Studio API for Windows](https://www.fmod.com/download) (Core) — `api/core` with `inc/fmod.hpp` and `lib/x64/fmod_vc.lib` + `fmod.dll`
- **Windows**: [MSVC 2022](https://aka.ms/vc14/vc_redist.x64.exe) (or compatible)
- **Linux cross-compile**: `clang-cl`, `lld-link`, `llvm-lib`, `llvm-mt`, `llvm-rc`, `ninja`, plus [xwin](https://github.com/Jake-Shadle/xwin) SDK extracted to `~/.xwin` (`crt/`, `sdk/`)

---

## Installation (User)

1. Drop the built DLL (and the FMOD runtime) into:
   ```
   <Cyberpunk 2077>\red4ext\plugins\AudioPlayer\AudioPlayer.dll
   <Cyberpunk 2077>\red4ext\plugins\AudioPlayer\fmod.dll
   ```
2. Launch the game. Check logs at:
   ```
   <Cyberpunk 2077>\red4ext\logs\AudioPlayer-<timestamp>.log
   ```

---

## Build (Developer)

Project layout (relevant parts):
```
cp2077-audio-player/
  CMakeLists.txt
  build.sh                 (Linux cross-compile driver)
  xwin-toolchain.cmake     (clang-cl + lld-link + xwin paths)
  patch-submodule.sh       (RTTI case-sensitivity fix for RED4ext.SDK)
  deps/
    red4ext.sdk/           (RED4ext.SDK submodule)
    fmod/                  (FMOD Core API — place `api/core` here)
  src/red4ext/
    main.cpp               (plugin code)
    version.rc             (Windows version info)
```

### Setup
```bash
git submodule update --init --recursive
./patch-submodule.sh   # fix RTTI case-sensitivity in RED4ext.SDK
```

Provide FMOD — pick one:
- `cp -r /path/to/FMOD/api/core deps/fmod`
- or `export FMOD_DIR=/path/to/FMOD/api/core`
- or `cmake -DFMOD_DIR=... ...`

### Build (Linux cross-compile)
```bash
./build.sh                  # Release
BUILD_TYPE=Debug ./build.sh
./build.sh clean            # clean + rebuild
```

### Build (Windows)
```bash
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Artifact: `bin/red4ext/plugins/AudioPlayer/AudioPlayer.dll` (+ `fmod.dll` copied next to it).

---

## Linux cross-compile notes

Four hurdles solved:

1. **Case-sensitive filesystem**: RED4ext.SDK upstream includes `<RED4ext/rtti/...>` while the on-disk directory is `RTTI/`. `patch-submodule.sh` rewrites the lowercase paths in-tree.
2. **MSVC ABI**: `clang-cl` with `--target=x86_64-pc-windows-msvc` plus xwin CRT/SDK paths in `xwin-toolchain.cmake`.
3. **Windows macro collisions**: `NOMINMAX` is set globally; `min()`/`max()` macro calls from the original were rewritten to `std::min` / `std::max` with explicit types.
4. **FMOD runtime**: only a Windows prebuilt exists — referenced as an `IMPORTED` target (`fmod_vc.lib` at link time, `fmod.dll` copied next to the plugin DLL at runtime).

---

## Logging

The plugin uses the **RED4ext logger**. You'll find messages in:
```
<CP2077>\red4ext\logs\AudioPlayer-<timestamp>.log
```

Key entries:
- `AudioPlayer: FMOD initialized` — FMOD system created and ready.
- `AudioPlayer: FMOD init failed (...)` — `FMOD::System_Create` or `init` returned an error.
- `AudioPlayer: createStream failed (...)` — file not found, unsupported format, or decoder error.

---

## Troubleshooting

- **No sound / `Play` silently does nothing**
  - Verify the path. `Play` returns no value; check whether `GetSongLength(path) > 0` first.
- **Plugin fails to load**
  - Confirm `fmod.dll` sits next to `AudioPlayer.dll` inside `red4ext\plugins\AudioPlayer\`.
- **All channels unresponsive**
  - Check the log for `FMOD init failed`; a failed init disables every native call.

---

## Versioning

- Targets the **latest** RED4ext loader/SDK and current game build.
- The FMOD Core API is binary-stable across minor revisions; any recent 2.x runtime works.

---

## Credits

Huge thanks to [**keanuWheeze**](https://github.com/keanuWheeze) and the [CP77_radioExt](https://github.com/keanuWheeze/CP77_radioExt) project — a great help for understanding how to drive **FMOD Core** from a RED4ext plugin and a solid reference point while building this independent, pared-down base from scratch.

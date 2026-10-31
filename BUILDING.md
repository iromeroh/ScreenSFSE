# Building ScreenSFSE 0.3.2

## Native plugin

Requirements: Windows x64, Visual Studio 2022 with Desktop development with C++,
a Windows SDK with Direct3D 12 and Media Foundation headers, and CMake 3.21 or
later, plus Git. The project uses C++23 and uses the
Address Library, so it is not tied to one game version (developed on Starfield 1.16.244).

Download CommonLibSF and commonlib-shared from their upstream repositories using
the exact revisions used by this release. Run these commands from the repository
root before configuring CMake:

```bat
git clone https://github.com/libxse/commonlibsf.git external/commonlibsf
git -C external/commonlibsf checkout --detach 84f8589b15a6d588d28da16baa7459a1fc414ea7
git -C external/commonlibsf submodule update --init --recursive
git -C external/commonlibsf/lib/commonlib-shared fetch origin af93af74e9a572aa2af9b10ea9fdd73bf80b3c9c
git -C external/commonlibsf/lib/commonlib-shared checkout --detach af93af74e9a572aa2af9b10ea9fdd73bf80b3c9c
```

The commonlib-shared revision is intentionally different from the parent
repository's default submodule revision. Both dependencies are downloaded
separately and ignored by this repository.

From an **x64 Native Tools Command Prompt for VS 2022**, in the repository root:

```bat
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release --target ScreenSFSEPlugin
```

Output: `build-vs/Release/ScreenSFSE.dll`. The required SFSE API header is included.
See THIRD_PARTY_NOTICES.txt for dependency versions and licenses.

## Install the DLL

With the game closed, copy `build-vs/Release/ScreenSFSE.dll` to
`Starfield/Data/SFSE/Plugins/ScreenSFSE.dll`. Use it with the published version 0.3.2
ESM, textures, models, and scripts. `ScreenSFSE.ini.example` is the default
configuration; preserve existing user settings when replacing an installation.

## Papyrus sources

`scripts/` contains the Papyrus source. To recompile a script, use the Creation
Kit's `PapyrusCompiler.exe`, with `scripts/` and the extracted base-game script
sources as import directories and `Starfield_Papyrus_Flags.flg` as the flags file.
The game script sources and flags are supplied in the Creation Kit's
`Tools/ContentResources.zip`. Place compiled PEX files in `Data/Scripts/`.
Python is not needed for the native DLL build.

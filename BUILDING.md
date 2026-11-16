# Building ScreenSFSE 0.3.3

## Requirements

- Windows x64.
- Visual Studio 2022 with Desktop development with C++ and a Windows SDK.
- CMake 3.21 or later and Git.
- CommonLibSF and commonlib-shared at the revisions below.

The native plugin uses C++23, Direct3D 12, Media Foundation, XAudio2 and Windows
Imaging Component. Required SDK libraries, including `gdi32` and
`windowscodecs`, are linked by CMake. Python is not required.

## Dependencies

From the repository root:

```bat
git clone https://github.com/libxse/commonlibsf.git external/commonlibsf
git -C external/commonlibsf checkout --detach 84f8589b15a6d588d28da16baa7459a1fc414ea7
git -C external/commonlibsf submodule update --init --recursive
git -C external/commonlibsf/lib/commonlib-shared fetch origin af93af74e9a572aa2af9b10ea9fdd73bf80b3c9c
git -C external/commonlibsf/lib/commonlib-shared checkout --detach af93af74e9a572aa2af9b10ea9fdd73bf80b3c9c
```

Both revisions are required. The SFSE API header and its license are included
under `external/sfse`; no SFSE implementation build is needed.

## Compile

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** in the
repository root and run:

```bat
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release --target ScreenSFSEPlugin --parallel 4
```

Alternatively run `build_windows.cmd` from that prompt. Output:
`build-vs/Release/ScreenSFSE.dll`.

The source list includes screen location tracking, save synchronization,
subtitles and picture slideshows. CMake builds the native plugin only.

## Use the build

Install the ScreenSFSE 0.3.3 mod assets, SFSE, and Address Library matching
your game version. Close Starfield, then copy the DLL to
`Starfield/Data/SFSE/Plugins/ScreenSFSE.dll`. Preserve your existing INI;
`ScreenSFSE.ini.example` contains the default settings. Launch through SFSE.

yt-dlp, FFmpeg and Deno are runtime tools for online media, not build
dependencies. They are obtained separately.

## Papyrus

To compile `scripts/*.psc`, use the Creation Kit's `PapyrusCompiler.exe`,
the extracted base-game script sources, and `Starfield_Papyrus_Flags.flg`
from `Tools/ContentResources.zip`. Add this repository's `scripts` directory
and the base-game sources as import directories. Compiled PEX files belong
in `Data/Scripts` or the mod's Main BA2.

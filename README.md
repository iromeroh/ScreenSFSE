# ScreenSFSE - Video Screens

Copyright (C) 2026 comespm and the ScreenSFSE contributors.

Source code for ScreenSFSE **0.1**, a Starfield mod providing buildable video
screens with local video playback, YouTube playlists, live streams, positional
audio, and independent channels.

## License

ScreenSFSE is free software under the **GNU General Public License, version 3
or any later version**, with the additional permissions in [EXCEPTIONS](EXCEPTIONS)
for Starfield and SFSE. Modification and redistribution are permitted under these
terms. The software is provided **WITHOUT ANY WARRANTY**, including merchantability
or fitness for a particular purpose.

- [Full GPL license](LICENSE.txt)
- [Copyright and license notice](README.txt)
- [Third-party notices](THIRD_PARTY_NOTICES.txt)

These terms do not relicense Bethesda game assets or user-supplied videos.

## Build

See [BUILDING.md](BUILDING.md) for dependencies and native DLL build commands.
This folder contains the C++ and Papyrus sources for version 0.1, the minimal
CMake build, and license notices. CommonLibSF and commonlib-shared are obtained
separately using the pinned revisions in the build instructions.

Use the built DLL with the version 0.1 mod assets. Game plugin and asset-generation
tools are not included in this native-source distribution.
